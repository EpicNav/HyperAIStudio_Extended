// Games by Hyper 2026.

#include "HyperAIStudioPIEPlaytestToolset.h"

#include "Components/ActorComponent.h"
#include "Containers/Ticker.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformTime.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioLogTailToolset.h"
#include "HyperAIStudioNativeReadToolset.h"
#include "HyperAIStudioOperationJournal.h"
#include "HyperAIStudioPlanExecuteToolset.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreDelegates.h"
#include "Misc/DateTime.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Misc/SecureHash.h"
#include "Modules/ModuleManager.h"
#include "PlayInEditorDataTypes.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"
#include "UnrealClient.h"

#include <type_traits>

#include UE_INLINE_GENERATED_CPP_BY_NAME(HyperAIStudioPIEPlaytestToolset)

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioPIEPlaytest, Log, All);

namespace HyperAIStudio::PIEPlaytest::Private
{
	constexpr int32 MaxStoredRuns = 32;
	constexpr int32 MaxRunsPumpedPerTick = 4;
	constexpr int32 MaxDiagnosticCharacters = 512;
	constexpr int32 MaxPreviewCharacters = 1024;
	constexpr int32 MaxCursorCharacters = 192;
	constexpr int32 TeardownTimeoutMs = 10000;
	constexpr int32 WorldReadyTimeoutMs = 30000;
	constexpr int32 MaxObservablePieWorlds = 64;
	static_assert(WorldReadyTimeoutMs > 0
		&& WorldReadyTimeoutMs <= FHyperAIStudioPIEPlaytestContracts::MaxWholeTimeoutMs,
		"PIE world-ready timeout must remain positive and within the whole-plan bound.");

	FString Clip(const FString& Value, const int32 MaxCharacters)
	{
		return Value.Len() <= MaxCharacters ? Value : Value.Left(MaxCharacters);
	}

	bool HasWellFormedUtf16(const FString& Value)
	{
		for (int32 Index = 0; Index < Value.Len(); ++Index)
		{
			const TCHAR Character = Value[Index];
			if (Character >= 0xd800 && Character <= 0xdbff)
			{
				if (Index + 1 >= Value.Len()
					|| Value[Index + 1] < 0xdc00 || Value[Index + 1] > 0xdfff)
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

	void AppendCanonicalToken(FString& Canonical, const FString& Token)
	{
		Canonical += FString::FromInt(Token.Len());
		Canonical += TEXT(":");
		Canonical += Token;
	}

	FString NumberToken(const double Value)
	{
		return FString::Printf(TEXT("%.17g"), Value);
	}

	bool IsZeroTransformFields(const FHyperAIPIEPlaytestStep& Step)
	{
		return Step.Location.IsNearlyZero() && Step.Rotation.IsNearlyZero();
	}

	bool IsKnownMode(const EHyperAIPIEPlaytestMode Mode)
	{
		return Mode == EHyperAIPIEPlaytestMode::StartInProcess
			|| Mode == EHyperAIPIEPlaytestMode::AttachOnly;
	}

	bool IsKnownTeardown(const EHyperAIPIEPlaytestTeardown Teardown)
	{
		return Teardown == EHyperAIPIEPlaytestTeardown::StopOwnedSession
			|| Teardown == EHyperAIPIEPlaytestTeardown::LeaveSessionRunning;
	}

	bool IsKnownStepKind(const EHyperAIPIEPlaytestStepKind Kind)
	{
		return !FHyperAIStudioPIEPlaytestContracts::StepKindToString(Kind).IsEmpty();
	}

	bool IsKnownValueType(const EHyperAIPIEPlaytestValueType Type)
	{
		return Type >= EHyperAIPIEPlaytestValueType::None
			&& Type <= EHyperAIPIEPlaytestValueType::Vector;
	}

	bool IsKnownKey(const EHyperAIPIEPlaytestKey Key)
	{
		return Key >= EHyperAIPIEPlaytestKey::SpaceBar
			&& Key <= EHyperAIPIEPlaytestKey::RightMouseButton;
	}

	bool IsKnownKeyEvent(const EHyperAIPIEPlaytestKeyEvent Event)
	{
		return Event == EHyperAIPIEPlaytestKeyEvent::Pressed
			|| Event == EHyperAIPIEPlaytestKeyEvent::Released;
	}

	bool ValidateBoundedString(
		const FString& Value,
		const int32 MaxCharacters,
		const TCHAR* Field,
		FString& OutErrorCode,
		FString& OutError)
	{
		if (Value.Len() > MaxCharacters)
		{
			OutErrorCode = TEXT("limit_exceeded");
			OutError = FString::Printf(TEXT("%s exceeds its hard character bound."), Field);
			return false;
		}
		bool bHasEmbeddedNull = false;
		for (const TCHAR Character : Value)
		{
			bHasEmbeddedNull |= Character == TEXT('\0');
		}
		if (!HasWellFormedUtf16(Value) || bHasEmbeddedNull)
		{
			OutErrorCode = TEXT("invalid_text");
			OutError = FString::Printf(TEXT("%s contains malformed UTF-16 or an embedded null."), Field);
			return false;
		}
		return true;
	}

	bool ValidateTypedValue(
		const FHyperAIPIEPlaytestValue& Value,
		FString& OutErrorCode,
		FString& OutError)
	{
		if (!IsKnownValueType(Value.Type))
		{
			OutErrorCode = TEXT("invalid_enum");
			OutError = TEXT("value.type is outside the closed value enum.");
			return false;
		}
		if (!FMath::IsFinite(Value.Float) || Value.Vector.ContainsNaN())
		{
			OutErrorCode = TEXT("invalid_number");
			OutError = TEXT("Typed values must contain only finite numbers.");
			return false;
		}
		if (!ValidateBoundedString(
			Value.Name.ToString(),
			FHyperAIStudioPIEPlaytestContracts::MaxNameCharacters,
			TEXT("value.name"), OutErrorCode, OutError)
			|| !ValidateBoundedString(
				Value.String,
				FHyperAIStudioPIEPlaytestContracts::MaxValueCharacters,
				TEXT("value.string"), OutErrorCode, OutError))
		{
			return false;
		}
		const bool bBooleanPopulated = Value.bBoolean;
		const bool bIntegerPopulated = Value.Integer != 0;
		const bool bFloatPopulated = Value.Float != 0.0;
		const bool bNamePopulated = !Value.Name.IsNone();
		const bool bStringPopulated = !Value.String.IsEmpty();
		const bool bVectorPopulated = Value.Vector != FVector::ZeroVector;
		bool bHasIrrelevantUnionField = false;
		switch (Value.Type)
		{
		case EHyperAIPIEPlaytestValueType::None:
			bHasIrrelevantUnionField = bBooleanPopulated || bIntegerPopulated || bFloatPopulated
				|| bNamePopulated || bStringPopulated || bVectorPopulated;
			break;
		case EHyperAIPIEPlaytestValueType::Boolean:
			bHasIrrelevantUnionField = bIntegerPopulated || bFloatPopulated || bNamePopulated
				|| bStringPopulated || bVectorPopulated;
			break;
		case EHyperAIPIEPlaytestValueType::Integer:
			bHasIrrelevantUnionField = bBooleanPopulated || bFloatPopulated || bNamePopulated
				|| bStringPopulated || bVectorPopulated;
			break;
		case EHyperAIPIEPlaytestValueType::Float:
			bHasIrrelevantUnionField = bBooleanPopulated || bIntegerPopulated || bNamePopulated
				|| bStringPopulated || bVectorPopulated;
			break;
		case EHyperAIPIEPlaytestValueType::Name:
			bHasIrrelevantUnionField = bBooleanPopulated || bIntegerPopulated || bFloatPopulated
				|| bStringPopulated || bVectorPopulated;
			break;
		case EHyperAIPIEPlaytestValueType::String:
			bHasIrrelevantUnionField = bBooleanPopulated || bIntegerPopulated || bFloatPopulated
				|| bNamePopulated || bVectorPopulated;
			break;
		case EHyperAIPIEPlaytestValueType::Vector:
			bHasIrrelevantUnionField = bBooleanPopulated || bIntegerPopulated || bFloatPopulated
				|| bNamePopulated || bStringPopulated;
			break;
		default:
			break;
		}
		if (bHasIrrelevantUnionField)
		{
			OutErrorCode = TEXT("irrelevant_field");
			OutError = TEXT("Typed value contains a populated field outside its selected closed union variant.");
			return false;
		}
		return true;
	}

	void AppendValueCanonical(FString& Canonical, const FHyperAIPIEPlaytestValue& Value)
	{
		AppendCanonicalToken(Canonical, FString::FromInt(static_cast<int32>(Value.Type)));
		switch (Value.Type)
		{
		case EHyperAIPIEPlaytestValueType::Boolean:
			AppendCanonicalToken(Canonical, Value.bBoolean ? TEXT("1") : TEXT("0"));
			break;
		case EHyperAIPIEPlaytestValueType::Integer:
			AppendCanonicalToken(Canonical, FString::FromInt(Value.Integer));
			break;
		case EHyperAIPIEPlaytestValueType::Float:
			AppendCanonicalToken(Canonical, NumberToken(Value.Float));
			break;
		case EHyperAIPIEPlaytestValueType::Name:
			AppendCanonicalToken(Canonical, Value.Name.ToString());
			break;
		case EHyperAIPIEPlaytestValueType::String:
			AppendCanonicalToken(Canonical, Value.String);
			break;
		case EHyperAIPIEPlaytestValueType::Vector:
			AppendCanonicalToken(Canonical, NumberToken(Value.Vector.X));
			AppendCanonicalToken(Canonical, NumberToken(Value.Vector.Y));
			AppendCanonicalToken(Canonical, NumberToken(Value.Vector.Z));
			break;
		default:
			AppendCanonicalToken(Canonical, TEXT("none"));
			break;
		}
	}

	bool RequiresTarget(const EHyperAIPIEPlaytestStepKind Kind)
	{
		switch (Kind)
		{
		case EHyperAIPIEPlaytestStepKind::QueryComponents:
		case EHyperAIPIEPlaytestStepKind::QueryScalarProperty:
		case EHyperAIPIEPlaytestStepKind::TeleportActor:
		case EHyperAIPIEPlaytestStepKind::DestroyAllowlistedActor:
		case EHyperAIPIEPlaytestStepKind::AIMoveTo:
		case EHyperAIPIEPlaytestStepKind::AIStop:
		case EHyperAIPIEPlaytestStepKind::BlackboardRead:
		case EHyperAIPIEPlaytestStepKind::BlackboardWrite:
		case EHyperAIPIEPlaytestStepKind::AssertActorExists:
		case EHyperAIPIEPlaytestStepKind::AssertScalarEquals:
			return true;
		default:
			return false;
		}
	}

	bool AllowsTarget(const EHyperAIPIEPlaytestStepKind Kind)
	{
		return RequiresTarget(Kind)
			|| Kind == EHyperAIPIEPlaytestStepKind::InjectEnhancedAction
			|| Kind == EHyperAIPIEPlaytestStepKind::InjectAllowlistedKey;
	}

	bool RequiresName(const EHyperAIPIEPlaytestStepKind Kind)
	{
		return Kind == EHyperAIPIEPlaytestStepKind::QueryScalarProperty
			|| Kind == EHyperAIPIEPlaytestStepKind::BlackboardRead
			|| Kind == EHyperAIPIEPlaytestStepKind::BlackboardWrite
			|| Kind == EHyperAIPIEPlaytestStepKind::LogMarker
			|| Kind == EHyperAIPIEPlaytestStepKind::AssertScalarEquals;
	}

	bool RequiresValue(const EHyperAIPIEPlaytestStepKind Kind)
	{
		return Kind == EHyperAIPIEPlaytestStepKind::BlackboardWrite
			|| Kind == EHyperAIPIEPlaytestStepKind::InjectEnhancedAction
			|| Kind == EHyperAIPIEPlaytestStepKind::AssertScalarEquals;
	}

	bool AllowsTransform(const EHyperAIPIEPlaytestStepKind Kind)
	{
		return Kind == EHyperAIPIEPlaytestStepKind::TeleportActor
			|| Kind == EHyperAIPIEPlaytestStepKind::SpawnAllowlistedActor
			|| Kind == EHyperAIPIEPlaytestStepKind::AIMoveTo;
	}

	bool ValidateStepShape(
		const FHyperAIPIEPlaytestStep& Step,
		FString& OutErrorCode,
		FString& OutError)
	{
		if (!IsKnownStepKind(Step.Kind))
		{
			OutErrorCode = TEXT("invalid_enum");
			OutError = TEXT("step.kind is outside the closed step enum.");
			return false;
		}
		if (Step.StepId.IsEmpty()
			|| !ValidateBoundedString(Step.StepId,
				FHyperAIStudioPIEPlaytestContracts::MaxStepIdCharacters,
				TEXT("step.step_id"), OutErrorCode, OutError))
		{
			if (OutError.IsEmpty())
			{
				OutErrorCode = TEXT("missing_step_id");
				OutError = TEXT("Every step requires a non-empty step_id.");
			}
			return false;
		}
		if (!ValidateBoundedString(Step.TargetObjectPath,
			FHyperAIStudioPIEPlaytestContracts::MaxObjectPathCharacters,
			TEXT("step.target_object_path"), OutErrorCode, OutError)
			|| !ValidateBoundedString(Step.SecondaryObjectPath,
				FHyperAIStudioPIEPlaytestContracts::MaxObjectPathCharacters,
				TEXT("step.secondary_object_path"), OutErrorCode, OutError)
			|| !ValidateBoundedString(Step.ClassId,
				FHyperAIStudioPIEPlaytestContracts::MaxClassIdCharacters,
				TEXT("step.class_id"), OutErrorCode, OutError)
			|| !ValidateBoundedString(Step.Name,
				Step.Kind == EHyperAIPIEPlaytestStepKind::LogMarker
					? FHyperAIStudioPIEPlaytestContracts::MaxMarkerCharacters
					: FHyperAIStudioPIEPlaytestContracts::MaxNameCharacters,
				TEXT("step.name"), OutErrorCode, OutError)
			|| !ValidateTypedValue(Step.Value, OutErrorCode, OutError))
		{
			return false;
		}
		if (Step.TimeoutMs < FHyperAIStudioPIEPlaytestContracts::MinStepTimeoutMs
			|| Step.TimeoutMs > FHyperAIStudioPIEPlaytestContracts::MaxStepTimeoutMs)
		{
			OutErrorCode = TEXT("invalid_step_timeout");
			OutError = TEXT("step.timeout_ms is outside the hard 100..30000 range.");
			return false;
		}
		if (Step.MaxItems < 1 || Step.MaxItems > FHyperAIStudioPIEPlaytestContracts::MaxActorResults)
		{
			OutErrorCode = TEXT("invalid_item_limit");
			OutError = TEXT("step.max_items must be between 1 and 128.");
			return false;
		}
		if (Step.PlayerIndex < 0 || Step.PlayerIndex > 7)
		{
			OutErrorCode = TEXT("invalid_player_index");
			OutError = TEXT("step.player_index must be between 0 and 7.");
			return false;
		}
		if (Step.Location.ContainsNaN() || Step.Rotation.ContainsNaN())
		{
			OutErrorCode = TEXT("invalid_transform");
			OutError = TEXT("Step transforms must contain only finite numbers.");
			return false;
		}
		if (RequiresTarget(Step.Kind) && Step.TargetObjectPath.IsEmpty())
		{
			OutErrorCode = TEXT("missing_target");
			OutError = TEXT("This step kind requires an exact loaded target_object_path.");
			return false;
		}
		if (!AllowsTarget(Step.Kind) && !Step.TargetObjectPath.IsEmpty())
		{
			OutErrorCode = TEXT("irrelevant_field");
			OutError = TEXT("target_object_path is not accepted by this step kind.");
			return false;
		}
		if (Step.Kind == EHyperAIPIEPlaytestStepKind::InjectEnhancedAction)
		{
			if (Step.SecondaryObjectPath.IsEmpty())
			{
				OutErrorCode = TEXT("missing_input_action");
				OutError = TEXT("InjectEnhancedAction requires an exact already-loaded InputAction path.");
				return false;
			}
		}
		else if (!Step.SecondaryObjectPath.IsEmpty())
		{
			OutErrorCode = TEXT("irrelevant_field");
			OutError = TEXT("secondary_object_path is accepted only by InjectEnhancedAction.");
			return false;
		}
		if (Step.Kind == EHyperAIPIEPlaytestStepKind::SpawnAllowlistedActor)
		{
			if (Step.ClassId.IsEmpty())
			{
				OutErrorCode = TEXT("missing_class_id");
				OutError = TEXT("SpawnAllowlistedActor requires an opaque server class_id.");
				return false;
			}
		}
		else if (!Step.ClassId.IsEmpty())
		{
			OutErrorCode = TEXT("irrelevant_field");
			OutError = TEXT("class_id is accepted only by SpawnAllowlistedActor.");
			return false;
		}
		if (RequiresName(Step.Kind) && Step.Name.IsEmpty())
		{
			OutErrorCode = TEXT("missing_name");
			OutError = TEXT("This step kind requires a bounded property/key/marker name.");
			return false;
		}
		if (!RequiresName(Step.Kind) && !Step.Name.IsEmpty())
		{
			OutErrorCode = TEXT("irrelevant_field");
			OutError = TEXT("name is not accepted by this step kind.");
			return false;
		}
		if (RequiresValue(Step.Kind) && Step.Value.Type == EHyperAIPIEPlaytestValueType::None)
		{
			OutErrorCode = TEXT("missing_value");
			OutError = TEXT("This step kind requires a typed value.");
			return false;
		}
		if (!RequiresValue(Step.Kind) && Step.Value.Type != EHyperAIPIEPlaytestValueType::None)
		{
			OutErrorCode = TEXT("irrelevant_field");
			OutError = TEXT("value is not accepted by this step kind.");
			return false;
		}
		if (!AllowsTransform(Step.Kind) && !IsZeroTransformFields(Step))
		{
			OutErrorCode = TEXT("irrelevant_field");
			OutError = TEXT("location/rotation are not accepted by this step kind.");
			return false;
		}
		if (!IsKnownKey(Step.Key) || !IsKnownKeyEvent(Step.KeyEvent))
		{
			OutErrorCode = TEXT("invalid_input_enum");
			OutError = TEXT("key/key_event must be members of the closed input allowlist.");
			return false;
		}
		if (Step.Kind != EHyperAIPIEPlaytestStepKind::InjectAllowlistedKey
			&& (Step.Key != EHyperAIPIEPlaytestKey::SpaceBar
				|| Step.KeyEvent != EHyperAIPIEPlaytestKeyEvent::Pressed))
		{
			OutErrorCode = TEXT("irrelevant_field");
			OutError = TEXT("key/key_event are accepted only by InjectAllowlistedKey.");
			return false;
		}
		const bool bInputStep = Step.Kind == EHyperAIPIEPlaytestStepKind::InjectEnhancedAction
			|| Step.Kind == EHyperAIPIEPlaytestStepKind::InjectAllowlistedKey;
		if (!bInputStep && Step.PlayerIndex != 0)
		{
			OutErrorCode = TEXT("irrelevant_field");
			OutError = TEXT("player_index is accepted only by typed input injection steps.");
			return false;
		}
		const bool bBoundedQuery = Step.Kind == EHyperAIPIEPlaytestStepKind::QueryActors
			|| Step.Kind == EHyperAIPIEPlaytestStepKind::QueryComponents;
		if (!bBoundedQuery && Step.MaxItems != 32)
		{
			OutErrorCode = TEXT("irrelevant_field");
			OutError = TEXT("max_items is accepted only by bounded actor/component queries.");
			return false;
		}
		return true;
	}

	FString BuildPlanCanonical(const FHyperAIPIEPlaytestRequest& Request, const bool bEffectsOnly)
	{
		FString Canonical;
		Canonical.Reserve(8192);
		AppendCanonicalToken(Canonical, bEffectsOnly
			? TEXT("hyperai.playtest-effects.v1")
			: TEXT("hyperai.playtest-plan.v1"));
		AppendCanonicalToken(Canonical, FString::FromInt(static_cast<int32>(Request.Mode)));
		AppendCanonicalToken(Canonical, FString::FromInt(static_cast<int32>(Request.Teardown)));
		AppendCanonicalToken(Canonical, FString::FromInt(Request.PieInstance));
		if (!bEffectsOnly)
		{
			AppendCanonicalToken(Canonical, FString::FromInt(Request.WholeTimeoutMs));
			AppendCanonicalToken(Canonical, FString::FromInt(Request.MaxEvidenceItems));
			AppendCanonicalToken(Canonical, FString::FromInt(Request.MaxOutputBytes));
		}
		for (const FHyperAIPIEPlaytestStep& Step : Request.Steps)
		{
			if (bEffectsOnly && !FHyperAIStudioPIEPlaytestContracts::IsEffectStep(Step.Kind))
			{
				continue;
			}
			AppendCanonicalToken(Canonical, Step.StepId);
			AppendCanonicalToken(Canonical, FHyperAIStudioPIEPlaytestContracts::StepKindToString(Step.Kind));
			AppendCanonicalToken(Canonical, Step.TargetObjectPath);
			AppendCanonicalToken(Canonical, Step.SecondaryObjectPath);
			AppendCanonicalToken(Canonical, Step.ClassId);
			AppendCanonicalToken(Canonical, Step.Name);
			AppendValueCanonical(Canonical, Step.Value);
			AppendCanonicalToken(Canonical, NumberToken(Step.Location.X));
			AppendCanonicalToken(Canonical, NumberToken(Step.Location.Y));
			AppendCanonicalToken(Canonical, NumberToken(Step.Location.Z));
			AppendCanonicalToken(Canonical, NumberToken(Step.Rotation.Pitch));
			AppendCanonicalToken(Canonical, NumberToken(Step.Rotation.Yaw));
			AppendCanonicalToken(Canonical, NumberToken(Step.Rotation.Roll));
			AppendCanonicalToken(Canonical, FString::FromInt(static_cast<int32>(Step.Key)));
			AppendCanonicalToken(Canonical, FString::FromInt(static_cast<int32>(Step.KeyEvent)));
			AppendCanonicalToken(Canonical, FString::FromInt(Step.PlayerIndex));
			AppendCanonicalToken(Canonical, FString::FromInt(Step.MaxItems));
			AppendCanonicalToken(Canonical, FString::FromInt(Step.TimeoutMs));
		}
		return Canonical;
	}

	FString BuildCapabilityHash()
	{
		FString Canonical;
		AppendCanonicalToken(Canonical, TEXT("hyperai.pie-runtime-capability.v1"));
		for (int32 Value = static_cast<int32>(EHyperAIPIEPlaytestStepKind::WaitForWorld);
			Value <= static_cast<int32>(EHyperAIPIEPlaytestStepKind::StopSession); ++Value)
		{
			AppendCanonicalToken(Canonical,
				FHyperAIStudioPIEPlaytestContracts::StepKindToString(
					static_cast<EHyperAIPIEPlaytestStepKind>(Value)));
		}
		return FHyperAIStudioPlanExecuteContracts::ComputeBoundedSha256(Canonical);
	}

	EHyperAIStudioPIEOptionalVariant VariantForStep(const EHyperAIPIEPlaytestStepKind Kind)
	{
		switch (Kind)
		{
		case EHyperAIPIEPlaytestStepKind::AIMoveTo:
		case EHyperAIPIEPlaytestStepKind::AIStop:
		case EHyperAIPIEPlaytestStepKind::BlackboardRead:
		case EHyperAIPIEPlaytestStepKind::BlackboardWrite:
			return EHyperAIStudioPIEOptionalVariant::GameplayAI;
		case EHyperAIPIEPlaytestStepKind::InjectEnhancedAction:
		case EHyperAIPIEPlaytestStepKind::InjectAllowlistedKey:
			return EHyperAIStudioPIEOptionalVariant::EnhancedInput;
		default:
			return EHyperAIStudioPIEOptionalVariant::Replay;
		}
	}

	bool IsOptionalStep(const EHyperAIPIEPlaytestStepKind Kind)
	{
		return Kind == EHyperAIPIEPlaytestStepKind::AIMoveTo
			|| Kind == EHyperAIPIEPlaytestStepKind::AIStop
			|| Kind == EHyperAIPIEPlaytestStepKind::BlackboardRead
			|| Kind == EHyperAIPIEPlaytestStepKind::BlackboardWrite
			|| Kind == EHyperAIPIEPlaytestStepKind::InjectEnhancedAction
			|| Kind == EHyperAIPIEPlaytestStepKind::InjectAllowlistedKey
			|| Kind == EHyperAIPIEPlaytestStepKind::ReplayCaptureStart
			|| Kind == EHyperAIPIEPlaytestStepKind::ReplayCaptureStop
			|| Kind == EHyperAIPIEPlaytestStepKind::ReplayPlaybackCaptured;
	}

	struct FOptionalAdapterState
	{
		FCriticalSection Mutex;
		TMap<EHyperAIStudioPIEOptionalVariant, TSharedPtr<IHyperAIStudioPIEOptionalVariantAdapter>> Adapters;
	};

	FOptionalAdapterState& GetOptionalAdapterState()
	{
		static FOptionalAdapterState State;
		return State;
	}
}

const TArray<FHyperAIStudioPIEPlaytestManifestEntry>& FHyperAIStudioPIEPlaytestContracts::GetManifest()
{
	static const TArray<FHyperAIStudioPIEPlaytestManifestEntry> Manifest = {
		{ TEXT("hyper_playtest_run"), GetQualifiedToolsetName() },
		{ TEXT("hyper_playtest_status"), GetQualifiedToolsetName() }
	};
	return Manifest;
}

FString FHyperAIStudioPIEPlaytestContracts::GetQualifiedToolsetName()
{
	return TEXT("HyperAIStudio.HyperAIStudioPIEPlaytestToolset");
}

bool FHyperAIStudioPIEPlaytestContracts::IsPendingNativeToolsTestEnabled()
{
	return FHyperAIStudioExtensionRuntime::AreSourceCandidateToolsEnabled();
}

bool FHyperAIStudioPIEPlaytestContracts::EvaluateCatalogAdmission(
	const FHyperAIStudioCapabilityCatalog& Catalog,
	const bool bAllowSourceCandidateForDev,
	FString& OutError)
{
	OutError.Reset();
	TOptional<EHyperAIStudioCapabilityAdmissionState> CohortState;
	FString CohortId;
	for (const FHyperAIStudioPIEPlaytestManifestEntry& Entry : GetManifest())
	{
		int32 MatchCount = 0;
		for (const FHyperAIStudioCapabilityToolDefinition& Tool : Catalog.Tools)
		{
			if (Tool.Name != Entry.Name)
			{
				continue;
			}
			++MatchCount;
			if (Tool.PackId != TEXT("pie_runtime"))
			{
				OutError = TEXT("A PIE playtest tool is bound to the wrong generated pack id.");
				return false;
			}
			if (Tool.AtomicCohortId.IsEmpty())
			{
				OutError = TEXT("The generated PIE playtest atomic cohort id is empty.");
				return false;
			}
			if (Tool.SourceArtifactCount <= 0 || Tool.SourceArtifactFingerprint.IsEmpty())
			{
				OutError = TEXT("The generated catalog does not bind this tool to checked source artifacts.");
				return false;
			}
			if (!CohortState.IsSet())
			{
				CohortState = Tool.AdmissionState;
				CohortId = Tool.AtomicCohortId;
			}
			else if (CohortState.GetValue() != Tool.AdmissionState || CohortId != Tool.AtomicCohortId)
			{
				OutError = TEXT("hyper_playtest_run and hyper_playtest_status are not one atomic generated cohort.");
				return false;
			}
		}
		if (MatchCount != 1)
		{
			OutError = FString::Printf(TEXT("Generated catalog must contain exactly one %s entry."), *Entry.Name);
			return false;
		}
	}
	if (!CohortState.IsSet())
	{
		OutError = TEXT("PIE playtest cohort state is unavailable.");
		return false;
	}
	return CohortState.GetValue() == EHyperAIStudioCapabilityAdmissionState::Admitted
		|| (bAllowSourceCandidateForDev
			&& CohortState.GetValue() == EHyperAIStudioCapabilityAdmissionState::SourceCandidate);
}

bool FHyperAIStudioPIEPlaytestContracts::IsRegistrationAllowed(const bool bAllowSourceCandidateForDev)
{
	TArray<FString> CatalogErrors;
	if (!FHyperAIStudioCapabilityPackRegistry::ValidateBuiltInCatalog(CatalogErrors))
	{
		return false;
	}
	FString Error;
	return EvaluateCatalogAdmission(
		FHyperAIStudioCapabilityPackRegistry::GetCatalog(),
		bAllowSourceCandidateForDev,
		Error);
}

bool FHyperAIStudioPIEPlaytestContracts::NormalizeRequest(
	const FHyperAIPIEPlaytestRequest& Request,
	FHyperAIStudioPIENormalizedPlan& OutPlan,
	FString& OutErrorCode,
	FString& OutError)
{
	using namespace HyperAIStudio::PIEPlaytest::Private;
	OutPlan = {};
	OutErrorCode.Reset();
	OutError.Reset();
	if (!IsKnownMode(Request.Mode) || !IsKnownTeardown(Request.Teardown))
	{
		OutErrorCode = TEXT("invalid_enum");
		OutError = TEXT("mode/teardown is outside the closed enum.");
		return false;
	}
	if (Request.Mode == EHyperAIPIEPlaytestMode::StartInProcess
		&& Request.Teardown != EHyperAIPIEPlaytestTeardown::StopOwnedSession)
	{
		OutErrorCode = TEXT("unsafe_teardown");
		OutError = TEXT("A run that starts PIE must stop its owned session deterministically.");
		return false;
	}
	if (!FHyperAIStudioOperationJournal::IsValidOperationId(Request.OperationId))
	{
		OutErrorCode = TEXT("invalid_operation_id");
		OutError = TEXT("operation_id does not satisfy the shared journal-v3 identifier contract.");
		return false;
	}
	if (Request.PieInstance < INDEX_NONE || Request.PieInstance > 64)
	{
		OutErrorCode = TEXT("invalid_pie_instance");
		OutError = TEXT("pie_instance must be -1 or between 0 and 64.");
		return false;
	}
	if (Request.Steps.Num() > MaxSteps)
	{
		OutErrorCode = TEXT("step_limit_exceeded");
		OutError = TEXT("The playtest plan exceeds the hard 32-step limit.");
		return false;
	}
	if (Request.WholeTimeoutMs < MinWholeTimeoutMs || Request.WholeTimeoutMs > MaxWholeTimeoutMs)
	{
		OutErrorCode = TEXT("invalid_whole_timeout");
		OutError = TEXT("whole_timeout_ms must be between 1000 and 300000.");
		return false;
	}
	if (Request.MaxEvidenceItems < 1 || Request.MaxEvidenceItems > MaxEvidenceItems
		|| Request.MaxOutputBytes < 8192 || Request.MaxOutputBytes > MaxOutputBytes)
	{
		OutErrorCode = TEXT("invalid_output_budget");
		OutError = TEXT("Evidence/output budgets are outside their hard ranges.");
		return false;
	}
	if (!ValidateBoundedString(Request.AuthorizationToken, MaxAuthorizationCharacters,
		TEXT("authorization_token"), OutErrorCode, OutError))
	{
		return false;
	}
	TSet<FString> StepIds;
	for (int32 StepIndex = 0; StepIndex < Request.Steps.Num(); ++StepIndex)
	{
		const FHyperAIPIEPlaytestStep& Step = Request.Steps[StepIndex];
		if (!ValidateStepShape(Step, OutErrorCode, OutError))
		{
			return false;
		}
		if (StepIds.Contains(Step.StepId))
		{
			OutErrorCode = TEXT("duplicate_step_id");
			OutError = TEXT("step_id values must be unique within one plan.");
			return false;
		}
		StepIds.Add(Step.StepId);
		if (Step.Kind == EHyperAIPIEPlaytestStepKind::StopSession
			&& StepIndex != Request.Steps.Num() - 1)
		{
			OutErrorCode = TEXT("invalid_step_order");
			OutError = TEXT("StopSession is terminal within a playtest plan and must be the final typed step.");
			return false;
		}
		OutPlan.bHasEffects |= IsEffectStep(Step.Kind);
		OutPlan.bHasDestructive |= IsDestructiveStep(Step.Kind);
	}
	OutPlan.bHasEffects |= Request.Mode == EHyperAIPIEPlaytestMode::StartInProcess;
	if (OutPlan.bHasEffects && Request.AuthorizationToken.IsEmpty())
	{
		OutErrorCode = TEXT("authorization_required");
		OutError = TEXT("Runtime mutation/external effects require an opaque exact-plan authorization token.");
		return false;
	}
	OutPlan.Request = Request;
	OutPlan.PlanHash = FHyperAIStudioPlanExecuteContracts::ComputeBoundedSha256(
		BuildPlanCanonical(Request, false));
	OutPlan.AuthorizationPlanHash = OutPlan.PlanHash;
	OutPlan.EffectFingerprint = FHyperAIStudioPlanExecuteContracts::ComputeBoundedSha256(
		BuildPlanCanonical(Request, true));
	OutPlan.CapabilityHash = BuildCapabilityHash();
	if (OutPlan.PlanHash.IsEmpty() || OutPlan.EffectFingerprint.IsEmpty() || OutPlan.CapabilityHash.IsEmpty())
	{
		OutErrorCode = TEXT("hash_failed");
		OutError = TEXT("The bounded shared SHA-256 contract rejected the canonical plan.");
		return false;
	}
	return true;
}

bool FHyperAIStudioPIEPlaytestContracts::IsEffectStep(const EHyperAIPIEPlaytestStepKind Kind)
{
	switch (Kind)
	{
	case EHyperAIPIEPlaytestStepKind::TeleportActor:
	case EHyperAIPIEPlaytestStepKind::SpawnAllowlistedActor:
	case EHyperAIPIEPlaytestStepKind::DestroyAllowlistedActor:
	case EHyperAIPIEPlaytestStepKind::AIMoveTo:
	case EHyperAIPIEPlaytestStepKind::AIStop:
	case EHyperAIPIEPlaytestStepKind::BlackboardWrite:
	case EHyperAIPIEPlaytestStepKind::InjectEnhancedAction:
	case EHyperAIPIEPlaytestStepKind::InjectAllowlistedKey:
	case EHyperAIPIEPlaytestStepKind::LogMarker:
	case EHyperAIPIEPlaytestStepKind::PauseSession:
	case EHyperAIPIEPlaytestStepKind::ResumeSession:
	case EHyperAIPIEPlaytestStepKind::ReplayCaptureStart:
	case EHyperAIPIEPlaytestStepKind::ReplayCaptureStop:
	case EHyperAIPIEPlaytestStepKind::ReplayPlaybackCaptured:
	case EHyperAIPIEPlaytestStepKind::StopSession:
		return true;
	default:
		return false;
	}
}

bool FHyperAIStudioPIEPlaytestContracts::IsDestructiveStep(const EHyperAIPIEPlaytestStepKind Kind)
{
	return Kind == EHyperAIPIEPlaytestStepKind::DestroyAllowlistedActor;
}

FString FHyperAIStudioPIEPlaytestContracts::StepKindToString(const EHyperAIPIEPlaytestStepKind Kind)
{
	switch (Kind)
	{
	case EHyperAIPIEPlaytestStepKind::WaitForWorld: return TEXT("wait_for_world");
	case EHyperAIPIEPlaytestStepKind::QueryActors: return TEXT("query_actors");
	case EHyperAIPIEPlaytestStepKind::QueryComponents: return TEXT("query_components");
	case EHyperAIPIEPlaytestStepKind::QueryScalarProperty: return TEXT("query_scalar_property");
	case EHyperAIPIEPlaytestStepKind::TeleportActor: return TEXT("teleport_actor");
	case EHyperAIPIEPlaytestStepKind::SpawnAllowlistedActor: return TEXT("spawn_allowlisted_actor");
	case EHyperAIPIEPlaytestStepKind::DestroyAllowlistedActor: return TEXT("destroy_allowlisted_actor");
	case EHyperAIPIEPlaytestStepKind::AIMoveTo: return TEXT("ai_move_to");
	case EHyperAIPIEPlaytestStepKind::AIStop: return TEXT("ai_stop");
	case EHyperAIPIEPlaytestStepKind::BlackboardRead: return TEXT("blackboard_read");
	case EHyperAIPIEPlaytestStepKind::BlackboardWrite: return TEXT("blackboard_write");
	case EHyperAIPIEPlaytestStepKind::InjectEnhancedAction: return TEXT("inject_enhanced_action");
	case EHyperAIPIEPlaytestStepKind::InjectAllowlistedKey: return TEXT("inject_allowlisted_key");
	case EHyperAIPIEPlaytestStepKind::CaptureScreenshotHash: return TEXT("capture_screenshot_hash");
	case EHyperAIPIEPlaytestStepKind::LogMarker: return TEXT("log_marker");
	case EHyperAIPIEPlaytestStepKind::AssertActorExists: return TEXT("assert_actor_exists");
	case EHyperAIPIEPlaytestStepKind::AssertScalarEquals: return TEXT("assert_scalar_equals");
	case EHyperAIPIEPlaytestStepKind::PauseSession: return TEXT("pause_session");
	case EHyperAIPIEPlaytestStepKind::ResumeSession: return TEXT("resume_session");
	case EHyperAIPIEPlaytestStepKind::ReplayCaptureStart: return TEXT("replay_capture_start");
	case EHyperAIPIEPlaytestStepKind::ReplayCaptureStop: return TEXT("replay_capture_stop");
	case EHyperAIPIEPlaytestStepKind::ReplayPlaybackCaptured: return TEXT("replay_playback_captured");
	case EHyperAIPIEPlaytestStepKind::StopSession: return TEXT("stop_session");
	default: return FString();
	}
}

FString FHyperAIStudioPIEPlaytestContracts::RuntimeStateToString(const EHyperAIStudioPIERuntimeState State)
{
	switch (State)
	{
	case EHyperAIStudioPIERuntimeState::Idle: return TEXT("idle");
	case EHyperAIStudioPIERuntimeState::Accepted: return TEXT("accepted");
	case EHyperAIStudioPIERuntimeState::StartingSession: return TEXT("starting_session");
	case EHyperAIStudioPIERuntimeState::WaitingForWorld: return TEXT("waiting_for_world");
	case EHyperAIStudioPIERuntimeState::ExecutingStep: return TEXT("executing_step");
	case EHyperAIStudioPIERuntimeState::Teardown: return TEXT("teardown");
	case EHyperAIStudioPIERuntimeState::Completed: return TEXT("completed");
	case EHyperAIStudioPIERuntimeState::Failed: return TEXT("failed");
	case EHyperAIStudioPIERuntimeState::Partial: return TEXT("partial");
	case EHyperAIStudioPIERuntimeState::RolledBack: return TEXT("rolled_back");
	case EHyperAIStudioPIERuntimeState::OutcomeUnknown: return TEXT("outcome_unknown");
	case EHyperAIStudioPIERuntimeState::ReplayCompleted: return TEXT("replay_completed");
	default: return TEXT("invalid");
	}
}

FString FHyperAIStudioPIEPlaytestContracts::ComputeEvidenceFingerprint(
	const TArray<FHyperAIPIEPlaytestEvidence>& Evidence)
{
	using namespace HyperAIStudio::PIEPlaytest::Private;
	FString Canonical;
	AppendCanonicalToken(Canonical, TEXT("hyperai.playtest-evidence.v1"));
	for (const FHyperAIPIEPlaytestEvidence& Item : Evidence)
	{
		AppendCanonicalToken(Canonical, FString::FromInt(Item.Sequence));
		AppendCanonicalToken(Canonical, Item.StepId);
		AppendCanonicalToken(Canonical, Item.Kind);
		AppendCanonicalToken(Canonical, Item.Status);
		AppendCanonicalToken(Canonical, Item.DiagnosticCode);
		AppendCanonicalToken(Canonical, Item.Diagnostic);
		AppendCanonicalToken(Canonical, Item.ObjectPath);
		AppendCanonicalToken(Canonical, Item.Name);
		AppendCanonicalToken(Canonical, Item.ValuePreview);
		AppendCanonicalToken(Canonical, FString::FromInt(Item.ItemCount));
		AppendCanonicalToken(Canonical, FString::FromInt(Item.ElapsedMs));
		AppendCanonicalToken(Canonical, Item.bEffect ? TEXT("1") : TEXT("0"));
		AppendCanonicalToken(Canonical, Item.bAssertion ? TEXT("1") : TEXT("0"));
		AppendCanonicalToken(Canonical, Item.bPassed ? TEXT("1") : TEXT("0"));
		AppendCanonicalToken(Canonical, Item.ScreenshotHash);
		AppendCanonicalToken(Canonical, FString::FromInt(Item.ScreenshotWidth));
		AppendCanonicalToken(Canonical, FString::FromInt(Item.ScreenshotHeight));
		AppendCanonicalToken(Canonical, FString::Printf(TEXT("%lld"), Item.LogSequenceBefore));
		AppendCanonicalToken(Canonical, FString::Printf(TEXT("%lld"), Item.LogSequenceAfter));
	}
	return FHyperAIStudioPlanExecuteContracts::ComputeBoundedSha256(Canonical);
}

FString FHyperAIStudioPIEPlaytestContracts::MakeCursor(
	const FString& EvidenceFingerprint,
	const int32 Offset)
{
	return FString::Printf(TEXT("v1|%s|%d"), *EvidenceFingerprint, Offset);
}

bool FHyperAIStudioPIEPlaytestContracts::ParseCursor(
	const FString& Cursor,
	const FString& EvidenceFingerprint,
	const int32 TotalItems,
	int32& OutOffset,
	FString& OutError)
{
	OutOffset = 0;
	OutError.Reset();
	if (Cursor.IsEmpty())
	{
		return true;
	}
	if (Cursor.Len() > HyperAIStudio::PIEPlaytest::Private::MaxCursorCharacters)
	{
		OutError = TEXT("Cursor exceeds its hard character bound.");
		return false;
	}
	TArray<FString> Parts;
	Cursor.ParseIntoArray(Parts, TEXT("|"), false);
	if (Parts.Num() != 3 || Parts[0] != TEXT("v1") || Parts[1] != EvidenceFingerprint
		|| !LexTryParseString(OutOffset, *Parts[2]) || OutOffset < 0 || OutOffset > TotalItems)
	{
		OutError = TEXT("Cursor is malformed, stale, or outside the immutable evidence snapshot.");
		OutOffset = 0;
		return false;
	}
	return true;
}

void FHyperAIStudioPIEOptionalVariantRegistry::SetAdapter(
	const EHyperAIStudioPIEOptionalVariant Variant,
	TSharedPtr<IHyperAIStudioPIEOptionalVariantAdapter> Adapter)
{
	auto& State = HyperAIStudio::PIEPlaytest::Private::GetOptionalAdapterState();
	FScopeLock Lock(&State.Mutex);
	if (Adapter)
	{
		State.Adapters.Add(Variant, MoveTemp(Adapter));
	}
	else
	{
		State.Adapters.Remove(Variant);
	}
}

void FHyperAIStudioPIEOptionalVariantRegistry::ResetAdapter(
	const EHyperAIStudioPIEOptionalVariant Variant)
{
	auto& State = HyperAIStudio::PIEPlaytest::Private::GetOptionalAdapterState();
	FScopeLock Lock(&State.Mutex);
	State.Adapters.Remove(Variant);
}

void FHyperAIStudioPIEOptionalVariantRegistry::ResetAll()
{
	auto& State = HyperAIStudio::PIEPlaytest::Private::GetOptionalAdapterState();
	FScopeLock Lock(&State.Mutex);
	State.Adapters.Reset();
}

TSharedPtr<IHyperAIStudioPIEOptionalVariantAdapter> FHyperAIStudioPIEOptionalVariantRegistry::GetAdapter(
	const EHyperAIStudioPIEOptionalVariant Variant)
{
	auto& State = HyperAIStudio::PIEPlaytest::Private::GetOptionalAdapterState();
	FScopeLock Lock(&State.Mutex);
	return State.Adapters.FindRef(Variant);
}

namespace HyperAIStudio::PIEPlaytest::Private
{
	FString NetModeToString(const ENetMode Mode)
	{
		switch (Mode)
		{
		case NM_Standalone: return TEXT("standalone");
		case NM_DedicatedServer: return TEXT("dedicated_server");
		case NM_ListenServer: return TEXT("listen_server");
		case NM_Client: return TEXT("client");
		default: return TEXT("unknown");
		}
	}

	bool ReadTopologyConfig(
		const ULevelEditorPlaySettings* Settings,
		FHyperAIStudioPIETopologyConfig& OutConfig)
	{
		if (!Settings)
		{
			return false;
		}
		EPlayNetMode PlayNetMode = PIE_Standalone;
		bool bRunUnderOneProcess = false;
		int32 ClientCount = 0;
		if (!Settings->GetPlayNetMode(PlayNetMode)
			|| !Settings->GetRunUnderOneProcess(bRunUnderOneProcess)
			|| !Settings->GetPlayNumberOfClients(ClientCount))
		{
			return false;
		}
		switch (PlayNetMode)
		{
		case PIE_Standalone: OutConfig.PlayNetMode = TEXT("standalone"); break;
		case PIE_ListenServer: OutConfig.PlayNetMode = TEXT("listen_server"); break;
		case PIE_Client: OutConfig.PlayNetMode = TEXT("client"); break;
		default: return false;
		}
		OutConfig.bRunUnderOneProcess = bRunUnderOneProcess;
		OutConfig.ClientCount = ClientCount;
		OutConfig.bLaunchSeparateServer = Settings->bLaunchSeparateServer;
		return ClientCount >= 1;
	}

	FString ValuePreview(const FHyperAIPIEPlaytestValue& Value)
	{
		switch (Value.Type)
		{
		case EHyperAIPIEPlaytestValueType::Boolean: return Value.bBoolean ? TEXT("true") : TEXT("false");
		case EHyperAIPIEPlaytestValueType::Integer: return FString::FromInt(Value.Integer);
		case EHyperAIPIEPlaytestValueType::Float: return NumberToken(Value.Float);
		case EHyperAIPIEPlaytestValueType::Name: return Clip(Value.Name.ToString(), MaxPreviewCharacters);
		case EHyperAIPIEPlaytestValueType::String: return Clip(Value.String, MaxPreviewCharacters);
		case EHyperAIPIEPlaytestValueType::Vector:
			return FString::Printf(TEXT("X=%s Y=%s Z=%s"),
				*NumberToken(Value.Vector.X), *NumberToken(Value.Vector.Y), *NumberToken(Value.Vector.Z));
		default: return TEXT("none");
		}
	}

	bool ReadSafeScalar(
		const UObject* Object,
		const FString& PropertyName,
		FHyperAIPIEPlaytestValue& OutValue,
		FString& OutCode,
		FString& OutError)
	{
		OutValue = {};
		if (!Object)
		{
			OutCode = TEXT("target_unavailable");
			OutError = TEXT("The exact loaded target is unavailable.");
			return false;
		}
		const FProperty* Property = FindFProperty<FProperty>(Object->GetClass(), FName(*PropertyName));
		if (!Property || !Property->HasAnyPropertyFlags(CPF_BlueprintVisible)
			|| Property->HasAnyPropertyFlags(CPF_Deprecated))
		{
			OutCode = TEXT("property_not_readable");
			OutError = TEXT("The exact property is absent or not Blueprint-visible scalar state.");
			return false;
		}
		const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Object);
		if (!ValuePtr)
		{
			OutCode = TEXT("property_unavailable");
			OutError = TEXT("The scalar property value is unavailable.");
			return false;
		}
		if (const FBoolProperty* Typed = CastField<FBoolProperty>(Property))
		{
			OutValue.Type = EHyperAIPIEPlaytestValueType::Boolean;
			OutValue.bBoolean = Typed->GetPropertyValue(ValuePtr);
			return true;
		}
		if (const FIntProperty* Typed = CastField<FIntProperty>(Property))
		{
			OutValue.Type = EHyperAIPIEPlaytestValueType::Integer;
			OutValue.Integer = Typed->GetPropertyValue(ValuePtr);
			return true;
		}
		if (const FByteProperty* Typed = CastField<FByteProperty>(Property))
		{
			OutValue.Type = EHyperAIPIEPlaytestValueType::Integer;
			OutValue.Integer = Typed->GetPropertyValue(ValuePtr);
			return true;
		}
		if (const FFloatProperty* Typed = CastField<FFloatProperty>(Property))
		{
			OutValue.Type = EHyperAIPIEPlaytestValueType::Float;
			OutValue.Float = Typed->GetPropertyValue(ValuePtr);
			return true;
		}
		if (const FDoubleProperty* Typed = CastField<FDoubleProperty>(Property))
		{
			OutValue.Type = EHyperAIPIEPlaytestValueType::Float;
			OutValue.Float = Typed->GetPropertyValue(ValuePtr);
			return true;
		}
		if (const FNameProperty* Typed = CastField<FNameProperty>(Property))
		{
			OutValue.Type = EHyperAIPIEPlaytestValueType::Name;
			OutValue.Name = Typed->GetPropertyValue(ValuePtr);
			return OutValue.Name.ToString().Len() <= FHyperAIStudioPIEPlaytestContracts::MaxValueCharacters;
		}
		if (const FStrProperty* Typed = CastField<FStrProperty>(Property))
		{
			const FString& StringValue = Typed->GetPropertyValue(ValuePtr);
			if (StringValue.Len() > FHyperAIStudioPIEPlaytestContracts::MaxValueCharacters)
			{
				OutCode = TEXT("scalar_value_too_large");
				OutError = TEXT("The string scalar exceeds the hard preview bound.");
				return false;
			}
			OutValue.Type = EHyperAIPIEPlaytestValueType::String;
			OutValue.String = StringValue;
			return true;
		}
		if (const FStructProperty* Typed = CastField<FStructProperty>(Property))
		{
			if (Typed->Struct == TBaseStructure<FVector>::Get())
			{
				OutValue.Type = EHyperAIPIEPlaytestValueType::Vector;
				OutValue.Vector = *static_cast<const FVector*>(ValuePtr);
				return !OutValue.Vector.ContainsNaN();
			}
		}
		OutCode = TEXT("property_type_not_allowlisted");
		OutError = TEXT("Only bool, int/byte, float/double, name, bounded string, and FVector reads are allowlisted.");
		return false;
	}

	bool EqualTypedValue(
		const FHyperAIPIEPlaytestValue& Actual,
		const FHyperAIPIEPlaytestValue& Expected)
	{
		if (Actual.Type != Expected.Type)
		{
			return false;
		}
		switch (Actual.Type)
		{
		case EHyperAIPIEPlaytestValueType::Boolean: return Actual.bBoolean == Expected.bBoolean;
		case EHyperAIPIEPlaytestValueType::Integer: return Actual.Integer == Expected.Integer;
		case EHyperAIPIEPlaytestValueType::Float: return FMath::IsNearlyEqual(Actual.Float, Expected.Float, 1.e-6);
		case EHyperAIPIEPlaytestValueType::Name: return Actual.Name == Expected.Name;
		case EHyperAIPIEPlaytestValueType::String: return Actual.String == Expected.String;
		case EHyperAIPIEPlaytestValueType::Vector: return Actual.Vector.Equals(Expected.Vector, 1.e-4);
		default: return true;
		}
	}

	UObject* ResolveLoadedObject(const FString& ObjectPath)
	{
		return ObjectPath.IsEmpty() ? nullptr : FSoftObjectPath(ObjectPath).ResolveObject();
	}

	bool BelongsToWorld(const UObject* Object, const UWorld* World)
	{
		return Object && World && Object->GetWorld() == World;
	}

	FString Sha1Bytes(const void* Data, const uint64 ByteCount)
	{
		uint8 Hash[FSHA1::DigestSize];
		FSHA1::HashBuffer(Data, ByteCount, Hash);
		return TEXT("sha1:") + BytesToHex(Hash, UE_ARRAY_COUNT(Hash)).ToLower();
	}

	FHyperAIStudioPIEDriverResult DriverFailure(
		const EHyperAIStudioPIEDriverOutcome Outcome,
		const FString& Code,
		const FString& Diagnostic)
	{
		FHyperAIStudioPIEDriverResult Result;
		Result.Outcome = Outcome;
		Result.Code = Code;
		Result.Diagnostic = Clip(Diagnostic, MaxDiagnosticCharacters);
		return Result;
	}

	FHyperAIStudioPIEDriverResult DriverSuccess(
		const FHyperAIPIEPlaytestStep& Step,
		const FString& Status = TEXT("completed"))
	{
		FHyperAIStudioPIEDriverResult Result;
		Result.Evidence.StepId = Step.StepId;
		Result.Evidence.Kind = FHyperAIStudioPIEPlaytestContracts::StepKindToString(Step.Kind);
		Result.Evidence.Status = Status;
		Result.Evidence.ObjectPath = Clip(Step.TargetObjectPath,
			FHyperAIStudioPIEPlaytestContracts::MaxObjectPathCharacters);
		Result.Evidence.Name = Clip(Step.Name, FHyperAIStudioPIEPlaytestContracts::MaxMarkerCharacters);
		Result.Evidence.bEffect = FHyperAIStudioPIEPlaytestContracts::IsEffectStep(Step.Kind);
		return Result;
	}

	class FSystemPIEClock final : public IHyperAIStudioPIEClock
	{
	public:
		virtual int64 NowMonotonicMs() const override
		{
			return static_cast<int64>(FPlatformTime::Seconds() * 1000.0);
		}
		virtual FString NowUtc() const override
		{
			return FDateTime::UtcNow().ToIso8601();
		}
	};

	class FUnrealPIEPlaytestDriver final : public IHyperAIStudioPIEPlaytestDriver
	{
	public:
		virtual FHyperAIStudioPIEDriverResult PreflightStart(
			const FHyperAIStudioPIENormalizedPlan& Plan) override
		{
			if (!IsInGameThread() || !GEditor)
			{
				return DriverFailure(EHyperAIStudioPIEDriverOutcome::FailedBeforeEffect,
					TEXT("editor_unavailable"), TEXT("PIE start requires the editor game thread."));
			}
			if (GEditor->IsPlaySessionInProgress())
			{
				return DriverFailure(EHyperAIStudioPIEDriverOutcome::FailedBeforeEffect,
					TEXT("session_already_active"), TEXT("StartInProcess refuses to replace or attach to an existing/queued play session."));
			}
			ULevelEditorPlaySettings* Settings = DuplicateObject<ULevelEditorPlaySettings>(
				GetDefault<ULevelEditorPlaySettings>(), GetTransientPackage());
			if (!Settings)
			{
				return DriverFailure(EHyperAIStudioPIEDriverOutcome::FailedBeforeEffect,
					TEXT("settings_unavailable"), TEXT("Could not create isolated transient PIE settings."));
			}
			Settings->SetPlayNetMode(PIE_Standalone);
			Settings->SetRunUnderOneProcess(true);
			Settings->SetPlayNumberOfClients(1);
			Settings->bLaunchSeparateServer = false;
			OwnedSettings.Reset(Settings);
			return FHyperAIStudioPIEDriverResult();
		}

		virtual FHyperAIStudioPIEDriverResult RequestStart(
			const FHyperAIStudioPIENormalizedPlan& Plan) override
		{
			if (!IsInGameThread() || !GEditor || !OwnedSettings.IsValid()
				|| GEditor->IsPlaySessionInProgress())
			{
				return DriverFailure(EHyperAIStudioPIEDriverOutcome::FailedBeforeEffect,
					TEXT("start_precondition_changed"),
					TEXT("The typed PIE start precondition changed after preflight and before dispatch."));
			}
			FRequestPlaySessionParams Params;
			Params.SessionDestination = EPlaySessionDestinationType::InProcess;
			Params.WorldType = EPlaySessionWorldType::PlayInEditor;
			Params.EditorPlaySettings = OwnedSettings.Get();
			Params.bAllowOnlineSubsystem = false;
			GEditor->RequestPlaySession(Params);
			bStartRequested = true;
			FHyperAIStudioPIEDriverResult Result;
			Result.bOwnedSessionClaimed = true;
			Result.Evidence.Kind = TEXT("start_in_process");
			Result.Evidence.Status = TEXT("requested");
			Result.Evidence.bEffect = true;
			return Result;
		}

		virtual FHyperAIStudioPIEDriverResult ObserveWorld(
			const FHyperAIStudioPIENormalizedPlan& Plan) override
		{
			if (!IsInGameThread() || !GEditor || !GEngine)
			{
				return DriverFailure(EHyperAIStudioPIEDriverOutcome::FailedBeforeEffect,
					TEXT("editor_unavailable"), TEXT("PIE observation requires the editor game thread."));
			}
			const TOptional<FPlayInEditorSessionInfo> SessionInfo = GEditor->GetPlayInEditorSessionInfo();
			if (!SessionInfo.IsSet())
			{
				if (GEditor->IsPlaySessionRequestQueued()
					|| Plan.Request.Mode == EHyperAIPIEPlaytestMode::AttachOnly)
				{
					FHyperAIStudioPIEDriverResult Pending;
					Pending.Outcome = EHyperAIStudioPIEDriverOutcome::Pending;
					Pending.Code = TEXT("waiting_for_session");
					return Pending;
				}
				return DriverFailure(EHyperAIStudioPIEDriverOutcome::FailedAfterKnownEffect,
					TEXT("session_start_failed"), TEXT("The owned PIE request ended without an active session."));
			}
			FHyperAIStudioPIESessionIdentity Identity;
			Identity.bActive = true;
			Identity.bExternalDestination =
				SessionInfo->OriginalRequestParams.SessionDestination != EPlaySessionDestinationType::InProcess;
			FHyperAIStudioPIETopologyConfig FrozenConfig;
			if (!ReadTopologyConfig(SessionInfo->OriginalRequestParams.EditorPlaySettings, FrozenConfig))
			{
				return DriverFailure(EHyperAIStudioPIEDriverOutcome::UnsupportedMultiprocess,
					TEXT("frozen_topology_unavailable"),
					TEXT("Active OriginalRequestParams topology is unavailable; mutable current settings are never substituted."));
			}
			FrozenConfig.bServerWasLaunched = SessionInfo->bServerWasLaunched;
			FrozenConfig.bExternalSessionDestination = Identity.bExternalDestination;
			const FHyperAIStudioPIETopologyResolution Topology =
				FHyperAIStudioNativeReadContracts::ResolvePieTopology(true, true, FrozenConfig, {});
			Identity.TopologySource = Topology.Source;
			Identity.bMultiprocess = !Topology.bValid || Topology.bMultiprocess;
			if (Identity.bExternalDestination || Identity.bMultiprocess)
			{
				FHyperAIStudioPIEDriverResult Unsupported = DriverFailure(
					EHyperAIStudioPIEDriverOutcome::UnsupportedMultiprocess,
					TEXT("unsupported_multiprocess"),
					TEXT("External or multiprocess PIE is unobservable and therefore refused."));
				Unsupported.Session = Identity;
				return Unsupported;
			}

			const FWorldContext* Chosen = nullptr;
			const FWorldContext* Primary = nullptr;
			int32 PrimaryCount = 0;
			int32 MatchingCount = 0;
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				if (Context.WorldType != EWorldType::PIE || !Context.World())
				{
					continue;
				}
				++Identity.ObservableWorldCount;
				if (Identity.ObservableWorldCount > MaxObservablePieWorlds)
				{
					return DriverFailure(EHyperAIStudioPIEDriverOutcome::FailedBeforeEffect,
						TEXT("world_source_limit"), TEXT("The in-process PIE world source exceeds the hard 64-world bound."));
				}
				if (Context.bIsPrimaryPIEInstance)
				{
					Primary = &Context;
					++PrimaryCount;
				}
				if (Plan.Request.PieInstance != INDEX_NONE && Context.PIEInstance == Plan.Request.PieInstance)
				{
					Chosen = &Context;
					++MatchingCount;
				}
			}
			if (Plan.Request.PieInstance == INDEX_NONE)
			{
				if (PrimaryCount == 1)
				{
					Chosen = Primary;
				}
				else if (Identity.ObservableWorldCount == 1)
				{
					for (const FWorldContext& Context : GEngine->GetWorldContexts())
					{
						if (Context.WorldType == EWorldType::PIE && Context.World())
						{
							Chosen = &Context;
							break;
						}
					}
				}
			}
			else if (MatchingCount != 1)
			{
				Chosen = nullptr;
			}
			if (!Chosen)
			{
				FHyperAIStudioPIEDriverResult Pending;
				Pending.Outcome = EHyperAIStudioPIEDriverOutcome::Pending;
				Pending.Code = Identity.ObservableWorldCount == 0
					? TEXT("waiting_for_world") : TEXT("ambiguous_world_selection");
				Pending.Session = Identity;
				return Pending;
			}

			UWorld* World = Chosen->World();
			Identity.ContextHandle = Clip(Chosen->ContextHandle.ToString(), 128);
			Identity.PieInstance = Chosen->PIEInstance;
			Identity.WorldPath = Clip(World->GetPathName(), FHyperAIStudioPIEPlaytestContracts::MaxObjectPathCharacters);
			Identity.NetMode = NetModeToString(World->GetNetMode());
			if (const UPackage* Package = World->GetPackage())
			{
				Identity.WorldPackage = Clip(Package->GetName(), FHyperAIStudioPIEPlaytestContracts::MaxObjectPathCharacters);
				int32 PrefixInstance = INDEX_NONE;
				Identity.SourcePackage = Clip(UWorld::RemovePIEPrefix(Identity.WorldPackage, &PrefixInstance),
					FHyperAIStudioPIEPlaytestContracts::MaxObjectPathCharacters);
			}
			FString Canonical;
			AppendCanonicalToken(Canonical, TEXT("hyperai.pie-session.v1"));
			AppendCanonicalToken(Canonical, Identity.TopologySource);
			AppendCanonicalToken(Canonical, Identity.ContextHandle);
			AppendCanonicalToken(Canonical, FString::FromInt(Identity.PieInstance));
			AppendCanonicalToken(Canonical, Identity.WorldPath);
			AppendCanonicalToken(Canonical, Identity.WorldPackage);
			AppendCanonicalToken(Canonical, Identity.SourcePackage);
			AppendCanonicalToken(Canonical, Identity.NetMode);
			Identity.Fingerprint = FHyperAIStudioPlanExecuteContracts::ComputeBoundedSha256(Canonical);
			if (Identity.Fingerprint.IsEmpty())
			{
				return DriverFailure(EHyperAIStudioPIEDriverOutcome::FailedBeforeEffect,
					TEXT("identity_hash_failed"), TEXT("PIE session identity could not be fingerprinted."));
			}
			if (ActiveSession.bActive && !ActiveSession.Fingerprint.IsEmpty()
				&& ActiveSession.Fingerprint != Identity.Fingerprint)
			{
				return DriverFailure(
					bStartRequested || bAnyRuntimeEffectAttempted
						? EHyperAIStudioPIEDriverOutcome::OutcomeUnknown
						: EHyperAIStudioPIEDriverOutcome::FailedBeforeEffect,
					TEXT("session_identity_changed"),
					TEXT("The observable PIE world no longer matches the frozen session identity."));
			}
			ActiveWorld = World;
			ActiveSession = Identity;
			FHyperAIStudioPIEDriverResult Result;
			Result.Session = Identity;
			Result.Code = TEXT("world_ready");
			return Result;
		}

		virtual FHyperAIStudioPIEDriverResult BeginStep(
			const FHyperAIStudioPIENormalizedPlan& Plan,
			const FHyperAIPIEPlaytestStep& Step,
			const FHyperAIStudioPIEExecutionBinding& Binding,
			IHyperAIStudioPIETrustedExecutionGate* TrustedGate) override
		{
			if (!IsInGameThread())
			{
				return DriverFailure(EHyperAIStudioPIEDriverOutcome::FailedBeforeEffect,
					TEXT("wrong_thread"), TEXT("PIE steps execute only on the editor game thread."));
			}
			UWorld* World = ActiveWorld.Get();
			if (!World || !ActiveSession.bActive)
			{
				return DriverFailure(EHyperAIStudioPIEDriverOutcome::FailedBeforeEffect,
					TEXT("world_unavailable"), TEXT("The frozen target PIE world is no longer available."));
			}
			PendingOptionalAdapter.Reset();
			PendingKind = Step.Kind;
			bAnyRuntimeEffectAttempted |= FHyperAIStudioPIEPlaytestContracts::IsEffectStep(Step.Kind);

			if (IsOptionalStep(Step.Kind))
			{
				PendingOptionalAdapter = FHyperAIStudioPIEOptionalVariantRegistry::GetAdapter(VariantForStep(Step.Kind));
				if (!PendingOptionalAdapter)
				{
					return DriverFailure(EHyperAIStudioPIEDriverOutcome::FailedBeforeEffect,
						TEXT("prerequisite_unavailable"),
						TEXT("The capability-gated AI, Enhanced Input, or replay adapter is not loaded."));
				}
				OptionalAdaptersUsed.Add(VariantForStep(Step.Kind), PendingOptionalAdapter);
				return PendingOptionalAdapter->Begin(Plan, Step, ActiveSession, Binding, TrustedGate);
			}

			FHyperAIStudioPIEDriverResult Result = DriverSuccess(Step);
			switch (Step.Kind)
			{
			case EHyperAIPIEPlaytestStepKind::WaitForWorld:
				return ObserveWorld(Plan);
			case EHyperAIPIEPlaytestStepKind::QueryActors:
			{
				int32 Scanned = 0;
				FString Preview;
				for (TActorIterator<AActor> It(World); It && Scanned < FHyperAIStudioPIEPlaytestContracts::MaxActorScan; ++It, ++Scanned)
				{
					const AActor* Actor = *It;
					if (!Actor || Result.Evidence.ItemCount >= Step.MaxItems)
					{
						continue;
					}
					++Result.Evidence.ItemCount;
					if (Preview.Len() < MaxPreviewCharacters)
					{
						if (!Preview.IsEmpty()) { Preview += TEXT(";"); }
						Preview += Clip(Actor->GetPathName(), 192);
					}
				}
				Result.Evidence.ValuePreview = Clip(Preview, MaxPreviewCharacters);
				if (Scanned >= FHyperAIStudioPIEPlaytestContracts::MaxActorScan)
				{
					Result.Evidence.Status = TEXT("partial");
					Result.Evidence.DiagnosticCode = TEXT("actor_scan_bound");
				}
				return Result;
			}
			case EHyperAIPIEPlaytestStepKind::QueryComponents:
			{
				const AActor* Actor = Cast<AActor>(ResolveLoadedObject(Step.TargetObjectPath));
				if (!Actor || !BelongsToWorld(Actor, World))
				{
					return DriverFailure(EHyperAIStudioPIEDriverOutcome::FailedBeforeEffect,
						TEXT("actor_not_found"), TEXT("The exact loaded actor is not in the frozen PIE world."));
				}
				int32 Scanned = 0;
				FString Preview;
				for (const UActorComponent* Component : Actor->GetComponents())
				{
					if (++Scanned > FHyperAIStudioPIEPlaytestContracts::MaxComponentScan)
					{
						Result.Evidence.Status = TEXT("partial");
						Result.Evidence.DiagnosticCode = TEXT("component_scan_bound");
						break;
					}
					if (!Component || Result.Evidence.ItemCount >= Step.MaxItems)
					{
						continue;
					}
					++Result.Evidence.ItemCount;
					if (Preview.Len() < MaxPreviewCharacters)
					{
						if (!Preview.IsEmpty()) { Preview += TEXT(";"); }
						Preview += Clip(Component->GetPathName(), 192);
					}
				}
				Result.Evidence.ValuePreview = Clip(Preview, MaxPreviewCharacters);
				return Result;
			}
			case EHyperAIPIEPlaytestStepKind::QueryScalarProperty:
			case EHyperAIPIEPlaytestStepKind::AssertScalarEquals:
			{
				UObject* Object = ResolveLoadedObject(Step.TargetObjectPath);
				if (!BelongsToWorld(Object, World))
				{
					return DriverFailure(EHyperAIStudioPIEDriverOutcome::FailedBeforeEffect,
						TEXT("object_not_found"), TEXT("The exact loaded object is not in the frozen PIE world."));
				}
				FHyperAIPIEPlaytestValue Actual;
				FString Code;
				FString Error;
				if (!ReadSafeScalar(Object, Step.Name, Actual, Code, Error))
				{
					return DriverFailure(EHyperAIStudioPIEDriverOutcome::FailedBeforeEffect, Code, Error);
				}
				Result.Evidence.ValuePreview = ValuePreview(Actual);
				if (Step.Kind == EHyperAIPIEPlaytestStepKind::AssertScalarEquals)
				{
					Result.Evidence.bAssertion = true;
					Result.Evidence.bPassed = EqualTypedValue(Actual, Step.Value);
					if (!Result.Evidence.bPassed)
					{
						Result.Outcome = EHyperAIStudioPIEDriverOutcome::FailedBeforeEffect;
						Result.Code = TEXT("assertion_failed");
						Result.Diagnostic = TEXT("The fresh scalar value does not equal the typed expected value.");
						Result.Evidence.Status = TEXT("failed");
						Result.Evidence.DiagnosticCode = Result.Code;
					}
				}
				return Result;
			}
			case EHyperAIPIEPlaytestStepKind::TeleportActor:
			{
				AActor* Actor = Cast<AActor>(ResolveLoadedObject(Step.TargetObjectPath));
				FString ApprovalError;
				if (!Actor || !BelongsToWorld(Actor, World) || !TrustedGate
					|| !TrustedGate->ApproveRuntimeTarget(Binding, Step.Kind, *Actor, ApprovalError))
				{
					return DriverFailure(EHyperAIStudioPIEDriverOutcome::FailedBeforeEffect,
						TEXT("target_not_allowlisted"), ApprovalError.IsEmpty()
							? TEXT("The exact PIE actor is unavailable or not server-allowlisted.") : ApprovalError);
				}
				if (!Actor->SetActorLocationAndRotation(
					Step.Location, Step.Rotation, false, nullptr, ETeleportType::TeleportPhysics))
				{
					return DriverFailure(EHyperAIStudioPIEDriverOutcome::FailedAfterKnownEffect,
						TEXT("teleport_rejected"), TEXT("The allowlisted actor rejected the requested transform."));
				}
				Result.Evidence.ValuePreview = FString::Printf(TEXT("%s|%s"),
					*Step.Location.ToCompactString(), *Step.Rotation.ToCompactString());
				return Result;
			}
			case EHyperAIPIEPlaytestStepKind::SpawnAllowlistedActor:
			{
				if (!TrustedGate)
				{
					return DriverFailure(EHyperAIStudioPIEDriverOutcome::FailedBeforeEffect,
						TEXT("trusted_gate_unavailable"), TEXT("Spawn defaults to deny without the shared trusted gate."));
				}
				FString ResolveError;
				UClass* Class = TrustedGate->ResolveAllowlistedSpawnClass(Binding, Step.ClassId, ResolveError);
				if (!Class || !Class->IsChildOf(AActor::StaticClass())
					|| Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
				{
					return DriverFailure(EHyperAIStudioPIEDriverOutcome::FailedBeforeEffect,
						TEXT("spawn_class_not_allowlisted"), ResolveError.IsEmpty()
							? TEXT("The opaque class_id did not resolve to an exact loaded allowlisted actor class.") : ResolveError);
				}
				FActorSpawnParameters Params;
				Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::DontSpawnIfColliding;
				AActor* Spawned = World->SpawnActor<AActor>(Class, Step.Location, Step.Rotation, Params);
				if (!Spawned)
				{
					return DriverFailure(EHyperAIStudioPIEDriverOutcome::FailedAfterKnownEffect,
						TEXT("spawn_failed"), TEXT("The allowlisted spawn was attempted but did not produce an actor."));
				}
				SpawnedActors.Add(Spawned);
				Result.Evidence.ObjectPath = Clip(Spawned->GetPathName(),
					FHyperAIStudioPIEPlaytestContracts::MaxObjectPathCharacters);
				return Result;
			}
			case EHyperAIPIEPlaytestStepKind::DestroyAllowlistedActor:
			{
				AActor* Actor = Cast<AActor>(ResolveLoadedObject(Step.TargetObjectPath));
				FString ApprovalError;
				if (!Actor || !BelongsToWorld(Actor, World) || !TrustedGate
					|| !TrustedGate->ApproveRuntimeTarget(Binding, Step.Kind, *Actor, ApprovalError))
				{
					return DriverFailure(EHyperAIStudioPIEDriverOutcome::FailedBeforeEffect,
						TEXT("target_not_allowlisted"), ApprovalError.IsEmpty()
							? TEXT("The exact PIE actor is unavailable or not server-allowlisted.") : ApprovalError);
				}
				if (!Actor->Destroy())
				{
					return DriverFailure(EHyperAIStudioPIEDriverOutcome::FailedAfterKnownEffect,
						TEXT("destroy_failed"), TEXT("The allowlisted runtime actor rejected Destroy()."));
				}
				return Result;
			}
			case EHyperAIPIEPlaytestStepKind::CaptureScreenshotHash:
			{
				UGameViewportClient* ViewportClient = World->GetGameViewport();
				FViewport* Viewport = ViewportClient ? ViewportClient->Viewport : nullptr;
				if (!Viewport)
				{
					return DriverFailure(EHyperAIStudioPIEDriverOutcome::FailedBeforeEffect,
						TEXT("viewport_unavailable"), TEXT("The selected PIE world has no observable game viewport."));
				}
				const FIntPoint Size = Viewport->GetSizeXY();
				const int64 Pixels = static_cast<int64>(Size.X) * static_cast<int64>(Size.Y);
				if (Size.X < 1 || Size.Y < 1
					|| Size.X > FHyperAIStudioPIEPlaytestContracts::MaxScreenshotWidth
					|| Size.Y > FHyperAIStudioPIEPlaytestContracts::MaxScreenshotHeight
					|| Pixels > FHyperAIStudioPIEPlaytestContracts::MaxScreenshotPixels)
				{
					return DriverFailure(EHyperAIStudioPIEDriverOutcome::FailedBeforeEffect,
						TEXT("viewport_pixel_limit"), TEXT("The PIE viewport exceeds the exact 1920x1080 pixel cap."));
				}
				TArray<FColor> Colors;
				if (!Viewport->ReadPixels(Colors) || Colors.Num() != Pixels)
				{
					return DriverFailure(EHyperAIStudioPIEDriverOutcome::FailedBeforeEffect,
						TEXT("pixel_read_failed"), TEXT("The bounded PIE viewport pixel read failed or returned the wrong count."));
				}
				Result.Evidence.ScreenshotWidth = Size.X;
				Result.Evidence.ScreenshotHeight = Size.Y;
				Result.Evidence.ScreenshotHash = Sha1Bytes(Colors.GetData(),
					static_cast<uint64>(Colors.Num()) * sizeof(FColor));
				return Result;
			}
			case EHyperAIPIEPlaytestStepKind::LogMarker:
			{
				FHyperAIStudioDiagnosticsLogBuffer& LogBuffer = FHyperAIStudioDiagnosticsLogBuffer::Get();
				const FHyperAIStudioDiagnosticsLogSnapshot Before = LogBuffer.Snapshot();
				if (!Before.bAttached)
				{
					return DriverFailure(EHyperAIStudioPIEDriverOutcome::FailedBeforeEffect,
						TEXT("log_capture_unavailable"),
						TEXT("The bounded diagnostics log buffer is not attached."));
				}
				Result.Evidence.LogSequenceBefore = Before.LatestSequence;
				const FString Marker = FString::Printf(
					TEXT("playtest-marker operation=%s step=%s marker=%s"),
					*Plan.Request.OperationId, *Step.StepId, *Step.Name);
				LogBuffer.Serialize(*Marker, ELogVerbosity::Display, FName(TEXT("LogHyperAIStudioPIEPlaytest")));
				const FHyperAIStudioDiagnosticsLogSnapshot After = LogBuffer.Snapshot();
				Result.Evidence.LogSequenceAfter = After.LatestSequence;
				if (After.LatestSequence <= Before.LatestSequence)
				{
					return DriverFailure(EHyperAIStudioPIEDriverOutcome::FailedAfterKnownEffect,
						TEXT("log_marker_unobservable"),
						TEXT("The bounded log marker was attempted but no fresh sequence could be proven."));
				}
				return Result;
			}
			case EHyperAIPIEPlaytestStepKind::AssertActorExists:
			{
				const AActor* Actor = Cast<AActor>(ResolveLoadedObject(Step.TargetObjectPath));
				Result.Evidence.bAssertion = true;
				Result.Evidence.bPassed = Actor && BelongsToWorld(Actor, World) && !Actor->IsActorBeingDestroyed();
				if (!Result.Evidence.bPassed)
				{
					Result.Outcome = EHyperAIStudioPIEDriverOutcome::FailedBeforeEffect;
					Result.Code = TEXT("assertion_failed");
					Result.Diagnostic = TEXT("The exact actor does not exist in the frozen PIE world.");
					Result.Evidence.Status = TEXT("failed");
					Result.Evidence.DiagnosticCode = Result.Code;
				}
				return Result;
			}
			case EHyperAIPIEPlaytestStepKind::PauseSession:
				if (!GEditor || !GEditor->SetPIEWorldsPaused(true))
				{
					return DriverFailure(EHyperAIStudioPIEDriverOutcome::FailedAfterKnownEffect,
						TEXT("pause_failed"), TEXT("UEditorEngine::SetPIEWorldsPaused(true) failed."));
				}
				bPausedByRun = true;
				return Result;
			case EHyperAIPIEPlaytestStepKind::ResumeSession:
				if (!GEditor || !GEditor->SetPIEWorldsPaused(false))
				{
					return DriverFailure(EHyperAIStudioPIEDriverOutcome::FailedAfterKnownEffect,
						TEXT("resume_failed"), TEXT("UEditorEngine::SetPIEWorldsPaused(false) failed."));
				}
				bPausedByRun = false;
				return Result;
			case EHyperAIPIEPlaytestStepKind::StopSession:
				if (!GEditor || !GEditor->IsPlaySessionInProgress())
				{
					return DriverFailure(EHyperAIStudioPIEDriverOutcome::FailedBeforeEffect,
						TEXT("session_not_active"), TEXT("There is no active or queued PIE session to stop."));
				}
				GEditor->RequestEndPlayMap();
				bStopRequested = true;
				Result.Outcome = EHyperAIStudioPIEDriverOutcome::Pending;
				return Result;
			default:
				return DriverFailure(EHyperAIStudioPIEDriverOutcome::FailedBeforeEffect,
					TEXT("step_not_implemented"), TEXT("The closed core PIE driver has no route for this step."));
			}
		}

		virtual FHyperAIStudioPIEDriverResult PollStep(
			const FHyperAIStudioPIENormalizedPlan& Plan,
			const FHyperAIPIEPlaytestStep& Step) override
		{
			if (PendingOptionalAdapter)
			{
				return PendingOptionalAdapter->Poll(Plan, Step, ActiveSession);
			}
			if (PendingKind == EHyperAIPIEPlaytestStepKind::WaitForWorld)
			{
				return ObserveWorld(Plan);
			}
			if (PendingKind == EHyperAIPIEPlaytestStepKind::StopSession)
			{
				if (GEditor && GEditor->IsPlaySessionInProgress())
				{
					FHyperAIStudioPIEDriverResult Pending;
					Pending.Outcome = EHyperAIStudioPIEDriverOutcome::Pending;
					return Pending;
				}
				ActiveWorld.Reset();
				ActiveSession.bActive = false;
				return DriverSuccess(Step);
			}
			return DriverFailure(EHyperAIStudioPIEDriverOutcome::OutcomeUnknown,
				TEXT("poll_without_pending_route"), TEXT("A pending core step lost its typed poll route."));
		}

		virtual FHyperAIStudioPIEDriverResult BeginTeardown(
			const FHyperAIStudioPIENormalizedPlan& Plan,
			const bool bOwnsSession) override
		{
			for (const auto& Pair : OptionalAdaptersUsed)
			{
				if (Pair.Value) { Pair.Value->Teardown(Plan); }
			}
			OptionalAdaptersUsed.Reset();
			PendingOptionalAdapter.Reset();
			for (const TWeakObjectPtr<AActor>& Spawned : SpawnedActors)
			{
				if (Spawned.IsValid() && !Spawned->IsActorBeingDestroyed())
				{
					Spawned->Destroy();
				}
			}
			SpawnedActors.Reset();
			if (bPausedByRun && GEditor)
			{
				GEditor->SetPIEWorldsPaused(false);
				bPausedByRun = false;
			}
			const bool bMustStop = bOwnsSession
				&& Plan.Request.Teardown == EHyperAIPIEPlaytestTeardown::StopOwnedSession;
			if (bMustStop && GEditor && GEditor->IsPlaySessionRequestQueued())
			{
				GEditor->CancelRequestPlaySession();
			}
			if (bMustStop && GEditor && GEditor->IsPlaySessionInProgress())
			{
				GEditor->RequestEndPlayMap();
				bStopRequested = true;
				FHyperAIStudioPIEDriverResult Pending;
				Pending.Outcome = EHyperAIStudioPIEDriverOutcome::Pending;
				Pending.Code = TEXT("waiting_for_owned_session_stop");
				return Pending;
			}
			FHyperAIStudioPIEDriverResult Result;
			Result.Code = TEXT("teardown_complete");
			return Result;
		}

		virtual FHyperAIStudioPIEDriverResult PollTeardown(
			const FHyperAIStudioPIENormalizedPlan& Plan,
			const bool bOwnsSession) override
		{
			if (bOwnsSession && Plan.Request.Teardown == EHyperAIPIEPlaytestTeardown::StopOwnedSession
				&& GEditor && GEditor->IsPlaySessionInProgress())
			{
				FHyperAIStudioPIEDriverResult Pending;
				Pending.Outcome = EHyperAIStudioPIEDriverOutcome::Pending;
				return Pending;
			}
			ActiveWorld.Reset();
			ActiveSession.bActive = false;
			OwnedSettings.Reset();
			FHyperAIStudioPIEDriverResult Result;
			Result.Code = TEXT("teardown_complete");
			return Result;
		}

		virtual void ForceTeardown(
			const FHyperAIStudioPIENormalizedPlan& Plan,
			const bool bOwnsSession) override
		{
			for (const auto& Pair : OptionalAdaptersUsed)
			{
				if (Pair.Value) { Pair.Value->Teardown(Plan); }
			}
			OptionalAdaptersUsed.Reset();
			PendingOptionalAdapter.Reset();
			if (bPausedByRun && GEditor)
			{
				GEditor->SetPIEWorldsPaused(false);
			}
			if (bOwnsSession && GEditor && GEditor->IsPlaySessionRequestQueued())
			{
				GEditor->CancelRequestPlaySession();
			}
			if (bOwnsSession && GEditor && GEditor->IsPlaySessionInProgress())
			{
				GEditor->RequestEndPlayMap();
			}
			ActiveWorld.Reset();
			OwnedSettings.Reset();
		}

	private:
		TStrongObjectPtr<ULevelEditorPlaySettings> OwnedSettings;
		TWeakObjectPtr<UWorld> ActiveWorld;
		FHyperAIStudioPIESessionIdentity ActiveSession;
		TArray<TWeakObjectPtr<AActor>> SpawnedActors;
		TMap<EHyperAIStudioPIEOptionalVariant, TSharedPtr<IHyperAIStudioPIEOptionalVariantAdapter>> OptionalAdaptersUsed;
		TSharedPtr<IHyperAIStudioPIEOptionalVariantAdapter> PendingOptionalAdapter;
		EHyperAIPIEPlaytestStepKind PendingKind = EHyperAIPIEPlaytestStepKind::WaitForWorld;
		bool bStartRequested = false;
		bool bStopRequested = false;
		bool bPausedByRun = false;
		bool bAnyRuntimeEffectAttempted = false;
	};
}

bool FHyperAIStudioPIEPlaytestRuntime::Start(
	const FHyperAIStudioPIENormalizedPlan& Plan,
	const FString& InCanonicalProjectId,
	TSharedPtr<IHyperAIStudioPIEPlaytestDriver> Driver,
	TSharedPtr<IHyperAIStudioPIETrustedExecutionGate> TrustedGate,
	TSharedPtr<IHyperAIStudioPIEClock> Clock,
	FString& OutError)
{
	OutError.Reset();
	if (Current.State != EHyperAIStudioPIERuntimeState::Idle)
	{
		OutError = TEXT("The PIE runtime instance has already been started.");
		return false;
	}
	if (!Driver || !Clock)
	{
		OutError = TEXT("PIE runtime requires a typed driver and monotonic clock.");
		return false;
	}
	Current.Plan = Plan;
	Current.StartedUtc = Clock->NowUtc();
	Current.UpdatedUtc = Current.StartedUtc;
	Current.Status = TEXT("accepted");
	Current.Diagnostic = TEXT("The bounded playtest plan was accepted for serial game-thread execution.");
	Current.State = EHyperAIStudioPIERuntimeState::Accepted;
	Current.bAccepted = true;
	Current.bOwnsSession = false;
	ActiveDriver = MoveTemp(Driver);
	ActiveTrustedGate = MoveTemp(TrustedGate);
	ActiveClock = MoveTemp(Clock);
	CanonicalProjectId = InCanonicalProjectId;
	WholeDeadlineMs = ActiveClock->NowMonotonicMs() + Plan.Request.WholeTimeoutMs;

	if (!Plan.bHasEffects)
	{
		return true;
	}
	if (!ActiveTrustedGate || CanonicalProjectId.IsEmpty())
	{
		Current.State = EHyperAIStudioPIERuntimeState::Failed;
		Current.Status = TEXT("trusted_gate_unavailable");
		Current.bAccepted = false;
		ActiveDriver.Reset();
		ActiveTrustedGate.Reset();
		ActiveClock.Reset();
		OutError = TEXT("Runtime effects default to deny until the shared authorization/journal-v3 staging seam is installed.");
		Current.Diagnostic = OutError;
		return false;
	}
	const FHyperAIStudioPIEExecutionBinding Binding = MakeBinding();
	const EHyperAIStudioPlanJournalBegin Begin = ActiveTrustedGate->Begin(Binding, OutError);
	switch (Begin)
	{
	case EHyperAIStudioPlanJournalBegin::ReplayCompleted:
		Current.State = EHyperAIStudioPIERuntimeState::ReplayCompleted;
		Current.Status = TEXT("replay_completed");
		Current.Diagnostic = TEXT("The shared journal-v3 gate proved an identical completed operation.");
		Current.bReplay = true;
		return true;
	case EHyperAIStudioPlanJournalBegin::ProceedNew:
		break;
	case EHyperAIStudioPlanJournalBegin::AlreadyInProgress:
		OutError = TEXT("operation_id is already in progress under the shared mutation lease.");
		break;
	case EHyperAIStudioPlanJournalBegin::OutcomeUnknown:
		OutError = TEXT("operation_id has outcome_unknown and requires evidence-based reconciliation.");
		break;
	case EHyperAIStudioPlanJournalBegin::Conflict:
		OutError = TEXT("operation_id is already bound to a different plan/capability hash.");
		break;
	case EHyperAIStudioPlanJournalBegin::ExistingTerminal:
		OutError = TEXT("operation_id already has a non-replayable terminal journal state.");
		break;
	default:
		OutError = TEXT("The shared journal-v3 gate is unavailable.");
		break;
	}
	if (Begin != EHyperAIStudioPlanJournalBegin::ProceedNew)
	{
		Current.State = Begin == EHyperAIStudioPlanJournalBegin::OutcomeUnknown
			? EHyperAIStudioPIERuntimeState::OutcomeUnknown
			: EHyperAIStudioPIERuntimeState::Failed;
		Current.bOutcomeUnknown = Begin == EHyperAIStudioPlanJournalBegin::OutcomeUnknown;
		Current.Status = FHyperAIStudioPIEPlaytestContracts::RuntimeStateToString(Current.State);
		Current.Diagnostic = OutError;
		Current.bAccepted = false;
		return false;
	}
	if (!ActiveTrustedGate->InspectAuthorization(Binding, OutError))
	{
		ActiveTrustedGate->Transition(Binding,
			EHyperAIStudioPlanJournalTransition::FailedPreCommit, {}, OutError);
		FinishTerminal(EHyperAIStudioPIERuntimeState::Failed, TEXT("authorization_rejected"), OutError);
		Current.bAccepted = false;
		return false;
	}
	if (!ActiveTrustedGate->Transition(Binding,
		EHyperAIStudioPlanJournalTransition::Running, {}, OutError))
	{
		FinishTerminal(EHyperAIStudioPIERuntimeState::Failed, TEXT("journal_transition_failed"), OutError);
		Current.bAccepted = false;
		return false;
	}
	if (!ActiveTrustedGate->ConsumeAuthorization(Binding, OutError))
	{
		ActiveTrustedGate->Transition(Binding,
			EHyperAIStudioPlanJournalTransition::FailedPreCommit, {}, OutError);
		FinishTerminal(EHyperAIStudioPIERuntimeState::Failed, TEXT("authorization_rejected"), OutError);
		Current.bAccepted = false;
		return false;
	}
	bAuthorizationConsumed = true;
	return true;
}

bool FHyperAIStudioPIEPlaytestRuntime::EnsureEffectBarrier(FString& OutError)
{
	OutError.Reset();
	if (Current.bCommitStarted)
	{
		return true;
	}
	if (!Current.Plan.bHasEffects || !bAuthorizationConsumed || !ActiveTrustedGate)
	{
		OutError = TEXT("The shared exact-plan authorization was not consumed before the effect barrier.");
		return false;
	}
	if (!ActiveTrustedGate->Transition(MakeBinding(),
		EHyperAIStudioPlanJournalTransition::CommitStarted, {}, OutError))
	{
		return false;
	}
	Current.bCommitStarted = true;
	return true;
}

void FHyperAIStudioPIEPlaytestRuntime::Tick()
{
	if (IsTerminal() || !ActiveClock || !ActiveDriver)
	{
		return;
	}
	const int64 NowMs = ActiveClock->NowMonotonicMs();
	Current.UpdatedUtc = ActiveClock->NowUtc();
	if (Current.State == EHyperAIStudioPIERuntimeState::Teardown)
	{
		if (NowMs >= TeardownDeadlineMs)
		{
			ActiveDriver->ForceTeardown(Current.Plan, Current.bOwnsSession);
			if (Current.bCommitStarted)
			{
				FString Ignored;
				if (ActiveTrustedGate)
				{
					ActiveTrustedGate->Transition(MakeBinding(),
						EHyperAIStudioPlanJournalTransition::OutcomeUnknown, {}, Ignored);
				}
				FinishTerminal(EHyperAIStudioPIERuntimeState::OutcomeUnknown,
					TEXT("teardown_timeout"), TEXT("Deterministic teardown did not complete before its hard deadline."));
			}
			else
			{
				FinishTerminal(EHyperAIStudioPIERuntimeState::Failed,
					TEXT("teardown_timeout"), TEXT("Teardown did not complete, but no effect barrier was crossed."));
			}
			return;
		}
		const FHyperAIStudioPIEDriverResult Result = bTeardownDispatched
			? ActiveDriver->PollTeardown(Current.Plan, Current.bOwnsSession)
			: ActiveDriver->BeginTeardown(Current.Plan, Current.bOwnsSession);
		bTeardownDispatched = true;
		if (Result.Outcome != EHyperAIStudioPIEDriverOutcome::Pending)
		{
			FinishAfterTeardown(Result);
		}
		return;
	}
	if (NowMs >= WholeDeadlineMs)
	{
		const bool bActiveEffectMayBePending = Current.State == EHyperAIStudioPIERuntimeState::ExecutingStep
				&& bStepDispatched && ActiveStepIndex < Current.Plan.Request.Steps.Num()
				&& FHyperAIStudioPIEPlaytestContracts::IsEffectStep(
					Current.Plan.Request.Steps[ActiveStepIndex].Kind);
		BeginTeardown(TEXT("whole_timeout"),
			TEXT("The playtest exceeded its whole-plan monotonic deadline."), bActiveEffectMayBePending);
		return;
	}

	switch (Current.State)
	{
	case EHyperAIStudioPIERuntimeState::Accepted:
		if (Current.Plan.Request.Mode == EHyperAIPIEPlaytestMode::StartInProcess)
		{
			const FHyperAIStudioPIEDriverResult Preflight = ActiveDriver->PreflightStart(Current.Plan);
			if (Preflight.Outcome != EHyperAIStudioPIEDriverOutcome::Succeeded)
			{
				HandleDriverResult(Preflight, false);
				return;
			}
			FString BarrierError;
			if (!EnsureEffectBarrier(BarrierError))
			{
				BeginTeardown(TEXT("effect_barrier_failed"), BarrierError, false);
				return;
			}
			const FHyperAIStudioPIEDriverResult Result = ActiveDriver->RequestStart(Current.Plan);
			Current.bOwnsSession |= Result.bOwnedSessionClaimed;
			if (Result.Outcome == EHyperAIStudioPIEDriverOutcome::Succeeded
				|| Result.Outcome == EHyperAIStudioPIEDriverOutcome::Pending)
			{
				if (!Result.Evidence.Kind.IsEmpty()) { AppendEvidence(Result.Evidence); }
				Current.State = EHyperAIStudioPIERuntimeState::StartingSession;
				Current.Status = TEXT("starting_session");
				WorldDeadlineMs = FMath::Min(
					WholeDeadlineMs,
					NowMs + HyperAIStudio::PIEPlaytest::Private::WorldReadyTimeoutMs);
				return;
			}
			HandleDriverResult(Result, true);
			return;
		}
		Current.State = EHyperAIStudioPIERuntimeState::WaitingForWorld;
		Current.Status = TEXT("waiting_for_world");
		WorldDeadlineMs = FMath::Min(
			WholeDeadlineMs,
			NowMs + HyperAIStudio::PIEPlaytest::Private::WorldReadyTimeoutMs);
		return;

	case EHyperAIStudioPIERuntimeState::StartingSession:
	case EHyperAIStudioPIERuntimeState::WaitingForWorld:
	{
		if (WorldDeadlineMs > 0 && NowMs >= WorldDeadlineMs)
		{
			BeginTeardown(TEXT("world_timeout"),
				TEXT("The observable in-process PIE world did not become ready before its lifecycle deadline."),
				false);
			return;
		}
		const FHyperAIStudioPIEDriverResult Result = ActiveDriver->ObserveWorld(Current.Plan);
		if (Result.Outcome == EHyperAIStudioPIEDriverOutcome::Pending)
		{
			Current.Session = Result.Session;
			return;
		}
		if (Result.Outcome == EHyperAIStudioPIEDriverOutcome::Succeeded)
		{
			Current.Session = Result.Session;
			FHyperAIPIEPlaytestEvidence Ready;
			Ready.Kind = TEXT("world_ready");
			Ready.Status = TEXT("completed");
			Ready.ValuePreview = Result.Session.Fingerprint;
			AppendEvidence(MoveTemp(Ready));
			Current.State = EHyperAIStudioPIERuntimeState::ExecutingStep;
			Current.Status = TEXT("running");
			StepStartedMs = 0;
			return;
		}
		HandleDriverResult(Result, Current.State == EHyperAIStudioPIERuntimeState::StartingSession);
		return;
	}

	case EHyperAIStudioPIERuntimeState::ExecutingStep:
	{
		if (ActiveStepIndex >= Current.Plan.Request.Steps.Num())
		{
			BeginTeardown(TEXT("completed"), TEXT("Every typed step completed; deterministic teardown is running."), false);
			return;
		}
		const FHyperAIPIEPlaytestStep& Step = Current.Plan.Request.Steps[ActiveStepIndex];
		if (StepStartedMs == 0)
		{
			StepStartedMs = NowMs;
		}
		if (NowMs - StepStartedMs >= Step.TimeoutMs)
		{
			const bool bAmbiguous = bStepDispatched
				&& FHyperAIStudioPIEPlaytestContracts::IsEffectStep(Step.Kind);
			BeginTeardown(TEXT("step_timeout"),
				FString::Printf(TEXT("Step '%s' exceeded its monotonic deadline."), *Step.StepId),
				bAmbiguous);
			return;
		}
		if (!bStepDispatched && FHyperAIStudioPIEPlaytestContracts::IsEffectStep(Step.Kind))
		{
			FString BarrierError;
			if (!EnsureEffectBarrier(BarrierError))
			{
				BeginTeardown(TEXT("effect_barrier_failed"), BarrierError, false);
				return;
			}
		}
		const FHyperAIStudioPIEDriverResult Result = bStepDispatched
			? ActiveDriver->PollStep(Current.Plan, Step)
			: ActiveDriver->BeginStep(Current.Plan, Step, MakeBinding(), ActiveTrustedGate.Get());
		bStepDispatched = true;
		if (Result.Outcome == EHyperAIStudioPIEDriverOutcome::Pending)
		{
			return;
		}
		HandleDriverResult(Result, FHyperAIStudioPIEPlaytestContracts::IsEffectStep(Step.Kind));
		return;
	}
	default:
		BeginTeardown(TEXT("invalid_runtime_state"),
			TEXT("The serial PIE runtime entered an invalid nonterminal state."), Current.bCommitStarted);
		return;
	}
}

void FHyperAIStudioPIEPlaytestRuntime::HandleDriverResult(
	const FHyperAIStudioPIEDriverResult& Result,
	const bool bWasEffect)
{
	FHyperAIPIEPlaytestEvidence Evidence = Result.Evidence;
	if (Current.State == EHyperAIStudioPIERuntimeState::ExecutingStep
		&& ActiveStepIndex < Current.Plan.Request.Steps.Num())
	{
		const FHyperAIPIEPlaytestStep& Step = Current.Plan.Request.Steps[ActiveStepIndex];
		if (Evidence.StepId.IsEmpty()) { Evidence.StepId = Step.StepId; }
		if (Evidence.Kind.IsEmpty()) { Evidence.Kind = FHyperAIStudioPIEPlaytestContracts::StepKindToString(Step.Kind); }
		Evidence.bEffect = bWasEffect;
		Evidence.ElapsedMs = ActiveClock
			? FMath::Max<int32>(0, static_cast<int32>(ActiveClock->NowMonotonicMs() - StepStartedMs)) : 0;
	}
	if (Evidence.Status.IsEmpty())
	{
		Evidence.Status = Result.Outcome == EHyperAIStudioPIEDriverOutcome::Succeeded
			? TEXT("completed") : TEXT("failed");
	}
	if (Evidence.DiagnosticCode.IsEmpty()) { Evidence.DiagnosticCode = Result.Code; }
	if (Evidence.Diagnostic.IsEmpty()) { Evidence.Diagnostic = Result.Diagnostic; }
	if (!Evidence.Kind.IsEmpty()) { AppendEvidence(MoveTemp(Evidence)); }

	if (Result.Outcome == EHyperAIStudioPIEDriverOutcome::Succeeded)
	{
		if (Current.State == EHyperAIStudioPIERuntimeState::ExecutingStep)
		{
			++ActiveStepIndex;
			Current.CompletedSteps = ActiveStepIndex;
			bStepDispatched = false;
			StepStartedMs = 0;
		}
		return;
	}
	if (Result.Outcome == EHyperAIStudioPIEDriverOutcome::UnsupportedMultiprocess)
	{
		Current.Session = Result.Session;
		BeginTeardown(TEXT("unsupported_multiprocess"), Result.Diagnostic, false);
		return;
	}
	const bool bAmbiguous = Result.Outcome == EHyperAIStudioPIEDriverOutcome::OutcomeUnknown;
	BeginTeardown(Result.Code.IsEmpty() ? TEXT("step_failed") : Result.Code,
		Result.Diagnostic.IsEmpty() ? TEXT("The typed PIE driver reported a failure.") : Result.Diagnostic,
		bAmbiguous);
}

void FHyperAIStudioPIEPlaytestRuntime::BeginTeardown(
	const FString& TerminalStatus,
	const FString& Diagnostic,
	const bool bAmbiguous)
{
	if (IsTerminal() || Current.State == EHyperAIStudioPIERuntimeState::Teardown)
	{
		return;
	}
	PendingTerminalStatus = HyperAIStudio::PIEPlaytest::Private::Clip(TerminalStatus, 64);
	PendingTerminalDiagnostic = HyperAIStudio::PIEPlaytest::Private::Clip(
		Diagnostic, HyperAIStudio::PIEPlaytest::Private::MaxDiagnosticCharacters);
	bPendingAmbiguous = bAmbiguous;
	Current.State = EHyperAIStudioPIERuntimeState::Teardown;
	Current.Status = TEXT("teardown");
	Current.Diagnostic = PendingTerminalDiagnostic;
	bTeardownDispatched = false;
	TeardownDeadlineMs = ActiveClock
		? ActiveClock->NowMonotonicMs() + HyperAIStudio::PIEPlaytest::Private::TeardownTimeoutMs : 0;
}

void FHyperAIStudioPIEPlaytestRuntime::FinishAfterTeardown(
	const FHyperAIStudioPIEDriverResult& Result)
{
	if (Result.Outcome == EHyperAIStudioPIEDriverOutcome::Pending)
	{
		return;
	}
	if (Result.Outcome != EHyperAIStudioPIEDriverOutcome::Succeeded)
	{
		bPendingAmbiguous = Current.bCommitStarted;
		PendingTerminalStatus = TEXT("teardown_failed");
		PendingTerminalDiagnostic = Result.Diagnostic.IsEmpty()
			? TEXT("The typed driver could not prove deterministic teardown.") : Result.Diagnostic;
	}

	if (PendingTerminalStatus == TEXT("completed"))
	{
		if (!Current.bCommitStarted)
		{
			FinishTerminal(EHyperAIStudioPIERuntimeState::Completed,
				TEXT("completed"), TEXT("Read-only playtest completed with bounded immutable evidence."));
			return;
		}
		FHyperAIStudioPlanOutcomeEvidence Evidence;
		FString Error;
		if (!ActiveTrustedGate
			|| !ActiveTrustedGate->IssueTerminalEvidence(
				MakeBinding(), ComputeEvidenceHash(), false, Evidence, Error)
			|| !ActiveTrustedGate->Transition(
				MakeBinding(), EHyperAIStudioPlanJournalTransition::Completed, Evidence, Error))
		{
			if (ActiveTrustedGate)
			{
				FString Ignored;
				ActiveTrustedGate->Transition(MakeBinding(),
					EHyperAIStudioPlanJournalTransition::OutcomeUnknown, {}, Ignored);
			}
			FinishTerminal(EHyperAIStudioPIERuntimeState::OutcomeUnknown,
				TEXT("outcome_unknown"), Error.IsEmpty()
					? TEXT("Fresh terminal evidence could not be issued/committed after runtime effects.") : Error);
			return;
		}
		FinishTerminal(EHyperAIStudioPIERuntimeState::Completed,
			TEXT("completed"), TEXT("Playtest effects and deterministic teardown have trusted terminal evidence."));
		return;
	}

	if (!Current.bCommitStarted)
	{
		if (Current.Plan.bHasEffects && ActiveTrustedGate)
		{
			FString Ignored;
			ActiveTrustedGate->Transition(MakeBinding(),
				EHyperAIStudioPlanJournalTransition::FailedPreCommit, {}, Ignored);
		}
		FinishTerminal(EHyperAIStudioPIERuntimeState::Failed,
			PendingTerminalStatus, PendingTerminalDiagnostic);
		return;
	}
	if (bPendingAmbiguous)
	{
		FString Ignored;
		if (ActiveTrustedGate)
		{
			ActiveTrustedGate->Transition(MakeBinding(),
				EHyperAIStudioPlanJournalTransition::OutcomeUnknown, {}, Ignored);
		}
		FinishTerminal(EHyperAIStudioPIERuntimeState::OutcomeUnknown,
			TEXT("outcome_unknown"), PendingTerminalDiagnostic);
		return;
	}

	FString Error;
	FHyperAIStudioPlanOutcomeEvidence RollbackEvidence;
	const bool bCanProveOwnedRollback = Current.bOwnsSession && ActiveTrustedGate
		&& ActiveTrustedGate->IssueTerminalEvidence(
			MakeBinding(), ComputeEvidenceHash(), true, RollbackEvidence, Error);
	if (bCanProveOwnedRollback
		&& ActiveTrustedGate->Transition(MakeBinding(),
			EHyperAIStudioPlanJournalTransition::RolledBack, RollbackEvidence, Error))
	{
		FinishTerminal(EHyperAIStudioPIERuntimeState::RolledBack,
			TEXT("rolled_back"), PendingTerminalDiagnostic);
		return;
	}
	FHyperAIStudioPlanOutcomeEvidence PartialEvidence;
	PartialEvidence.RollbackState = EHyperAIStudioPlanRollbackState::Unsupported;
	if (ActiveTrustedGate)
	{
		ActiveTrustedGate->Transition(MakeBinding(),
			EHyperAIStudioPlanJournalTransition::Partial, PartialEvidence, Error);
	}
	FinishTerminal(EHyperAIStudioPIERuntimeState::Partial,
		TEXT("partial"), Error.IsEmpty() ? PendingTerminalDiagnostic : Error);
}

void FHyperAIStudioPIEPlaytestRuntime::FinishTerminal(
	const EHyperAIStudioPIERuntimeState TerminalState,
	const FString& Status,
	const FString& Diagnostic)
{
	Current.State = TerminalState;
	Current.Status = HyperAIStudio::PIEPlaytest::Private::Clip(Status, 64);
	Current.Diagnostic = HyperAIStudio::PIEPlaytest::Private::Clip(
		Diagnostic, HyperAIStudio::PIEPlaytest::Private::MaxDiagnosticCharacters);
	Current.bOutcomeUnknown = TerminalState == EHyperAIStudioPIERuntimeState::OutcomeUnknown;
	if (ActiveClock) { Current.UpdatedUtc = ActiveClock->NowUtc(); }
}

void FHyperAIStudioPIEPlaytestRuntime::AppendEvidence(FHyperAIPIEPlaytestEvidence Evidence)
{
	if (Current.Evidence.Num() >= Current.Plan.Request.MaxEvidenceItems)
	{
		return;
	}
	Evidence.Sequence = Current.Evidence.Num() + 1;
	Evidence.StepId = HyperAIStudio::PIEPlaytest::Private::Clip(
		Evidence.StepId, FHyperAIStudioPIEPlaytestContracts::MaxStepIdCharacters);
	Evidence.Kind = HyperAIStudio::PIEPlaytest::Private::Clip(Evidence.Kind, 64);
	Evidence.Status = HyperAIStudio::PIEPlaytest::Private::Clip(Evidence.Status, 64);
	Evidence.DiagnosticCode = HyperAIStudio::PIEPlaytest::Private::Clip(Evidence.DiagnosticCode, 64);
	Evidence.Diagnostic = HyperAIStudio::PIEPlaytest::Private::Clip(
		Evidence.Diagnostic, HyperAIStudio::PIEPlaytest::Private::MaxDiagnosticCharacters);
	Evidence.ObjectPath = HyperAIStudio::PIEPlaytest::Private::Clip(
		Evidence.ObjectPath, FHyperAIStudioPIEPlaytestContracts::MaxObjectPathCharacters);
	Evidence.Name = HyperAIStudio::PIEPlaytest::Private::Clip(
		Evidence.Name, FHyperAIStudioPIEPlaytestContracts::MaxMarkerCharacters);
	Evidence.ValuePreview = HyperAIStudio::PIEPlaytest::Private::Clip(
		Evidence.ValuePreview, HyperAIStudio::PIEPlaytest::Private::MaxPreviewCharacters);
	Evidence.ScreenshotHash = HyperAIStudio::PIEPlaytest::Private::Clip(Evidence.ScreenshotHash, 96);
	Evidence.ItemCount = FMath::Clamp(Evidence.ItemCount, 0, FHyperAIStudioPIEPlaytestContracts::MaxActorScan);
	Evidence.ElapsedMs = FMath::Clamp(Evidence.ElapsedMs, 0, FHyperAIStudioPIEPlaytestContracts::MaxWholeTimeoutMs);
	Evidence.ScreenshotWidth = FMath::Clamp(
		Evidence.ScreenshotWidth, 0, FHyperAIStudioPIEPlaytestContracts::MaxScreenshotWidth);
	Evidence.ScreenshotHeight = FMath::Clamp(
		Evidence.ScreenshotHeight, 0, FHyperAIStudioPIEPlaytestContracts::MaxScreenshotHeight);
	Evidence.LogSequenceBefore = FMath::Max<int64>(0, Evidence.LogSequenceBefore);
	Evidence.LogSequenceAfter = FMath::Max<int64>(0, Evidence.LogSequenceAfter);
	Current.Evidence.Add(MoveTemp(Evidence));
}

FString FHyperAIStudioPIEPlaytestRuntime::ComputeEvidenceHash() const
{
	FString Canonical;
	HyperAIStudio::PIEPlaytest::Private::AppendCanonicalToken(Canonical, Current.Plan.PlanHash);
	HyperAIStudio::PIEPlaytest::Private::AppendCanonicalToken(Canonical, Current.Session.Fingerprint);
	HyperAIStudio::PIEPlaytest::Private::AppendCanonicalToken(Canonical,
		FHyperAIStudioPIEPlaytestContracts::ComputeEvidenceFingerprint(Current.Evidence));
	HyperAIStudio::PIEPlaytest::Private::AppendCanonicalToken(Canonical,
		FString::FromInt(Current.CompletedSteps));
	return FHyperAIStudioPlanExecuteContracts::ComputeBoundedSha256(Canonical);
}

FHyperAIStudioPIEExecutionBinding FHyperAIStudioPIEPlaytestRuntime::MakeBinding() const
{
	FHyperAIStudioPIEExecutionBinding Binding;
	Binding.CanonicalProjectId = CanonicalProjectId;
	Binding.OperationId = Current.Plan.Request.OperationId;
	Binding.PlanHash = Current.Plan.PlanHash;
	Binding.AuthorizationPlanHash = Current.Plan.AuthorizationPlanHash;
	Binding.CapabilityHash = Current.Plan.CapabilityHash;
	Binding.EffectFingerprint = Current.Plan.EffectFingerprint;
	Binding.AuthorizationToken = Current.Plan.Request.AuthorizationToken;
	Binding.Safety = Current.Plan.bHasDestructive
		? EHyperAIStudioPlanSafety::Destructive
		: (Current.Plan.bHasEffects ? EHyperAIStudioPlanSafety::ExternalEffect : EHyperAIStudioPlanSafety::Read);
	return Binding;
}

bool FHyperAIStudioPIEPlaytestRuntime::RequestCancel(
	const FString& Reason,
	FString& OutError)
{
	OutError.Reset();
	if (IsTerminal())
	{
		OutError = TEXT("The playtest is already terminal.");
		return false;
	}
	const bool bActiveEffectPending = Current.State == EHyperAIStudioPIERuntimeState::ExecutingStep
			&& bStepDispatched && ActiveStepIndex < Current.Plan.Request.Steps.Num()
			&& FHyperAIStudioPIEPlaytestContracts::IsEffectStep(
				Current.Plan.Request.Steps[ActiveStepIndex].Kind);
	BeginTeardown(TEXT("cancelled"), Reason.IsEmpty()
		? TEXT("Cancellation requested.") : Reason, bActiveEffectPending);
	return true;
}

void FHyperAIStudioPIEPlaytestRuntime::Shutdown()
{
	if (!IsTerminal() && ActiveDriver)
	{
		ActiveDriver->ForceTeardown(Current.Plan, Current.bOwnsSession);
		if (Current.bCommitStarted && ActiveTrustedGate)
		{
			FString Ignored;
			ActiveTrustedGate->Transition(MakeBinding(),
				EHyperAIStudioPlanJournalTransition::OutcomeUnknown, {}, Ignored);
		}
		FinishTerminal(Current.bCommitStarted
			? EHyperAIStudioPIERuntimeState::OutcomeUnknown : EHyperAIStudioPIERuntimeState::Failed,
			Current.bCommitStarted ? TEXT("outcome_unknown") : TEXT("shutdown_before_effect"),
			TEXT("Plugin shutdown interrupted the playtest; owned cleanup was requested synchronously."));
	}
}

bool FHyperAIStudioPIEPlaytestRuntime::IsTerminal() const
{
	switch (Current.State)
	{
	case EHyperAIStudioPIERuntimeState::Completed:
	case EHyperAIStudioPIERuntimeState::Failed:
	case EHyperAIStudioPIERuntimeState::Partial:
	case EHyperAIStudioPIERuntimeState::RolledBack:
	case EHyperAIStudioPIERuntimeState::OutcomeUnknown:
	case EHyperAIStudioPIERuntimeState::ReplayCompleted:
		return true;
	default:
		return false;
	}
}

FHyperAIStudioPIERuntimeSnapshot FHyperAIStudioPIEPlaytestRuntime::Snapshot() const
{
	return Current;
}

namespace HyperAIStudio::PIEPlaytest::Private
{
	struct FStoredRun
	{
		TSharedPtr<FHyperAIStudioPIEPlaytestRuntime> Runtime;
		FString PlanHash;
		int64 Serial = 0;
	};

	struct FPIEPlaytestServiceState
	{
		FCriticalSection Mutex;
		TMap<FString, FStoredRun> Runs;
		TSharedPtr<IHyperAIStudioPIETrustedExecutionGate> TrustedGate;
		FTSTicker::FDelegateHandle TickerHandle;
		int64 NextSerial = 1;
		int64 NextPumpSerial = 1;
		bool bStarted = false;
	};

	static_assert(std::is_same_v<decltype(FPIEPlaytestServiceState::TickerHandle), FTSTicker::FDelegateHandle>,
		"PIE ticker storage must use FTSTicker's exact opaque handle type.");

	FPIEPlaytestServiceState& GetServiceState()
	{
		static FPIEPlaytestServiceState State;
		return State;
	}

	bool IsTerminalState(const EHyperAIStudioPIERuntimeState State)
	{
		switch (State)
		{
		case EHyperAIStudioPIERuntimeState::Completed:
		case EHyperAIStudioPIERuntimeState::Failed:
		case EHyperAIStudioPIERuntimeState::Partial:
		case EHyperAIStudioPIERuntimeState::RolledBack:
		case EHyperAIStudioPIERuntimeState::OutcomeUnknown:
		case EHyperAIStudioPIERuntimeState::ReplayCompleted:
			return true;
		default:
			return false;
		}
	}

	FHyperAIPIEPlaytestRunResult MakeRunResult(const FHyperAIStudioPIERuntimeSnapshot& Snapshot)
	{
		FHyperAIPIEPlaytestRunResult Result;
		Result.bAccepted = Snapshot.bAccepted;
		Result.bTerminal = IsTerminalState(Snapshot.State);
		Result.bReplay = Snapshot.bReplay;
		Result.bOutcomeUnknown = Snapshot.bOutcomeUnknown;
		Result.Status = Snapshot.Status;
		Result.Diagnostic = Snapshot.Diagnostic;
		Result.OperationId = Snapshot.Plan.Request.OperationId;
		Result.PlanHash = Snapshot.Plan.PlanHash;
		Result.AuthorizationPlanHash = Snapshot.Plan.AuthorizationPlanHash;
		Result.CapabilityHash = Snapshot.Plan.CapabilityHash;
		Result.EffectFingerprint = Snapshot.Plan.EffectFingerprint;
		Result.State = FHyperAIStudioPIEPlaytestContracts::RuntimeStateToString(Snapshot.State);
		Result.SessionFingerprint = Snapshot.Session.Fingerprint;
		Result.bOwnsSession = Snapshot.bOwnsSession;
		Result.CompletedSteps = Snapshot.CompletedSteps;
		Result.TotalSteps = Snapshot.Plan.Request.Steps.Num();
		Result.bOk = Result.bAccepted
			&& Snapshot.State != EHyperAIStudioPIERuntimeState::Failed
			&& Snapshot.State != EHyperAIStudioPIERuntimeState::Partial
			&& Snapshot.State != EHyperAIStudioPIERuntimeState::OutcomeUnknown;
		return Result;
	}

	int32 EstimateEvidenceBytes(const FHyperAIPIEPlaytestEvidence& Evidence)
	{
		return 256 + (Evidence.StepId.Len() + Evidence.Kind.Len() + Evidence.Status.Len()
			+ Evidence.DiagnosticCode.Len() + Evidence.Diagnostic.Len() + Evidence.ObjectPath.Len()
			+ Evidence.Name.Len() + Evidence.ValuePreview.Len() + Evidence.ScreenshotHash.Len()) * sizeof(TCHAR);
	}

	bool TickService(float)
	{
		TArray<TSharedPtr<FHyperAIStudioPIEPlaytestRuntime>> Active;
		{
			FPIEPlaytestServiceState& State = GetServiceState();
			FScopeLock Lock(&State.Mutex);
			struct FPumpCandidate
			{
				int64 Serial = 0;
				TSharedPtr<FHyperAIStudioPIEPlaytestRuntime> Runtime;
			};
			TArray<FPumpCandidate> Candidates;
			for (const TPair<FString, FStoredRun>& Pair : State.Runs)
			{
				if (Pair.Value.Runtime && !Pair.Value.Runtime->IsTerminal())
				{
					Candidates.Add({ Pair.Value.Serial, Pair.Value.Runtime });
				}
			}
			Candidates.Sort([](const FPumpCandidate& A, const FPumpCandidate& B)
			{
				return A.Serial < B.Serial;
			});
			if (!Candidates.IsEmpty())
			{
				int32 StartIndex = Candidates.IndexOfByPredicate([&State](const FPumpCandidate& Candidate)
				{
					return Candidate.Serial >= State.NextPumpSerial;
				});
				if (StartIndex == INDEX_NONE) { StartIndex = 0; }
				const int32 PumpCount = FMath::Min(MaxRunsPumpedPerTick, Candidates.Num());
				int64 LastSerial = 0;
				for (int32 Offset = 0; Offset < PumpCount; ++Offset)
				{
					const FPumpCandidate& Candidate = Candidates[(StartIndex + Offset) % Candidates.Num()];
					Active.Add(Candidate.Runtime);
					LastSerial = Candidate.Serial;
				}
				State.NextPumpSerial = LastSerial == MAX_int64 ? 1 : LastSerial + 1;
			}
		}
		for (const TSharedPtr<FHyperAIStudioPIEPlaytestRuntime>& Runtime : Active)
		{
			Runtime->Tick();
		}
		return true;
	}

	bool RemoveOldestTerminalRun(FPIEPlaytestServiceState& State)
	{
		FString OldestKey;
		int64 OldestSerial = MAX_int64;
		for (const TPair<FString, FStoredRun>& Pair : State.Runs)
		{
			if (Pair.Value.Runtime && Pair.Value.Runtime->IsTerminal()
				&& Pair.Value.Serial < OldestSerial)
			{
				OldestSerial = Pair.Value.Serial;
				OldestKey = Pair.Key;
			}
		}
		return !OldestKey.IsEmpty() && State.Runs.Remove(OldestKey) == 1;
	}
}

void FHyperAIStudioPIEPlaytestService::Startup()
{
	using namespace HyperAIStudio::PIEPlaytest::Private;
	if (!IsInGameThread())
	{
		return;
	}
	FPIEPlaytestServiceState& State = GetServiceState();
	FScopeLock Lock(&State.Mutex);
	if (State.bStarted)
	{
		return;
	}
	State.bStarted = true;
	State.TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateStatic(&TickService), 0.0f);
}

void FHyperAIStudioPIEPlaytestService::Shutdown()
{
	using namespace HyperAIStudio::PIEPlaytest::Private;
	FPIEPlaytestServiceState& State = GetServiceState();
	TArray<TSharedPtr<FHyperAIStudioPIEPlaytestRuntime>> Runs;
	FTSTicker::FDelegateHandle TickerHandle;
	{
		FScopeLock Lock(&State.Mutex);
		TickerHandle = State.TickerHandle;
		State.TickerHandle.Reset();
		for (const TPair<FString, FStoredRun>& Pair : State.Runs)
		{
			if (Pair.Value.Runtime) { Runs.Add(Pair.Value.Runtime); }
		}
		State.Runs.Reset();
		State.TrustedGate.Reset();
		State.NextSerial = 1;
		State.NextPumpSerial = 1;
		State.bStarted = false;
	}
	if (TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
	}
	for (const TSharedPtr<FHyperAIStudioPIEPlaytestRuntime>& Runtime : Runs)
	{
		Runtime->Shutdown();
	}
	FHyperAIStudioPIEOptionalVariantRegistry::ResetAll();
}

FHyperAIPIEPlaytestRunResult FHyperAIStudioPIEPlaytestService::Submit(
	const FHyperAIPIEPlaytestRequest& Request)
{
	using namespace HyperAIStudio::PIEPlaytest::Private;
	FHyperAIPIEPlaytestRunResult Failure;
	Failure.OperationId = Clip(Request.OperationId, 128);
	if (!IsInGameThread())
	{
		Failure.Status = TEXT("wrong_thread");
		Failure.Diagnostic = TEXT("Playtest submission is game-thread only.");
		return Failure;
	}
	FHyperAIStudioPIENormalizedPlan Plan;
	FString ErrorCode;
	FString Error;
	if (!FHyperAIStudioPIEPlaytestContracts::NormalizeRequest(
		Request, Plan, ErrorCode, Error))
	{
		Failure.Status = ErrorCode;
		Failure.Diagnostic = Error;
		return Failure;
	}
	Failure.PlanHash = Plan.PlanHash;
	Failure.AuthorizationPlanHash = Plan.AuthorizationPlanHash;
	Failure.CapabilityHash = Plan.CapabilityHash;
	Failure.EffectFingerprint = Plan.EffectFingerprint;

	FPIEPlaytestServiceState& State = GetServiceState();
	TSharedPtr<IHyperAIStudioPIETrustedExecutionGate> TrustedGate;
	{
		FScopeLock Lock(&State.Mutex);
		if (const FStoredRun* Existing = State.Runs.Find(Request.OperationId))
		{
			if (Existing->PlanHash != Plan.PlanHash)
			{
				Failure.Status = TEXT("operation_conflict");
				Failure.Diagnostic = TEXT("operation_id is already bound to a different canonical playtest plan.");
				return Failure;
			}
			FHyperAIPIEPlaytestRunResult Replay = MakeRunResult(Existing->Runtime->Snapshot());
			Replay.bReplay = Replay.bTerminal;
			Replay.Status = Replay.bTerminal ? TEXT("idempotent_replay") : TEXT("already_in_progress");
			return Replay;
		}
		while (State.Runs.Num() >= MaxStoredRuns && RemoveOldestTerminalRun(State))
		{
		}
		if (State.Runs.Num() >= MaxStoredRuns)
		{
			Failure.Status = TEXT("capacity_exceeded");
			Failure.Diagnostic = TEXT("Every bounded operation slot is occupied by nonterminal work.");
			return Failure;
		}
		TrustedGate = State.TrustedGate;
	}

	const FString CanonicalProjectId = FHyperAIStudioOperationJournal::MakeCanonicalProjectId(
		FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
	if (Plan.bHasEffects && CanonicalProjectId.IsEmpty())
	{
		Failure.Status = TEXT("project_identity_unavailable");
		Failure.Diagnostic = TEXT("A strong physical project identity is required before runtime effects.");
		return Failure;
	}
	TSharedPtr<FHyperAIStudioPIEPlaytestRuntime> Runtime =
		MakeShared<FHyperAIStudioPIEPlaytestRuntime>();
	if (!Runtime->Start(
		Plan,
		CanonicalProjectId,
		MakeShared<FUnrealPIEPlaytestDriver>(),
		TrustedGate,
		MakeShared<FSystemPIEClock>(),
		Error))
	{
		const FHyperAIStudioPIERuntimeSnapshot Snapshot = Runtime->Snapshot();
		FHyperAIPIEPlaytestRunResult Result = MakeRunResult(Snapshot);
		if (Result.Status.IsEmpty()) { Result.Status = TEXT("rejected"); }
		if (Result.Diagnostic.IsEmpty()) { Result.Diagnostic = Error; }
		return Result;
	}
	{
		FScopeLock Lock(&State.Mutex);
		FStoredRun& Stored = State.Runs.Add(Request.OperationId);
		Stored.Runtime = Runtime;
		Stored.PlanHash = Plan.PlanHash;
		Stored.Serial = State.NextSerial++;
	}
	return MakeRunResult(Runtime->Snapshot());
}

FHyperAIPIEPlaytestStatusResult FHyperAIStudioPIEPlaytestService::GetStatus(
	const FString& OperationId,
	const int32 PageSize,
	const FString& Cursor)
{
	using namespace HyperAIStudio::PIEPlaytest::Private;
	FHyperAIPIEPlaytestStatusResult Result;
	Result.OperationId = Clip(OperationId, 128);
	if (!IsInGameThread())
	{
		Result.Status = TEXT("wrong_thread");
		Result.Diagnostic = TEXT("Playtest status is game-thread only.");
		return Result;
	}
	if (!FHyperAIStudioOperationJournal::IsValidOperationId(OperationId)
		|| PageSize < 1 || PageSize > 64 || Cursor.Len() > MaxCursorCharacters)
	{
		Result.Status = TEXT("invalid_request");
		Result.Diagnostic = TEXT("operation_id, page_size (1..64), or cursor is invalid.");
		return Result;
	}
	TSharedPtr<FHyperAIStudioPIEPlaytestRuntime> Runtime;
	{
		FPIEPlaytestServiceState& State = GetServiceState();
		FScopeLock Lock(&State.Mutex);
		if (const FStoredRun* Stored = State.Runs.Find(OperationId))
		{
			Runtime = Stored->Runtime;
		}
	}
	if (!Runtime)
	{
		Result.bOk = true;
		Result.Status = TEXT("not_found");
		Result.Diagnostic = TEXT("No process-local playtest evidence exists; use hyper_operation_status for durable journal reconciliation.");
		return Result;
	}
	const FHyperAIStudioPIERuntimeSnapshot Snapshot = Runtime->Snapshot();
	Result.bOk = true;
	Result.bFound = true;
	Result.bTerminal = IsTerminalState(Snapshot.State);
	Result.bReplay = Snapshot.bReplay;
	Result.bOutcomeUnknown = Snapshot.bOutcomeUnknown;
	Result.Status = Snapshot.Status;
	Result.Diagnostic = Snapshot.Diagnostic;
	Result.State = FHyperAIStudioPIEPlaytestContracts::RuntimeStateToString(Snapshot.State);
	Result.PlanHash = Snapshot.Plan.PlanHash;
	Result.CapabilityHash = Snapshot.Plan.CapabilityHash;
	Result.EffectFingerprint = Snapshot.Plan.EffectFingerprint;
	Result.SessionFingerprint = Snapshot.Session.Fingerprint;
	Result.SessionContextHandle = Snapshot.Session.ContextHandle;
	Result.TopologySource = Snapshot.Session.TopologySource;
	Result.PieInstance = Snapshot.Session.PieInstance;
	Result.bOwnsSession = Snapshot.bOwnsSession;
	Result.bSessionActive = Snapshot.Session.bActive;
	Result.CompletedSteps = Snapshot.CompletedSteps;
	Result.TotalSteps = Snapshot.Plan.Request.Steps.Num();
	Result.StartedUtc = Snapshot.StartedUtc;
	Result.UpdatedUtc = Snapshot.UpdatedUtc;
	Result.EvidenceFingerprint = FHyperAIStudioPIEPlaytestContracts::ComputeEvidenceFingerprint(Snapshot.Evidence);
	Result.TotalEvidenceItems = Snapshot.Evidence.Num();
	Result.PageSize = PageSize;
	FString CursorError;
	if (!FHyperAIStudioPIEPlaytestContracts::ParseCursor(
		Cursor, Result.EvidenceFingerprint, Snapshot.Evidence.Num(), Result.PageOffset, CursorError))
	{
		Result.bOk = false;
		Result.Status = TEXT("invalid_cursor");
		Result.Diagnostic = CursorError;
		return Result;
	}
	int32 ApproximateBytes = 1024;
	int32 Index = Result.PageOffset;
	for (; Index < Snapshot.Evidence.Num() && Result.Evidence.Num() < PageSize; ++Index)
	{
		const int32 ItemBytes = EstimateEvidenceBytes(Snapshot.Evidence[Index]);
		if (!Result.Evidence.IsEmpty()
			&& ApproximateBytes + ItemBytes > Snapshot.Plan.Request.MaxOutputBytes)
		{
			break;
		}
		ApproximateBytes += ItemBytes;
		Result.Evidence.Add(Snapshot.Evidence[Index]);
	}
	Result.ReturnedEvidenceItems = Result.Evidence.Num();
	Result.bTruncated = Index < Snapshot.Evidence.Num();
	if (Result.bTruncated)
	{
		Result.NextCursor = FHyperAIStudioPIEPlaytestContracts::MakeCursor(
			Result.EvidenceFingerprint, Index);
	}
	return Result;
}

bool FHyperAIStudioPIEPlaytestService::Cancel(
	const FString& OperationId,
	const FString& Reason,
	FString& OutError)
{
	using namespace HyperAIStudio::PIEPlaytest::Private;
	if (!IsInGameThread())
	{
		OutError = TEXT("Cancellation is game-thread only.");
		return false;
	}
	TSharedPtr<FHyperAIStudioPIEPlaytestRuntime> Runtime;
	{
		FPIEPlaytestServiceState& State = GetServiceState();
		FScopeLock Lock(&State.Mutex);
		if (const FStoredRun* Stored = State.Runs.Find(OperationId))
		{
			Runtime = Stored->Runtime;
		}
	}
	if (!Runtime)
	{
		OutError = TEXT("No process-local playtest operation matches operation_id.");
		return false;
	}
	return Runtime->RequestCancel(Reason, OutError);
}

void FHyperAIStudioPIEPlaytestService::SetTrustedExecutionGate(
	TSharedPtr<IHyperAIStudioPIETrustedExecutionGate> Gate)
{
	using namespace HyperAIStudio::PIEPlaytest::Private;
	FPIEPlaytestServiceState& State = GetServiceState();
	FScopeLock Lock(&State.Mutex);
	for (const TPair<FString, FStoredRun>& Pair : State.Runs)
	{
		if (Pair.Value.Runtime && !Pair.Value.Runtime->IsTerminal())
		{
			UE_LOG(LogHyperAIStudioPIEPlaytest, Error,
				TEXT("Refused to replace the trusted PIE execution seam while work is nonterminal."));
			return;
		}
	}
	State.TrustedGate = MoveTemp(Gate);
}

void FHyperAIStudioPIEPlaytestService::ResetTrustedExecutionGate()
{
	SetTrustedExecutionGate(nullptr);
}

FHyperAIPIEPlaytestRunResult UHyperAIStudioPIEPlaytestToolset::hyper_playtest_run(
	const FHyperAIPIEPlaytestRequest& Request)
{
	return FHyperAIStudioPIEPlaytestService::Submit(Request);
}

FHyperAIPIEPlaytestStatusResult UHyperAIStudioPIEPlaytestToolset::hyper_playtest_status(
	const FString& OperationId,
	const int32 PageSize,
	const FString& Cursor)
{
	return FHyperAIStudioPIEPlaytestService::GetStatus(OperationId, PageSize, Cursor);
}

void FHyperAIStudioPIEPlaytestRegistration::Startup()
{
	if (bStarted)
	{
		return;
	}
	bStarted = true;
	PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(
		this, &FHyperAIStudioPIEPlaytestRegistration::RegisterAfterEngineInit);
	if (GIsRunning && GEngine && UObjectInitialized())
	{
		RegisterAfterEngineInit();
	}
}

void FHyperAIStudioPIEPlaytestRegistration::Shutdown()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}
	FHyperAIStudioPIEPlaytestService::Shutdown();
	if (bOwnsRegistration && IsInGameThread() && UObjectInitialized()
		&& UToolsetRegistry::IsAvailable())
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::UnregisterOwned(
			UHyperAIStudioPIEPlaytestToolset::StaticClass(),
			FHyperAIStudioPIEPlaytestContracts::GetQualifiedToolsetName(),
			Error))
		{
			UE_LOG(LogHyperAIStudioPIEPlaytest, Warning,
				TEXT("Could not unregister the owned PIE playtest toolset: %s"), *Error);
		}
	}
	bOwnsRegistration = false;
	bStarted = false;
}

bool FHyperAIStudioPIEPlaytestRegistration::IsRegistered() const
{
	return UObjectInitialized() && UToolsetRegistry::IsAvailable()
		&& FHyperAIStudioPIEPlaytestContracts::IsRegistrationAllowed(
			FHyperAIStudioPIEPlaytestContracts::IsPendingNativeToolsTestEnabled())
		&& FHyperAIStudioCapabilityRuntimeIndex::IsOwned(
			UHyperAIStudioPIEPlaytestToolset::StaticClass(),
			FHyperAIStudioPIEPlaytestContracts::GetQualifiedToolsetName());
}

void FHyperAIStudioPIEPlaytestRegistration::RegisterAfterEngineInit()
{
	if (!bStarted || !IsInGameThread() || IsEngineExitRequested() || !UObjectInitialized()
		|| !UToolsetRegistry::IsAvailable()
		|| !FHyperAIStudioPIEPlaytestContracts::IsRegistrationAllowed(
			FHyperAIStudioPIEPlaytestContracts::IsPendingNativeToolsTestEnabled()))
	{
		return;
	}
	if (!FHyperAIStudioCapabilityRuntimeIndex::IsOwned(
		UHyperAIStudioPIEPlaytestToolset::StaticClass(),
		FHyperAIStudioPIEPlaytestContracts::GetQualifiedToolsetName()))
	{
		FString Error;
		bOwnsRegistration = FHyperAIStudioCapabilityRuntimeIndex::RegisterOwnedToolsetClass(
			UHyperAIStudioPIEPlaytestToolset::StaticClass(),
			FHyperAIStudioPIEPlaytestContracts::GetQualifiedToolsetName(),
			Error);
		if (!bOwnsRegistration)
		{
			UE_LOG(LogHyperAIStudioPIEPlaytest, Error,
				TEXT("ToolsetRegistry rejected the owned PIE playtest toolset: %s"), *Error);
		}
	}
	if (FHyperAIStudioCapabilityRuntimeIndex::IsOwned(
		UHyperAIStudioPIEPlaytestToolset::StaticClass(),
		FHyperAIStudioPIEPlaytestContracts::GetQualifiedToolsetName()))
	{
		FHyperAIStudioPIEPlaytestService::Startup();
	}
}
