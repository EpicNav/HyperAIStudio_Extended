// Games by Hyper 2026.

#include "HyperAIStudioAudioToolset.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "CoreGlobals.h"
#include "DocumentTemplates/MetasoundFrontendDocumentTemplate.h"
#include "Engine/Engine.h"
#include "EngineDefines.h"
#include "HAL/PlatformTime.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "Internationalization/Text.h"
#include "Interfaces/MetasoundFrontendSourceInterface.h"
#include "Interfaces/IPluginManager.h"
#include "MetasoundDocumentInterface.h"
#include "MetasoundFrontendDataTypeRegistry.h"
#include "MetasoundFrontendDocument.h"
#include "MetasoundFrontendDocumentBuilder.h"
#include "MetasoundFrontendLiteral.h"
#include "MetasoundFrontendNodeClassRegistry.h"
#include "MetasoundFrontendRegistryKey.h"
#include "MetasoundPolymorphic.h"
#include "Misc/CoreDelegates.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "Sound/DialogueVoice.h"
#include "Sound/DialogueWave.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundClass.h"
#include "Sound/SoundConcurrency.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundMix.h"
#include "Sound/SoundWave.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/Package.h"
#include "UObject/PropertyOptional.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/TextProperty.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(HyperAIStudioAudioToolset)

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioAudio, Log, All);

namespace HyperAIStudio::Audio::Private
{
	constexpr int32 MinOutputBytes = 4096;
	constexpr int32 MaxTextCharacters = 512;
	constexpr int32 MaxIssueTextCharacters = 384;
	constexpr int32 MaxReferences = 64;
	constexpr int32 MaxMetaSoundPages = 64;
	constexpr int32 MaxMetaSoundNodes = 8192;
	constexpr int32 MaxMetaSoundEdges = 16384;
	constexpr int32 MaxMetaSoundVariables = 8192;
	constexpr int32 MaxMetaSoundVertices = 32768;
	constexpr int32 MaxMetaSoundVariableReferences = 32768;
	constexpr int32 MaxMetaSoundClasses = 16384;
	constexpr int32 MaxMetaSoundInterfaces = 1024;
	constexpr int32 MaxMetaSoundLiterals = 32768;
	constexpr int32 MaxPersistedProperties = 131072;
	constexpr int32 MaxPersistedElements = 262144;
	constexpr int32 MaxPersistedContainerElements = 32768;
	constexpr int32 MaxPersistedDepth = 16;
	constexpr int32 MaxPersistedTextCharacters = 65536;

	bool IsSparseContainerShapeBounded(const int32 Num, const int32 MaxIndex)
	{
		return Num >= 0 && Num <= MaxPersistedContainerElements
			&& MaxIndex >= 0 && MaxIndex <= MaxPersistedContainerElements
			&& Num <= MaxIndex;
	}

	bool IsMetaSoundTypeCastable(const FName FromType, const FName ToType)
	{
		PRAGMA_DISABLE_EXPERIMENTAL_WARNINGS
		const bool bCastable = Metasound::IsCastable(FromType, ToType);
		PRAGMA_ENABLE_EXPERIMENTAL_WARNINGS
		return bCastable;
	}

	struct FGateDefinition
	{
		const TCHAR* Family;
		const TCHAR* Plugin;
		const TCHAR* Module;
	};

	const TArray<FGateDefinition>& GetGateDefinitions()
	{
		static const TArray<FGateDefinition> Gates = {
			{TEXT("metasound"), TEXT("Metasound"), TEXT("MetasoundFrontend")},
			{TEXT("audio_modulation"), TEXT("AudioModulation"), TEXT("AudioModulation")},
			{TEXT("wave_table"), TEXT("WaveTable"), TEXT("WaveTable")},
			{TEXT("synthesis_effect"), TEXT("Synthesis"), TEXT("Synthesis")},
			{TEXT("synesthesia_nrt"), TEXT("AudioSynesthesia"), TEXT("AudioSynesthesia")},
			{TEXT("soundscape"), TEXT("Soundscape"), TEXT("Soundscape")},
			{TEXT("sound_utility"), TEXT("SoundUtilities"), TEXT("SoundUtilities")},
			{TEXT("sound_cue_template"), TEXT("SoundCueTemplates"), TEXT("SoundCueTemplates")},
			{TEXT("audio_gameplay_volume"), TEXT("AudioGameplayVolume"), TEXT("AudioGameplayVolume")},
			{TEXT("audio_widget"), TEXT("AudioWidgets"), TEXT("AudioWidgets")},
			{TEXT("audio_capture"), TEXT("AudioCapture"), TEXT("AudioCapture")},
			{TEXT("motor_sim"), TEXT("AudioMotorSim"), TEXT("AudioMotorSim")},
			{TEXT("motor_sim_components"), TEXT("AudioMotorSim"),
				TEXT("AudioMotorSimStandardComponents")}};
		return Gates;
	}

	FString Clip(const FString& Value, const int32 Maximum = MaxTextCharacters)
	{
		return Value.Len() <= Maximum ? Value : Value.Left(Maximum);
	}

	void AppendToken(FString& Canonical, const FString& Value)
	{
		Canonical += FString::Printf(TEXT("%d:"), Value.Len());
		Canonical += Value;
		Canonical += TEXT("|");
	}

	void AppendInt(FString& Canonical, const int64 Value)
	{
		AppendToken(Canonical, FString::Printf(TEXT("%lld"), Value));
	}

	void AppendDouble(FString& Canonical, const double Value)
	{
		AppendToken(Canonical, FString::Printf(TEXT("%.17g"), Value));
	}

	void AppendBool(FString& Canonical, const bool bValue)
	{
		AppendToken(Canonical, bValue ? TEXT("1") : TEXT("0"));
	}

	void AppendGuid(FString& Canonical, const FGuid& Value)
	{
		AppendToken(Canonical, Value.ToString(EGuidFormats::DigitsWithHyphensLower));
	}

	void AppendVector(FString& Canonical, const FVector& Value)
	{
		AppendDouble(Canonical, Value.X);
		AppendDouble(Canonical, Value.Y);
		AppendDouble(Canonical, Value.Z);
	}

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

	bool IsFiniteVector(const FVector& Value)
	{
		return FMath::IsFinite(Value.X) && FMath::IsFinite(Value.Y) && FMath::IsFinite(Value.Z);
	}

	bool IsSafeObjectPath(const FString& Path)
	{
		if (Path.IsEmpty() || Path.Len() > FHyperAIStudioAudioContracts::MaxPathCharacters
			|| !Path.StartsWith(TEXT("/Game/"), ESearchCase::CaseSensitive)
			|| Path.Contains(TEXT("..")) || Path.Contains(TEXT("\\"))
			|| Path.Contains(TEXT("*")) || Path.Contains(TEXT("?")))
		{
			return false;
		}
		for (const TCHAR Character : Path)
		{
			if (Character < TEXT(' ') || Character == TEXT('"')) return false;
		}
		return true;
	}

	bool IsSafeName(const FString& Value, const int32 Maximum = 128)
	{
		if (Value.IsEmpty() || Value.Len() > Maximum) return false;
		for (const TCHAR Character : Value)
		{
			if (!(FChar::IsAlnum(Character) || Character == TEXT('_') || Character == TEXT('-')
				|| Character == TEXT('.') || Character == TEXT(' '))) return false;
		}
		return true;
	}

	bool ValidateStringSet(
		const TArray<FString>& Values,
		const int32 Maximum,
		TFunctionRef<bool(const FString&)> Predicate,
		FString& OutError)
	{
		if (Values.Num() > Maximum)
		{
			OutError = TEXT("A bounded string-list limit was exceeded.");
			return false;
		}
		TSet<FString> Unique;
		for (const FString& Value : Values)
		{
			if (!Predicate(Value) || Unique.Contains(Value))
			{
				OutError = TEXT("A list contains an invalid or duplicate closed value.");
				return false;
			}
			Unique.Add(Value);
		}
		return true;
	}

	void AddIssue(
		TArray<FHyperAIAudioIssue>& Issues,
		const int32 Maximum,
		bool& bOutTruncated,
		const TCHAR* Code,
		const TCHAR* Severity,
		const FString& Variant,
		const FString& ObjectPath,
		const FString& StableId,
		const int32 OperationIndex,
		const FString& Message)
	{
		if (Issues.Num() >= Maximum)
		{
			bOutTruncated = true;
			return;
		}
		FHyperAIAudioIssue Issue;
		Issue.Code = Clip(Code, 96);
		Issue.Severity = Clip(Severity, 16);
		Issue.Variant = Clip(Variant, 96);
		Issue.ObjectPath = Clip(ObjectPath, MaxIssueTextCharacters);
		Issue.StableId = Clip(StableId, MaxIssueTextCharacters);
		Issue.OperationIndex = OperationIndex;
		Issue.Message = Clip(Message, MaxIssueTextCharacters);
		Issues.Add(MoveTemp(Issue));
	}

	void AddCaptureIssue(
		FHyperAIAudioValueSnapshot& Snapshot,
		const TCHAR* Code,
		const TCHAR* Severity,
		const FString& Variant,
		const FString& ObjectPath,
		const FString& Message)
	{
		bool bUnused = false;
		AddIssue(Snapshot.Issues, FHyperAIStudioAudioContracts::MaxIssues, bUnused,
			Code, Severity, Variant, ObjectPath, FString(), -1, Message);
	}

	const FGateDefinition* FindGateByFamily(const FString& Family)
	{
		return GetGateDefinitions().FindByPredicate([&](const FGateDefinition& Gate)
		{
			return Family == Gate.Family;
		});
	}

	FString OptionalFamilyForVariant(const FString& Variant)
	{
		if (Variant.StartsWith(TEXT("metasound"))) return TEXT("metasound");
		if (Variant.StartsWith(TEXT("modulation")) || Variant == TEXT("audio_modulation"))
			return TEXT("audio_modulation");
		if (Variant.StartsWith(TEXT("wavetable")) || Variant == TEXT("wave_table"))
			return TEXT("wave_table");
		if (Variant.StartsWith(TEXT("synthesis")) || Variant == TEXT("synthesis_effect"))
			return TEXT("synthesis_effect");
		if (Variant.StartsWith(TEXT("synesthesia")) || Variant == TEXT("synesthesia_nrt"))
			return TEXT("synesthesia_nrt");
		if (Variant.StartsWith(TEXT("soundscape"))) return TEXT("soundscape");
		if (Variant.StartsWith(TEXT("sound_utility")) || Variant == TEXT("sound_utility"))
			return TEXT("sound_utility");
		if (Variant.StartsWith(TEXT("sound_cue_template"))) return TEXT("sound_cue_template");
		if (Variant.StartsWith(TEXT("audio_gameplay_volume"))) return TEXT("audio_gameplay_volume");
		if (Variant.StartsWith(TEXT("audio_widget"))) return TEXT("audio_widget");
		if (Variant.StartsWith(TEXT("audio_capture"))) return TEXT("audio_capture");
		if (Variant == TEXT("motor_sim.add_component")
			|| Variant == TEXT("motor_sim.remove_component"))
			return TEXT("motor_sim_components");
		if (Variant.StartsWith(TEXT("motor_sim"))) return TEXT("motor_sim");
		return FString();
	}

	FHyperAIAudioPrerequisite ProbeGate(const FGateDefinition& Gate)
	{
		FHyperAIAudioPrerequisite Result;
		Result.Family = Gate.Family;
		Result.Plugin = Gate.Plugin;
		Result.Module = Gate.Module;
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(Gate.Plugin);
		if (!Plugin.IsValid()) Result.State = TEXT("unavailable");
		else if (!Plugin->IsEnabled()) Result.State = TEXT("disabled");
		else if (!FModuleManager::Get().IsModuleLoaded(FName(Gate.Module)))
			Result.State = TEXT("not_loaded");
		else Result.State = TEXT("available");
		return Result;
	}

	bool IsGateReady(const FString& Family, FHyperAIAudioPrerequisite* Out = nullptr)
	{
		const FGateDefinition* Gate = FindGateByFamily(Family);
		if (!Gate) return Family.IsEmpty();
		const FHyperAIAudioPrerequisite Result = ProbeGate(*Gate);
		if (Out) *Out = Result;
		return Result.State == TEXT("available");
	}

	void CollectPrerequisites(TArray<FHyperAIAudioPrerequisite>& Out)
	{
		Out.Reset();
		for (const FGateDefinition& Gate : GetGateDefinitions()) Out.Add(ProbeGate(Gate));
		Out.Sort([](const FHyperAIAudioPrerequisite& Left, const FHyperAIAudioPrerequisite& Right)
		{
			return Left.Family < Right.Family;
		});
	}

	FTopLevelAssetPath ClassPath(const TCHAR* Package, const TCHAR* Class)
	{
		return FTopLevelAssetPath(FName(Package), FName(Class));
	}

	struct FAudioClassEvidence
	{
		FTopLevelAssetPath Exact;
		TSet<FTopLevelAssetPath> Ancestors;
		bool bAncestryComplete = true;

		bool Is(const TCHAR* Package, const TCHAR* Class) const
		{
			const FTopLevelAssetPath Candidate = ClassPath(Package, Class);
			return Exact == Candidate || Ancestors.Contains(Candidate);
		}
	};

	FAudioClassEvidence EvidenceFromObject(const UObject* Object)
	{
		FAudioClassEvidence Evidence;
		if (!Object) return Evidence;
		for (const UClass* Class = Object->GetClass(); Class; Class = Class->GetSuperClass())
		{
			if (!Evidence.Exact.IsValid()) Evidence.Exact = Class->GetClassPathName();
			else Evidence.Ancestors.Add(Class->GetClassPathName());
		}
		return Evidence;
	}

	FString VariantFromClassEvidence(const FAudioClassEvidence& E)
	{
		if (E.Is(TEXT("/Script/MetasoundEngine"), TEXT("MetaSoundSource"))) return TEXT("metasound_source");
		if (E.Is(TEXT("/Script/MetasoundEngine"), TEXT("MetaSoundPatch"))) return TEXT("metasound_patch");
		if (E.Is(TEXT("/Script/Engine"), TEXT("SoundSourceBus"))
			|| E.Is(TEXT("/Script/Engine"), TEXT("SoundWave"))) return TEXT("sound_wave");
		if (E.Is(TEXT("/Script/Engine"), TEXT("SoundCue"))) return TEXT("sound_cue");
		if (E.Is(TEXT("/Script/Engine"), TEXT("SoundClass"))) return TEXT("sound_class");
		if (E.Is(TEXT("/Script/Engine"), TEXT("SoundMix"))) return TEXT("sound_mix");
		if (E.Is(TEXT("/Script/Engine"), TEXT("SoundAttenuation"))) return TEXT("sound_attenuation");
		if (E.Is(TEXT("/Script/Engine"), TEXT("SoundConcurrency"))) return TEXT("sound_concurrency");
		if (E.Is(TEXT("/Script/Engine"), TEXT("DialogueWave"))) return TEXT("dialogue_wave");
		if (E.Is(TEXT("/Script/Engine"), TEXT("DialogueVoice"))) return TEXT("dialogue_voice");
		if (E.Is(TEXT("/Script/AudioModulation"), TEXT("SoundModulationParameter"))
			|| E.Is(TEXT("/Script/AudioModulation"), TEXT("SoundControlBus"))
			|| E.Is(TEXT("/Script/AudioModulation"), TEXT("SoundControlBusMix"))
			|| E.Is(TEXT("/Script/AudioModulation"), TEXT("SoundModulationPatch"))
			|| E.Is(TEXT("/Script/AudioModulation"), TEXT("SoundModulationGenerator"))) return TEXT("audio_modulation");
		if (E.Is(TEXT("/Script/WaveTable"), TEXT("WaveTableBank"))) return TEXT("wave_table");
		if (E.Is(TEXT("/Script/Engine"), TEXT("SoundEffectSourcePresetChain"))
			|| E.Is(TEXT("/Script/Engine"), TEXT("SoundEffectSourcePreset"))
			|| E.Is(TEXT("/Script/Engine"), TEXT("SoundEffectSubmixPreset"))) return TEXT("synthesis_effect");
		if (E.Is(TEXT("/Script/AudioSynesthesia"), TEXT("AudioSynesthesiaNRT"))
			|| E.Is(TEXT("/Script/AudioSynesthesia"), TEXT("AudioSynesthesiaNRTSettings"))) return TEXT("synesthesia_nrt");
		if (E.Is(TEXT("/Script/Soundscape"), TEXT("SoundscapePalette"))
			|| E.Is(TEXT("/Script/Soundscape"), TEXT("SoundscapeColor"))) return TEXT("soundscape");
		if (E.Is(TEXT("/Script/SoundUtilities"), TEXT("SoundSimple"))) return TEXT("sound_utility");
		if (E.Is(TEXT("/Script/SoundCueTemplates"), TEXT("SoundCueTemplate"))) return TEXT("sound_cue_template");
		if (E.Is(TEXT("/Script/AudioGameplayVolume"), TEXT("AudioGameplayVolume"))) return TEXT("audio_gameplay_volume");
		if (E.Is(TEXT("/Script/AudioWidgets"), TEXT("AudioMeter"))
			|| E.Is(TEXT("/Script/AudioWidgets"), TEXT("AudioRadialSlider"))) return TEXT("audio_widget");
		if (E.Is(TEXT("/Script/AudioCapture"), TEXT("AudioCapture"))) return TEXT("audio_capture");
		if (E.Is(TEXT("/Script/AudioMotorSim"), TEXT("AudioMotorSim"))
			|| E.Is(TEXT("/Script/AudioMotorSimStandardComponents"), TEXT("MotorSimComponent"))) return TEXT("motor_sim");
		return FString();
	}

	FString VariantFromObject(const UObject* Object)
	{
		if (!Object) return FString();

		// A loaded object is authoritative: classify core families with IsA so
		// Blueprint/native subclasses cannot be confused by display names. MetaSound
		// must be tested first because a Source is also a USoundWave.
		const FAudioClassEvidence Evidence = EvidenceFromObject(Object);
		if (Cast<IMetaSoundDocumentInterface>(Object))
		{
			if (Evidence.Is(TEXT("/Script/MetasoundEngine"), TEXT("MetaSoundSource")))
				return TEXT("metasound_source");
			if (Evidence.Is(TEXT("/Script/MetasoundEngine"), TEXT("MetaSoundPatch")))
				return TEXT("metasound_patch");
			return FString();
		}
		if (Object->IsA<USoundWave>()) return TEXT("sound_wave");
		if (Object->IsA<USoundCue>()) return TEXT("sound_cue");
		if (Object->IsA<USoundClass>()) return TEXT("sound_class");
		if (Object->IsA<USoundMix>()) return TEXT("sound_mix");
		if (Object->IsA<USoundAttenuation>()) return TEXT("sound_attenuation");
		if (Object->IsA<USoundConcurrency>()) return TEXT("sound_concurrency");
		if (Object->IsA<UDialogueWave>()) return TEXT("dialogue_wave");
		if (Object->IsA<UDialogueVoice>()) return TEXT("dialogue_voice");
		return VariantFromClassEvidence(Evidence);
	}

	struct FPersistedStateWalkResult
	{
		FString Fingerprint;
		TArray<FString> References;
		bool bComplete = true;
		FString Failure;
	};

	/**
	 * Bounded canonical traversal of reflected persisted state. Container values are
	 * hashed recursively (arrays ordered, sets/maps sorted), so same-count mutations
	 * cannot alias. It never resolves a soft path or loads an object.
	 */
	class FPersistedStateWalker final
	{
	public:
		explicit FPersistedStateWalker(
			const double InAbsoluteDeadline = 0.0,
			const bool bInDelegateMetaSoundDocument = true)
			: AbsoluteDeadline(InAbsoluteDeadline)
			, bDelegateMetaSoundDocument(bInDelegateMetaSoundDocument)
		{
		}

		FPersistedStateWalkResult WalkObject(const UObject* Object)
		{
			if (!IsValid(Object))
			{
				Fail(TEXT("invalid_object"));
				return Finish();
			}
			RootPackage = Object->GetPackage();
			ActiveObjects.Add(Object);
			FString Canonical;
			AppendToken(Canonical, TEXT("hyperai.persisted-reflection.v2"));
			AppendToken(Canonical, Object->GetClass()->GetPathName());
			const FString ObjectState = WalkStruct(Object->GetClass(), Object, 0);
			AppendToken(Canonical, ObjectState);
			ActiveObjects.Remove(Object);
			ObjectFingerprints.Add(Object, ObjectState);
			RootFingerprint = Hash(Canonical);
			return Finish();
		}

		FPersistedStateWalkResult WalkScriptStruct(const UScriptStruct* Struct, const void* Data)
		{
			if (!Struct || !Data)
			{
				Fail(TEXT("invalid_struct"));
				return Finish();
			}
			FString Canonical;
			AppendToken(Canonical, TEXT("hyperai.persisted-struct.v2"));
			AppendToken(Canonical, Struct->GetPathName());
			AppendToken(Canonical, WalkStruct(Struct, Data, 0));
			RootFingerprint = Hash(Canonical);
			return Finish();
		}

		FPersistedStateWalkResult WalkPropertyValue(
			const FProperty* Property,
			const void* Address,
			const UPackage* InRootPackage)
		{
			if (!Property || !Address)
			{
				Fail(TEXT("invalid_property_value"));
				return Finish();
			}
			if (AbsoluteDeadline > 0.0 && FPlatformTime::Seconds() > AbsoluteDeadline)
			{
				Fail(TEXT("deadline"));
				return Finish();
			}
			RootPackage = InRootPackage;
			FString Canonical;
			AppendToken(Canonical, TEXT("hyperai.persisted-property.v1"));
			AppendToken(Canonical, Property->GetClass()->GetName());
			AppendToken(Canonical, WalkValue(Property, Address, 0));
			RootFingerprint = Hash(Canonical);
			return Finish();
		}

	private:
		static constexpr EPropertyFlags ExcludedFlags = static_cast<EPropertyFlags>(
			CPF_Transient | CPF_DuplicateTransient | CPF_NonPIEDuplicateTransient
			| CPF_SkipSerialization | CPF_TextExportTransient);

		void Fail(const TCHAR* Reason)
		{
			bComplete = false;
			if (Failure.IsEmpty()) Failure = Reason;
		}

		bool SpendProperty()
		{
			if (++PropertiesVisited > MaxPersistedProperties)
			{
				Fail(TEXT("property_limit"));
				return false;
			}
			if (AbsoluteDeadline > 0.0 && FPlatformTime::Seconds() > AbsoluteDeadline)
			{
				Fail(TEXT("deadline"));
				return false;
			}
			return true;
		}

		bool SpendElements(const int32 Count)
		{
			if (Count < 0 || Count > MaxPersistedContainerElements
				|| ElementsVisited > MaxPersistedElements - Count)
			{
				Fail(TEXT("container_limit"));
				return false;
			}
			ElementsVisited += Count;
			if (AbsoluteDeadline > 0.0 && FPlatformTime::Seconds() > AbsoluteDeadline)
			{
				Fail(TEXT("deadline"));
				return false;
			}
			return true;
		}

		FString Hash(const FString& Canonical)
		{
			const FString Result = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
			if (!IsSha256(Result)) Fail(TEXT("canonical_hash_limit"));
			return Result;
		}

		FString FoldSequence(const FString& Kind, const TArray<FString>& Values)
		{
			FString Seed;
			AppendToken(Seed, TEXT("hyperai.persisted-fold.v1"));
			AppendToken(Seed, Kind);
			AppendInt(Seed, Values.Num());
			FString State = Hash(Seed);
			for (const FString& Value : Values)
			{
				if (AbsoluteDeadline > 0.0 && FPlatformTime::Seconds() > AbsoluteDeadline)
				{
					Fail(TEXT("deadline"));
					return FString();
				}
				FString Step;
				AppendToken(Step, State);
				AppendToken(Step, Value);
				State = Hash(Step);
			}
			return State;
		}

		void ObserveReference(const FString& Path)
		{
			if (!IsSafeObjectPath(Path)) return;
			if (!References.Contains(Path))
			{
				if (References.Num() >= MaxReferences)
				{
					Fail(TEXT("reference_limit"));
					return;
				}
				References.Add(Path);
			}
		}

		FString BoundedTextHash(const FString& Prefix, const FString& Value)
		{
			if (Value.Len() > MaxPersistedTextCharacters)
			{
				Fail(TEXT("text_limit"));
				return FString();
			}
			FString Canonical;
			AppendToken(Canonical, Prefix);
			AppendInt(Canonical, Value.Len());
			AppendToken(Canonical, Value);
			return Hash(Canonical);
		}

		FString WalkStruct(const UStruct* Struct, const void* Container, const int32 Depth)
		{
			if (!Struct || !Container || Depth > MaxPersistedDepth)
			{
				Fail(TEXT("struct_depth"));
				return FString();
			}
			TArray<FString> Fields;
			for (TFieldIterator<FProperty> It(Struct, EFieldIteratorFlags::IncludeSuper); It; ++It)
			{
				const FProperty* Property = *It;
				if (!Property || Property->HasAnyPropertyFlags(ExcludedFlags)) continue;
				if (!SpendProperty()) break;
				if (Property->ArrayDim > 1 && !SpendElements(Property->ArrayDim)) break;
			for (int32 StaticIndex = 0; StaticIndex < Property->ArrayDim; ++StaticIndex)
			{
				if (AbsoluteDeadline > 0.0 && FPlatformTime::Seconds() > AbsoluteDeadline)
				{
					Fail(TEXT("deadline"));
					break;
				}
				const void* Address = Property->ContainerPtrToValuePtr<void>(Container, StaticIndex);
					const FString Value = WalkValue(Property, Address, Depth + 1);
					FString Field;
					AppendToken(Field, Property->GetName());
					AppendInt(Field, StaticIndex);
					AppendToken(Field, Value);
					Fields.Add(Hash(Field));
				}
			}
			if (Fields.IsEmpty() && Struct->GetStructureSize() > 1)
				Fail(TEXT("opaque_struct"));
			Fields.Sort();
			return FoldSequence(TEXT("struct:") + Struct->GetPathName(), Fields);
		}

		FString WalkLiteralText(const FText& Text)
		{
			const FString* Source = FTextInspector::GetSourceString(Text);
			const FString& Display = FTextInspector::GetDisplayString(Text);
			if ((Source && Source->Len() > MaxPersistedTextCharacters)
				|| Display.Len() > MaxPersistedTextCharacters)
			{
				Fail(TEXT("text_limit"));
				return FString();
			}
			FString Persisted;
			FTextStringHelper::WriteToBuffer(Persisted, Text, false, false);
			return BoundedTextHash(TEXT("text"), Persisted);
		}

		FString WalkInstancedStruct(const FInstancedStruct& Value, const int32 Depth)
		{
			if (!Value.IsValid()) return BoundedTextHash(TEXT("instanced_struct"), TEXT("none"));
			FString Canonical;
			AppendToken(Canonical, Value.GetScriptStruct()->GetPathName());
			AppendToken(Canonical, WalkStruct(Value.GetScriptStruct(), Value.GetMemory(), Depth + 1));
			return Hash(Canonical);
		}

		FString WalkValue(const FProperty* Property, const void* Address, const int32 Depth)
		{
			if (!Property || !Address || Depth > MaxPersistedDepth)
			{
				Fail(TEXT("value_depth"));
				return FString();
			}
			if (const FOptionalProperty* Optional = CastField<FOptionalProperty>(Property))
			{
				if (!Optional->IsSet(Address)) return BoundedTextHash(TEXT("optional"), TEXT("unset"));
				return WalkValue(Optional->GetValueProperty(),
					Optional->GetValuePointerForRead(Address), Depth + 1);
			}
			if (const FArrayProperty* Array = CastField<FArrayProperty>(Property))
			{
				FScriptArrayHelper Helper(Array, Address);
				if (!SpendElements(Helper.Num())) return FString();
				TArray<FString> Elements;
				Elements.Reserve(Helper.Num());
				for (int32 Index = 0; Index < Helper.Num(); ++Index)
				{
					if (AbsoluteDeadline > 0.0 && FPlatformTime::Seconds() > AbsoluteDeadline)
					{
						Fail(TEXT("deadline"));
						return FString();
					}
					Elements.Add(WalkValue(Array->Inner, Helper.GetRawPtr(Index), Depth + 1));
				}
				return FoldSequence(TEXT("array"), Elements);
			}
			if (const FSetProperty* Set = CastField<FSetProperty>(Property))
			{
				const FScriptSetHelper Helper(Set, Address);
				const int32 MaxIndex = Helper.GetMaxIndex();
				if (!IsSparseContainerShapeBounded(Helper.Num(), MaxIndex))
				{
					Fail(TEXT("sparse_index_limit"));
					return FString();
				}
				if (!SpendElements(Helper.Num()) || !SpendElements(MaxIndex)) return FString();
				TArray<FString> Elements;
				int32 VisitedEntries = 0;
				for (int32 Index = 0; Index < MaxIndex; ++Index)
				{
					if (AbsoluteDeadline > 0.0 && FPlatformTime::Seconds() > AbsoluteDeadline)
					{
						Fail(TEXT("deadline"));
						return FString();
					}
					if (Helper.IsValidIndex(Index))
					{
						++VisitedEntries;
						Elements.Add(WalkValue(Set->ElementProp, Helper.GetElementPtr(Index), Depth + 1));
					}
				}
				if (VisitedEntries != Helper.Num())
				{
					Fail(TEXT("sparse_set_count_mismatch"));
					return FString();
				}
				Elements.Sort();
				return FoldSequence(TEXT("set"), Elements);
			}
			if (const FMapProperty* Map = CastField<FMapProperty>(Property))
			{
				FScriptMapHelper Helper(Map, Address);
				const int32 MaxIndex = Helper.GetMaxIndex();
				if (!IsSparseContainerShapeBounded(Helper.Num(), MaxIndex))
				{
					Fail(TEXT("sparse_index_limit"));
					return FString();
				}
				if (Helper.Num() > MaxPersistedContainerElements / 2
					|| !SpendElements(Helper.Num() * 2) || !SpendElements(MaxIndex)) return FString();
				TArray<FString> Pairs;
				int32 VisitedEntries = 0;
				for (int32 Index = 0; Index < MaxIndex; ++Index)
				{
					if (AbsoluteDeadline > 0.0 && FPlatformTime::Seconds() > AbsoluteDeadline)
					{
						Fail(TEXT("deadline"));
						return FString();
					}
					if (!Helper.IsValidIndex(Index)) continue;
					++VisitedEntries;
					FString Pair;
					AppendToken(Pair, WalkValue(Map->KeyProp, Helper.GetKeyPtr(Index), Depth + 1));
					AppendToken(Pair, WalkValue(Map->ValueProp, Helper.GetValuePtr(Index), Depth + 1));
					Pairs.Add(Hash(Pair));
				}
				if (VisitedEntries != Helper.Num())
				{
					Fail(TEXT("sparse_map_count_mismatch"));
					return FString();
				}
				Pairs.Sort();
				return FoldSequence(TEXT("map"), Pairs);
			}
			if (CastField<FSoftObjectProperty>(Property))
			{
				const FSoftObjectPath Path = static_cast<const FSoftObjectPtr*>(Address)->ToSoftObjectPath();
				const FString Value = Path.IsNull() ? TEXT("none") : Path.ToString();
				ObserveReference(Value);
				return BoundedTextHash(TEXT("soft_object"), Value);
			}
			if (const FTextProperty* TextProperty = CastField<FTextProperty>(Property))
				return WalkLiteralText(TextProperty->GetPropertyValue(Address));
			if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
			{
				const FNumericProperty* Underlying = EnumProperty->GetUnderlyingProperty();
				FString Canonical;
				AppendToken(Canonical, EnumProperty->GetEnum() ? EnumProperty->GetEnum()->GetPathName() : TEXT("none"));
				AppendToken(Canonical, Underlying->GetNumericPropertyValueToString(Address));
				return Hash(Canonical);
			}
			if (const FByteProperty* Byte = CastField<FByteProperty>(Property))
			{
				FString Canonical;
				AppendToken(Canonical, Byte->Enum ? Byte->Enum->GetPathName() : TEXT("byte"));
				AppendInt(Canonical, Byte->GetPropertyValue(Address));
				return Hash(Canonical);
			}
			if (const FBoolProperty* Bool = CastField<FBoolProperty>(Property))
				return BoundedTextHash(TEXT("bool"), Bool->GetPropertyValue(Address) ? TEXT("1") : TEXT("0"));
			if (const FNumericProperty* Numeric = CastField<FNumericProperty>(Property))
				return BoundedTextHash(TEXT("numeric"), Numeric->GetNumericPropertyValueToString(Address));
			if (const FNameProperty* Name = CastField<FNameProperty>(Property))
				return BoundedTextHash(TEXT("name"), Name->GetPropertyValue(Address).ToString());
			if (const FStrProperty* String = CastField<FStrProperty>(Property))
				return BoundedTextHash(TEXT("string"), String->GetPropertyValue(Address));
			if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
			{
				const FName StructName = StructProperty->Struct->GetFName();
				if (bDelegateMetaSoundDocument && StructName == TEXT("MetasoundFrontendDocument"))
					return BoundedTextHash(TEXT("delegated"), TEXT("metasound_frontend_document_v2"));
				if (StructName == TEXT("SoftObjectPath") || StructName == TEXT("SoftClassPath"))
				{
					const FString Value = static_cast<const FSoftObjectPath*>(Address)->ToString();
					ObserveReference(Value);
					return BoundedTextHash(TEXT("soft_path"), Value);
				}
				if (StructName == TEXT("TopLevelAssetPath"))
					return BoundedTextHash(TEXT("top_level_asset_path"),
						static_cast<const FTopLevelAssetPath*>(Address)->ToString());
				if (StructName == TEXT("InstancedStruct"))
					return WalkInstancedStruct(*static_cast<const FInstancedStruct*>(Address), Depth + 1);
				return WalkStruct(StructProperty->Struct, Address, Depth + 1);
			}
			if (CastField<FWeakObjectProperty>(Property) || CastField<FLazyObjectProperty>(Property))
			{
				Fail(TEXT("unsupported_weak_or_lazy_reference"));
				return BoundedTextHash(TEXT("unsupported_reference"), Property->GetClass()->GetName());
			}
			if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
			{
				const UObject* Reference = ObjectProperty->GetObjectPropertyValue(Address);
				const FString Value = Reference ? Reference->GetPathName() : TEXT("none");
				if (Value.Len() > FHyperAIStudioAudioContracts::MaxPathCharacters)
					Fail(TEXT("object_path_limit"));
				if (FHyperAIStudioAudioContracts::IsCanonicalProjectAssetPath(Value)) ObserveReference(Value);
				FString Canonical;
				AppendToken(Canonical, TEXT("object"));
				AppendToken(Canonical, Value);
				if (Reference && Reference->GetPackage() == RootPackage)
				{
					if (const FString* Cached = ObjectFingerprints.Find(Reference))
						AppendToken(Canonical, *Cached);
					else if (ActiveObjects.Contains(Reference))
					{
						Fail(TEXT("object_cycle"));
						AppendToken(Canonical, TEXT("cycle:") + Value);
					}
					else
					{
						ActiveObjects.Add(Reference);
						const FString Nested = WalkStruct(Reference->GetClass(), Reference, Depth + 1);
						ActiveObjects.Remove(Reference);
						ObjectFingerprints.Add(Reference, Nested);
						AppendToken(Canonical, Nested);
					}
				}
				return Hash(Canonical);
			}
			if (const FInterfaceProperty* InterfaceProperty = CastField<FInterfaceProperty>(Property))
			{
				const UObject* Reference = InterfaceProperty->GetPropertyValue(Address).GetObject();
				const FString Value = Reference ? Reference->GetPathName() : TEXT("none");
				ObserveReference(Value);
				return BoundedTextHash(TEXT("interface"), Value);
			}

			// Delegates, field paths, and new/custom property kinds must never silently
			// collapse out of a CAS revision.
			Fail(TEXT("unsupported_property"));
			return BoundedTextHash(TEXT("unsupported"), Property->GetClass()->GetName());
		}

		FPersistedStateWalkResult Finish()
		{
			References.Sort();
			FPersistedStateWalkResult Result;
			Result.Fingerprint = bComplete && IsSha256(RootFingerprint) ? RootFingerprint : FString();
			Result.References = MoveTemp(References);
			Result.bComplete = bComplete && IsSha256(Result.Fingerprint);
			Result.Failure = Failure;
			return Result;
		}

		double AbsoluteDeadline = 0.0;
		bool bDelegateMetaSoundDocument = true;
		int32 PropertiesVisited = 0;
		int32 ElementsVisited = 0;
		bool bComplete = true;
		FString Failure;
		FString RootFingerprint;
		TArray<FString> References;
		const UPackage* RootPackage = nullptr;
		TSet<const UObject*> ActiveObjects;
		TMap<const UObject*, FString> ObjectFingerprints;
	};

		bool IsTypedMetaSoundLiteral(
		const FMetasoundFrontendLiteral& Literal,
		const FName& DataType,
		bool& bOutDataTypeRegistered)
	{
		bOutDataTypeRegistered = false;
		if (DataType.IsNone() || !Literal.IsValid()) return false;
		Metasound::Frontend::IDataTypeRegistry& Registry =
			Metasound::Frontend::IDataTypeRegistry::Get();
		bOutDataTypeRegistered = Registry.IsRegistered(DataType);
		if (!bOutDataTypeRegistered
			|| !Registry.IsLiteralTypeSupported(DataType, Literal.GetType())) return false;
		if (Literal.GetType() != EMetasoundFrontendLiteralType::UObject
			&& Literal.GetType() != EMetasoundFrontendLiteralType::UObjectArray
			&& !Literal.ToLiteral(DataType).IsValid()) return false;
		if (Literal.GetType() == EMetasoundFrontendLiteralType::UObject)
		{
			UObject* Value = nullptr;
			return Literal.TryGet(Value)
				&& Registry.IsValidUObjectForDataType(DataType, Value);
		}
		if (Literal.GetType() == EMetasoundFrontendLiteralType::UObjectArray)
		{
			TArray<UObject*> Values;
			if (!Literal.TryGet(Values)) return false;
			for (const UObject* Value : Values)
				if (!Registry.IsValidUObjectForDataType(DataType, Value)) return false;
		}
			return true;
		}

		bool AreMetaSoundClassInterfacesRegistryExact(
			const FMetasoundFrontendClassInterface& Persisted,
			const FMetasoundFrontendClassInterface& Registered)
		{
			if (Persisted.Inputs.Num() != Registered.Inputs.Num()
				|| Persisted.Outputs.Num() != Registered.Outputs.Num()
				|| Persisted.Environment.Num() != Registered.Environment.Num()
				|| Persisted.Inputs.Num() + Persisted.Outputs.Num() + Persisted.Environment.Num()
					> MaxMetaSoundVertices) return false;
			for (const FMetasoundFrontendClassInput& Input : Persisted.Inputs)
			{
				const FMetasoundFrontendClassInput* Match = Registered.Inputs.FindByPredicate(
					[&](const FMetasoundFrontendClassInput& Candidate) { return Candidate.Name == Input.Name; });
				if (!Match || Input.VertexID != Match->VertexID || Input.TypeName != Match->TypeName
					|| Input.NodeID != Match->NodeID || Input.AccessType != Match->AccessType
					|| !FMetasoundFrontendClassVertex::IsFunctionalEquivalent(Input, *Match)
					|| Input.GetDefaults().Num() != Match->GetDefaults().Num()
					|| Input.GetDefaults().Num() > MaxMetaSoundLiterals) return false;
				TMap<FGuid, const FMetasoundFrontendClassInputDefault*> RegisteredDefaults;
				for (const FMetasoundFrontendClassInputDefault& Default : Match->GetDefaults())
				{
					if (RegisteredDefaults.Contains(Default.PageID)) return false;
					RegisteredDefaults.Add(Default.PageID, &Default);
				}
				for (const FMetasoundFrontendClassInputDefault& Default : Input.GetDefaults())
				{
					const FMetasoundFrontendClassInputDefault* const* MatchingDefault =
						RegisteredDefaults.Find(Default.PageID);
					if (!MatchingDefault || !*MatchingDefault
						|| !FMetasoundFrontendClassInputDefault::IsFunctionalEquivalent(
							Default, **MatchingDefault)) return false;
				}
			}
			for (const FMetasoundFrontendClassOutput& Output : Persisted.Outputs)
			{
				const FMetasoundFrontendClassOutput* Match = Registered.Outputs.FindByPredicate(
					[&](const FMetasoundFrontendClassOutput& Candidate) { return Candidate.Name == Output.Name; });
				if (!Match || Output.VertexID != Match->VertexID || Output.TypeName != Match->TypeName
					|| Output.NodeID != Match->NodeID || Output.AccessType != Match->AccessType
					|| !FMetasoundFrontendClassVertex::IsFunctionalEquivalent(Output, *Match)) return false;
			}
			for (const FMetasoundFrontendClassEnvironmentVariable& Environment : Persisted.Environment)
			{
				const FMetasoundFrontendClassEnvironmentVariable* Match = Registered.Environment.FindByPredicate(
					[&](const FMetasoundFrontendClassEnvironmentVariable& Candidate)
					{
						return Candidate.Name == Environment.Name;
					});
				if (!Match || Environment.TypeName != Match->TypeName
					|| Environment.bIsRequired != Match->bIsRequired) return false;
			}
			return true;
		}

		bool IsMetaSoundClassRegistryExact(
			const FMetasoundFrontendClass& Persisted,
			const FMetasoundFrontendClass& Registered,
			const Metasound::Frontend::FNodeRegistryKey& RegistryKey,
			const double AbsoluteDeadline = 0.0)
		{
			const FMetasoundFrontendClassMetadata& PersistedMetadata = Persisted.Metadata;
			const FMetasoundFrontendClassMetadata& RegisteredMetadata = Registered.Metadata;
			const Metasound::Frontend::FNodeRegistryKey RegisteredKey(
				RegisteredMetadata.GetType(), RegisteredMetadata.GetClassName(), RegisteredMetadata.GetVersion());
			const double EffectiveDeadline = AbsoluteDeadline > 0.0
				? AbsoluteDeadline : FPlatformTime::Seconds() + 0.05;
			const FPersistedStateWalkResult PersistedInterface = FPersistedStateWalker(
				EffectiveDeadline, false).WalkScriptStruct(
					FMetasoundFrontendClassInterface::StaticStruct(), &Persisted.GetDefaultInterface());
			const FPersistedStateWalkResult RegisteredInterface = FPersistedStateWalker(
				EffectiveDeadline, false).WalkScriptStruct(
					FMetasoundFrontendClassInterface::StaticStruct(), &Registered.GetDefaultInterface());
			return RegisteredKey == RegistryKey
				&& PersistedMetadata.GetType() == RegisteredMetadata.GetType()
				&& PersistedMetadata.GetClassName() == RegisteredMetadata.GetClassName()
				&& PersistedMetadata.GetVersion() == RegisteredMetadata.GetVersion()
				&& PersistedMetadata.GetAccessFlags() == RegisteredMetadata.GetAccessFlags()
				&& PersistedInterface.bComplete && RegisteredInterface.bComplete
				&& IsSha256(PersistedInterface.Fingerprint)
				&& IsSha256(RegisteredInterface.Fingerprint)
				&& PersistedInterface.Fingerprint == RegisteredInterface.Fingerprint
				&& AreMetaSoundClassInterfacesRegistryExact(
					Persisted.GetDefaultInterface(), Registered.GetDefaultInterface());
		}

		FString ComputeMetaSoundRegistryInterfaceFingerprint(
			const Metasound::Frontend::FNodeRegistryKey& RegistryKey,
			const FMetasoundFrontendClass& RegisteredClass,
			const double AbsoluteDeadline = 0.0)
		{
			FString Canonical;
			AppendToken(Canonical, TEXT("hyperai.metasound-registry-interface.v1"));
			AppendToken(Canonical, RegistryKey.ToString());
			AppendInt(Canonical, static_cast<int32>(RegisteredClass.Metadata.GetAccessFlags()));
			const double EffectiveDeadline = AbsoluteDeadline > 0.0
				? AbsoluteDeadline : FPlatformTime::Seconds() + 0.05;
			const FPersistedStateWalkResult InterfaceState = FPersistedStateWalker(
				EffectiveDeadline, false).WalkScriptStruct(
					FMetasoundFrontendClassInterface::StaticStruct(),
					&RegisteredClass.GetDefaultInterface());
			if (!InterfaceState.bComplete || !IsSha256(InterfaceState.Fingerprint)) return FString();
			AppendToken(Canonical, InterfaceState.Fingerprint);
			return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
		}

		bool InspectMetaSound(
		const UObject* Object,
		FHyperAIAudioRecord& Record,
		FHyperAIAudioValueSnapshot* Snapshot,
		FString& OutCanonical,
		bool* bOutTraversalComplete = nullptr,
		const double AbsoluteDeadline = 0.0,
		bool* bOutFrontendValid = nullptr,
		const FMetasoundFrontendDocument* DocumentOverride = nullptr)
	{
		if (bOutTraversalComplete) *bOutTraversalComplete = true;
		if (bOutFrontendValid) *bOutFrontendValid = false;
		const IMetaSoundDocumentInterface* Interface = Object
			? Cast<IMetaSoundDocumentInterface>(Object) : nullptr;
		if (!Interface && !DocumentOverride) return false;
		const FMetasoundFrontendDocument& Document = DocumentOverride
			? *DocumentOverride : Interface->GetConstDocument();
		struct FObservedMetaSoundPage
		{
			const FMetasoundFrontendGraph* Graph = nullptr;
			FGuid OwningGraphClassId;
			bool bRoot = false;
		};
		const TArray<FMetasoundFrontendGraph>& RootPages = Document.RootGraph.GetConstGraphPages();
		TArray<FObservedMetaSoundPage> Pages;
		for (int32 Index = 0; Index < FMath::Min(RootPages.Num(), MaxMetaSoundPages + 1); ++Index)
			Pages.Add({&RootPages[Index], Document.RootGraph.ID, true});
		for (int32 SubgraphIndex = 0;
			SubgraphIndex < FMath::Min(Document.Subgraphs.Num(), MaxMetaSoundClasses)
				&& Pages.Num() <= MaxMetaSoundPages; ++SubgraphIndex)
		{
			const TArray<FMetasoundFrontendGraph>& SubgraphPages =
				Document.Subgraphs[SubgraphIndex].GetConstGraphPages();
			for (int32 PageIndex = 0;
				PageIndex < SubgraphPages.Num() && Pages.Num() <= MaxMetaSoundPages; ++PageIndex)
				Pages.Add({&SubgraphPages[PageIndex], Document.Subgraphs[SubgraphIndex].ID, false});
		}
		int64 DeclaredNodes = 0;
		int64 DeclaredEdges = 0;
		int64 DeclaredVariables = 0;
		int32 TraversedNodes = 0;
		int32 TraversedEdges = 0;
		int32 TraversedVariables = 0;
		int32 TraversedVertices = 0;
		int32 TraversedVariableReferences = 0;
		int32 TraversedLiterals = 0;
		bool bTraversalComplete = Pages.Num() <= MaxMetaSoundPages
			&& Document.Interfaces.Num() <= MaxMetaSoundInterfaces
			&& Document.Subgraphs.Num() <= MaxMetaSoundClasses
			&& Document.Dependencies.Num() <= MaxMetaSoundClasses;
		bool bValid = bTraversalComplete;
		const double EffectiveDeadline = AbsoluteDeadline > 0.0
			? AbsoluteDeadline : FPlatformTime::Seconds() + 0.2;
		auto WithinDeadline = [&]()
		{
			if (FPlatformTime::Seconds() <= EffectiveDeadline) return true;
			bTraversalComplete = false;
			bValid = false;
			return false;
		};
		auto IsRegisteredMetaSoundType = [&](const FName& TypeName)
		{
			const bool bRegistered = !TypeName.IsNone()
				&& Metasound::Frontend::IDataTypeRegistry::Get().IsRegistered(TypeName);
			if (!bRegistered) bTraversalComplete = false;
			return bRegistered;
		};
		AppendToken(OutCanonical, TEXT("metasound.persisted-frontend.v2"));
		// The reflected document fingerprint covers every persisted field, including
		// literals/defaults, dependencies, subgraphs, declared interfaces, and the
		// polymorphic document template. Semantic checks below independently reject
		// malformed identifiers, members, references, and edges.
		const FPersistedStateWalkResult PersistedDocument =
			FPersistedStateWalker(EffectiveDeadline, false).WalkScriptStruct(
				FMetasoundFrontendDocument::StaticStruct(), &Document);
		if (!PersistedDocument.bComplete || !IsSha256(PersistedDocument.Fingerprint))
		{
			bTraversalComplete = false;
			bValid = false;
		}
		AppendToken(OutCanonical, PersistedDocument.Fingerprint);
		AppendInt(OutCanonical, Pages.Num());
		AppendInt(OutCanonical, Document.Interfaces.Num());
		AppendInt(OutCanonical, Document.Subgraphs.Num());
		AppendInt(OutCanonical, Document.Dependencies.Num());
		if (!Document.Metadata.Version.IsValid()) bValid = false;
		int32 InterfaceCount = 0;
		TSet<FMetasoundFrontendVersion> DeclaredInterfaceVersions;
		for (const FMetasoundFrontendVersion& Version : Document.Interfaces)
		{
			if (++InterfaceCount > MaxMetaSoundInterfaces) break;
			if (!Version.IsValid() || DeclaredInterfaceVersions.Contains(Version)) bValid = false;
			DeclaredInterfaceVersions.Add(Version);
		}
		if (Document.Template.IsValid()
			&& !Document.Template.GetScriptStruct()->IsChildOf(
				FMetaSoundFrontendDocumentTemplate::StaticStruct())) bValid = false;
		FString SemanticState = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
			TEXT("hyperai.metasound-semantic-chain.v1"));
		auto FoldToken = [&](const FString& Value)
		{
			FString Step;
			AppendToken(Step, SemanticState);
			AppendToken(Step, Value);
			SemanticState = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Step);
			if (!IsSha256(SemanticState)) bTraversalComplete = bValid = false;
		};
		auto FoldInt = [&](const int64 Value) { FoldToken(FString::Printf(TEXT("%lld"), Value)); };
		auto FoldGuid = [&](const FGuid& Value)
		{
			FoldToken(Value.ToString(EGuidFormats::DigitsWithHyphensLower));
		};
		const FMetasoundFrontendClassInterface& RootInterface =
			Document.RootGraph.GetDefaultInterface();
		TSet<FGuid> ResolvableClassIds;
		TMap<FGuid, const FMetasoundFrontendClass*> ResolvableClasses;
		TMap<FGuid, TSet<FGuid>> ReferencedDefaultPageIdsByGraphClass;
			auto ValidateClassInterface = [&](const FMetasoundFrontendClass& Class,
				const bool bOwnsGraphPages)
			{
				const FMetasoundFrontendClassInterface& ClassInterface = Class.GetDefaultInterface();
				const EMetasoundFrontendClassType ClassType = Class.Metadata.GetType();
				// UE's generated data-type Input/Output registry classes bind vertex names per instance.
				const bool bAllowsUnboundVertexNames = ClassType == EMetasoundFrontendClassType::Input
					|| ClassType == EMetasoundFrontendClassType::Output;
			TSet<FGuid> InputVertexIds;
			TSet<FGuid> OutputVertexIds;
			TSet<FName> InputVertexNames;
			TSet<FName> OutputVertexNames;
			TSet<FName> EnvironmentNames;
			FoldGuid(Class.ID);
			FoldInt(ClassInterface.Inputs.Num());
			FoldInt(ClassInterface.Outputs.Num());
			FoldInt(ClassInterface.Environment.Num());
			for (const FMetasoundFrontendClassInput& Input : ClassInterface.Inputs)
			{
				if (!WithinDeadline()) break;
				if (TraversedVertices >= MaxMetaSoundVertices)
				{
					bTraversalComplete = bValid = false;
					break;
				}
				++TraversedVertices;
				if (!Input.VertexID.IsValid() || (!bAllowsUnboundVertexNames && Input.Name.IsNone())
					|| !IsRegisteredMetaSoundType(Input.TypeName)
					|| InputVertexIds.Contains(Input.VertexID) || InputVertexNames.Contains(Input.Name))
					bValid = false;
				InputVertexIds.Add(Input.VertexID);
				InputVertexNames.Add(Input.Name);
				FoldGuid(Input.VertexID);
				FoldToken(Input.Name.ToString());
				FoldToken(Input.TypeName.ToString());
				FoldInt(static_cast<int32>(Input.AccessType));
				TSet<FGuid> DefaultPages;
				for (const FMetasoundFrontendClassInputDefault& Default : Input.GetDefaults())
				{
					if (!WithinDeadline()) break;
					if (++TraversedLiterals > MaxMetaSoundLiterals)
					{
						bTraversalComplete = bValid = false;
						break;
					}
					const bool bAdmittedPage = Default.PageID == Metasound::Frontend::DefaultPageID
						|| Default.PageID.IsValid();
					bool bDataTypeRegistered = false;
					const bool bLiteralTyped = IsTypedMetaSoundLiteral(
						Default.Literal, Input.TypeName, bDataTypeRegistered);
					if (!bAdmittedPage || DefaultPages.Contains(Default.PageID)
						|| !bLiteralTyped) bValid = false;
					if (!bDataTypeRegistered) bTraversalComplete = false;
					DefaultPages.Add(Default.PageID);
					if (bOwnsGraphPages)
						ReferencedDefaultPageIdsByGraphClass.FindOrAdd(Class.ID).Add(Default.PageID);
					FoldGuid(Default.PageID);
					FoldInt(static_cast<int32>(Default.Literal.GetType()));
					FoldInt(Default.Literal.GetArrayNum());
				}
			}
			for (const FMetasoundFrontendClassOutput& Output : ClassInterface.Outputs)
			{
				if (!WithinDeadline()) break;
				if (TraversedVertices >= MaxMetaSoundVertices)
				{
					bTraversalComplete = bValid = false;
					break;
				}
				++TraversedVertices;
				if (!Output.VertexID.IsValid() || (!bAllowsUnboundVertexNames && Output.Name.IsNone())
					|| !IsRegisteredMetaSoundType(Output.TypeName)
					|| OutputVertexIds.Contains(Output.VertexID) || OutputVertexNames.Contains(Output.Name))
					bValid = false;
				OutputVertexIds.Add(Output.VertexID);
				OutputVertexNames.Add(Output.Name);
				FoldGuid(Output.VertexID);
				FoldToken(Output.Name.ToString());
				FoldToken(Output.TypeName.ToString());
				FoldInt(static_cast<int32>(Output.AccessType));
			}
			for (const FMetasoundFrontendClassEnvironmentVariable& Environment : ClassInterface.Environment)
			{
				if (!WithinDeadline()) break;
				if (TraversedVertices >= MaxMetaSoundVertices)
				{
					bTraversalComplete = bValid = false;
					break;
				}
				++TraversedVertices;
				if (Environment.Name.IsNone() || !IsRegisteredMetaSoundType(Environment.TypeName)
					|| EnvironmentNames.Contains(Environment.Name)) bValid = false;
				EnvironmentNames.Add(Environment.Name);
				FoldToken(Environment.Name.ToString());
				FoldToken(Environment.TypeName.ToString());
				FoldInt(Environment.bIsRequired ? 1 : 0);
			}
		};
		auto AddResolvableClass = [&](const FMetasoundFrontendClass& Class)
		{
			if (!Class.ID.IsValid() || ResolvableClassIds.Contains(Class.ID)) bValid = false;
			ResolvableClassIds.Add(Class.ID);
			ResolvableClasses.Add(Class.ID, &Class);
			FoldToken(Class.Metadata.GetClassName().ToString());
		};
		AddResolvableClass(Document.RootGraph);
		ValidateClassInterface(Document.RootGraph, true);
		for (int32 Index = 0; Index < FMath::Min(Document.Subgraphs.Num(), MaxMetaSoundClasses); ++Index)
		{
			AddResolvableClass(Document.Subgraphs[Index]);
			ValidateClassInterface(Document.Subgraphs[Index], true);
		}
			TSet<Metasound::Frontend::FNodeRegistryKey> DependencyRegistryKeys;
			Metasound::Frontend::INodeClassRegistry* NodeClassRegistry =
				Metasound::Frontend::INodeClassRegistry::Get();
			if (!NodeClassRegistry && !Document.Dependencies.IsEmpty())
				bTraversalComplete = bValid = false;
			for (int32 Index = 0; Index < FMath::Min(Document.Dependencies.Num(), MaxMetaSoundClasses); ++Index)
			{
			const FMetasoundFrontendClass& Dependency = Document.Dependencies[Index];
			AddResolvableClass(Document.Dependencies[Index]);
			ValidateClassInterface(Dependency, false);
			const EMetasoundFrontendClassType DependencyType = Dependency.Metadata.GetType();
			Metasound::Frontend::FNodeRegistryKey RegistryKey;
			if (DependencyType == EMetasoundFrontendClassType::Graph
				|| DependencyType == EMetasoundFrontendClassType::Invalid) bValid = false;
			else RegistryKey = Metasound::Frontend::FNodeRegistryKey(
				DependencyType, Dependency.Metadata.GetClassName(), Dependency.Metadata.GetVersion());
				if (!RegistryKey.IsValid() || DependencyRegistryKeys.Contains(RegistryKey)) bValid = false;
				DependencyRegistryKeys.Add(RegistryKey);
				FoldToken(RegistryKey.ToString());
				FMetasoundFrontendClass RegisteredClass;
				if (!NodeClassRegistry || !RegistryKey.IsValid()
					|| !NodeClassRegistry->IsNodeRegistered(RegistryKey)
					|| !NodeClassRegistry->FindFrontendClassFromRegistered(RegistryKey, RegisteredClass)
					|| !IsMetaSoundClassRegistryExact(
						Dependency, RegisteredClass, RegistryKey, EffectiveDeadline))
				{
					bValid = false;
					WithinDeadline();
				}
				else
				{
					const FString InterfaceFingerprint = ComputeMetaSoundRegistryInterfaceFingerprint(
						RegistryKey, RegisteredClass, EffectiveDeadline);
					if (!IsSha256(InterfaceFingerprint)) bTraversalComplete = bValid = false;
					FoldToken(InterfaceFingerprint);
				}
			}

		TScriptInterface<IMetaSoundDocumentInterface> DocumentInterface;
		if (Interface)
		{
			DocumentInterface.SetObject(const_cast<UObject*>(Object));
			DocumentInterface.SetInterface(const_cast<IMetaSoundDocumentInterface*>(Interface));
		}
		TUniquePtr<FMetaSoundFrontendDocumentBuilder> DocumentBuilder;
		TSet<FName> RootMemberNames;
		for (const FMetasoundFrontendClassInput& Input : RootInterface.Inputs)
		{
			RootMemberNames.Add(Input.Name);
		}
		for (const FMetasoundFrontendClassOutput& Output : RootInterface.Outputs)
		{
			RootMemberNames.Add(Output.Name);
		}
		const int32 PageLimit = FMath::Min(Pages.Num(), MaxMetaSoundPages);
		TMap<FGuid, TSet<FGuid>> PageIdsByOwningGraphClass;
		for (int32 PageIndex = 0; PageIndex < PageLimit; ++PageIndex)
		{
			if (!WithinDeadline()) break;
			const FObservedMetaSoundPage& ObservedPage = Pages[PageIndex];
			if (!ObservedPage.Graph)
			{
				bValid = false;
				continue;
			}
			const FMetasoundFrontendGraph& Page = *ObservedPage.Graph;
			DeclaredNodes += Page.Nodes.Num();
			DeclaredEdges += Page.Edges.Num();
			DeclaredVariables += Page.Variables.Num();
			TSet<FGuid>& OwnerPageIds = PageIdsByOwningGraphClass.FindOrAdd(
				ObservedPage.OwningGraphClassId);
			if ((Page.PageID != Metasound::Frontend::DefaultPageID && !Page.PageID.IsValid())
				|| OwnerPageIds.Contains(Page.PageID)) bValid = false;
			OwnerPageIds.Add(Page.PageID);
			FoldGuid(Page.PageID);
			FoldInt(Page.Nodes.Num());
			FoldInt(Page.Edges.Num());
			FoldInt(Page.Variables.Num());
			TSet<FGuid> NodeIds;
			TMap<FGuid, TSet<FGuid>> Inputs;
			TMap<FGuid, TSet<FGuid>> Outputs;
			TMap<FString, const FMetasoundFrontendVertex*> InputVertices;
			TMap<FString, const FMetasoundFrontendVertex*> OutputVertices;
			TMap<FString, EMetasoundFrontendVertexAccessType> InputAccess;
			TMap<FString, EMetasoundFrontendVertexAccessType> OutputAccess;
			auto VertexKey = [](const FGuid& NodeId, const FGuid& VertexId)
			{
				return NodeId.ToString(EGuidFormats::Digits) + TEXT(":")
					+ VertexId.ToString(EGuidFormats::Digits);
			};
			const int32 NodesRemaining = MaxMetaSoundNodes - TraversedNodes;
			const int32 NodeLimit = FMath::Min(Page.Nodes.Num(), FMath::Max(0, NodesRemaining));
			if (NodeLimit != Page.Nodes.Num()) bTraversalComplete = bValid = false;
			for (int32 NodeIndex = 0; NodeIndex < NodeLimit; ++NodeIndex)
			{
				if (!WithinDeadline()) break;
				const FMetasoundFrontendNode& Node = Page.Nodes[NodeIndex];
				++TraversedNodes;
					const FGuid NodeId = Node.GetID();
					if (!NodeId.IsValid() || !Node.ClassID.IsValid()
						|| !ResolvableClassIds.Contains(Node.ClassID) || NodeIds.Contains(NodeId)) bValid = false;
					NodeIds.Add(NodeId);
					const FMetasoundFrontendClass* const* NodeClass = ResolvableClasses.Find(Node.ClassID);
					const FMetasoundFrontendClassInterface* ClassInterface = NodeClass && *NodeClass
						? &(*NodeClass)->GetInterfaceForNode(Node) : nullptr;
					const EMetasoundFrontendClassType NodeClassType = NodeClass && *NodeClass
						? (*NodeClass)->Metadata.GetType() : EMetasoundFrontendClassType::Invalid;
					const bool bAllowsInstanceVertexNames = NodeClassType == EMetasoundFrontendClassType::Input
						|| NodeClassType == EMetasoundFrontendClassType::Output;
				if (!ClassInterface
					|| Node.Interface.Inputs.Num() != ClassInterface->Inputs.Num()
					|| Node.Interface.Outputs.Num() != ClassInterface->Outputs.Num()
					|| Node.Interface.Environment.Num() != ClassInterface->Environment.Num()) bValid = false;
				TSet<FName> NodeInputNames;
				TSet<FName> NodeOutputNames;
				TSet<FName> NodeEnvironmentNames;
				TSet<FGuid> NodeInputVertexIds;
				TSet<FGuid> NodeOutputVertexIds;
				FoldGuid(NodeId);
				FoldGuid(Node.ClassID);
				FoldToken(Node.Name.ToString());
				for (const FMetasoundFrontendVertex& Vertex : Node.Interface.Inputs)
				{
					if (!WithinDeadline()) break;
					if (TraversedVertices >= MaxMetaSoundVertices)
					{
						bTraversalComplete = bValid = false;
						break;
					}
					++TraversedVertices;
					TSet<FGuid>& NodeInputs = Inputs.FindOrAdd(NodeId);
					const FString ExactVertexKey = VertexKey(NodeId, Vertex.VertexID);
					if (!Vertex.VertexID.IsValid() || Vertex.Name.IsNone()
						|| !IsRegisteredMetaSoundType(Vertex.TypeName)
						|| NodeInputs.Contains(Vertex.VertexID) || NodeInputNames.Contains(Vertex.Name)
						|| NodeInputVertexIds.Contains(Vertex.VertexID)
						|| InputVertices.Contains(ExactVertexKey)) bValid = false;
					NodeInputs.Add(Vertex.VertexID);
					NodeInputNames.Add(Vertex.Name);
					NodeInputVertexIds.Add(Vertex.VertexID);
					InputVertices.Add(ExactVertexKey, &Vertex);
					if (ClassInterface)
					{
							if (const FMetasoundFrontendClassInput* ClassInput = ClassInterface->Inputs.FindByPredicate(
								[&](const FMetasoundFrontendClassInput& Candidate)
								{
									return Candidate.VertexID == Vertex.VertexID;
								}))
							{
								InputAccess.Add(ExactVertexKey, ClassInput->AccessType);
								if (ClassInput->VertexID != Vertex.VertexID
									|| ClassInput->TypeName != Vertex.TypeName
									|| (!bAllowsInstanceVertexNames && ClassInput->Name != Vertex.Name))
									bValid = false;
						}
						else bValid = false;
					}
					FoldGuid(Vertex.VertexID);
					FoldToken(Vertex.Name.ToString());
					FoldToken(Vertex.TypeName.ToString());
				}
					for (const FMetasoundFrontendVertex& Vertex : Node.Interface.Outputs)
				{
					if (!WithinDeadline()) break;
					if (TraversedVertices >= MaxMetaSoundVertices)
					{
						bTraversalComplete = bValid = false;
						break;
					}
					++TraversedVertices;
					TSet<FGuid>& NodeOutputs = Outputs.FindOrAdd(NodeId);
					const FString ExactVertexKey = VertexKey(NodeId, Vertex.VertexID);
					if (!Vertex.VertexID.IsValid() || Vertex.Name.IsNone()
						|| !IsRegisteredMetaSoundType(Vertex.TypeName)
						|| NodeOutputs.Contains(Vertex.VertexID) || NodeOutputNames.Contains(Vertex.Name)
						|| NodeOutputVertexIds.Contains(Vertex.VertexID)
						|| OutputVertices.Contains(ExactVertexKey)) bValid = false;
					NodeOutputs.Add(Vertex.VertexID);
					NodeOutputNames.Add(Vertex.Name);
					NodeOutputVertexIds.Add(Vertex.VertexID);
					OutputVertices.Add(ExactVertexKey, &Vertex);
					if (ClassInterface)
					{
							if (const FMetasoundFrontendClassOutput* ClassOutput = ClassInterface->Outputs.FindByPredicate(
								[&](const FMetasoundFrontendClassOutput& Candidate)
								{
									return Candidate.VertexID == Vertex.VertexID;
								}))
							{
								OutputAccess.Add(ExactVertexKey, ClassOutput->AccessType);
								if (ClassOutput->VertexID != Vertex.VertexID
									|| ClassOutput->TypeName != Vertex.TypeName
									|| (!bAllowsInstanceVertexNames && ClassOutput->Name != Vertex.Name))
									bValid = false;
						}
						else bValid = false;
					}
					FoldGuid(Vertex.VertexID);
					FoldToken(Vertex.Name.ToString());
					FoldToken(Vertex.TypeName.ToString());
					}
					for (const FMetasoundFrontendVertex& Vertex : Node.Interface.Environment)
					{
						if (!WithinDeadline()) break;
						if (TraversedVertices >= MaxMetaSoundVertices)
						{
							bTraversalComplete = bValid = false;
							break;
						}
						++TraversedVertices;
						if (Vertex.Name.IsNone() || !IsRegisteredMetaSoundType(Vertex.TypeName)
							|| NodeEnvironmentNames.Contains(Vertex.Name)) bValid = false;
						NodeEnvironmentNames.Add(Vertex.Name);
						if (ClassInterface)
						{
							const FMetasoundFrontendClassEnvironmentVariable* ClassEnvironment =
								ClassInterface->Environment.FindByPredicate(
									[&](const FMetasoundFrontendClassEnvironmentVariable& Candidate)
									{
										return Candidate.Name == Vertex.Name;
									});
							if (!ClassEnvironment || ClassEnvironment->TypeName != Vertex.TypeName) bValid = false;
						}
						FoldGuid(Vertex.VertexID);
						FoldToken(Vertex.Name.ToString());
						FoldToken(Vertex.TypeName.ToString());
					}
					TSet<FGuid> LiteralVertexIds;
					for (const FMetasoundFrontendVertexLiteral& Literal : Node.InputLiterals)
					{
						if (!WithinDeadline()) break;
						if (++TraversedLiterals > MaxMetaSoundLiterals)
						{
							bTraversalComplete = bValid = false;
							break;
						}
						const FMetasoundFrontendVertex* const* LiteralVertex =
							InputVertices.Find(VertexKey(NodeId, Literal.VertexID));
						bool bDataTypeRegistered = false;
						const bool bLiteralTyped = LiteralVertex && *LiteralVertex
							&& IsTypedMetaSoundLiteral(Literal.Value, (*LiteralVertex)->TypeName,
								bDataTypeRegistered);
						if (!Literal.VertexID.IsValid() || LiteralVertexIds.Contains(Literal.VertexID)
							|| !Inputs.FindOrAdd(NodeId).Contains(Literal.VertexID)
							|| !LiteralVertex || !*LiteralVertex
							|| !bLiteralTyped) bValid = false;
						if (LiteralVertex && *LiteralVertex && !bDataTypeRegistered)
							bTraversalComplete = false;
						LiteralVertexIds.Add(Literal.VertexID);
						FoldGuid(Literal.VertexID);
						FoldInt(static_cast<int32>(Literal.Value.GetType()));
						FoldInt(Literal.Value.GetArrayNum());
					}
				}
			const int32 EdgesRemaining = MaxMetaSoundEdges - TraversedEdges;
			const int32 EdgeLimit = FMath::Min(Page.Edges.Num(), FMath::Max(0, EdgesRemaining));
			if (EdgeLimit != Page.Edges.Num()) bTraversalComplete = bValid = false;
			TSet<FString> EdgeKeys;
			TSet<FString> ConnectedInputKeys;
			TMap<FGuid, TArray<FGuid>> Adjacency;
			TMap<FGuid, int32> InDegree;
			for (const FGuid& NodeId : NodeIds) InDegree.Add(NodeId, 0);
			for (int32 EdgeIndex = 0; EdgeIndex < EdgeLimit; ++EdgeIndex)
			{
				if (!WithinDeadline()) break;
				const FMetasoundFrontendEdge& Edge = Page.Edges[EdgeIndex];
				++TraversedEdges;
				const FString EdgeKey = Edge.FromNodeID.ToString(EGuidFormats::Digits)
					+ Edge.FromVertexID.ToString(EGuidFormats::Digits)
					+ Edge.ToNodeID.ToString(EGuidFormats::Digits)
					+ Edge.ToVertexID.ToString(EGuidFormats::Digits);
				const FString InputKey = Edge.ToNodeID.ToString(EGuidFormats::Digits)
					+ Edge.ToVertexID.ToString(EGuidFormats::Digits);
					if (EdgeKeys.Contains(EdgeKey) || ConnectedInputKeys.Contains(InputKey)) bValid = false;
				EdgeKeys.Add(EdgeKey);
				ConnectedInputKeys.Add(InputKey);
				FoldGuid(Edge.FromNodeID);
				FoldGuid(Edge.FromVertexID);
				FoldGuid(Edge.ToNodeID);
				FoldGuid(Edge.ToVertexID);
				const TSet<FGuid>* From = Outputs.Find(Edge.FromNodeID);
				const TSet<FGuid>* To = Inputs.Find(Edge.ToNodeID);
					if (!From || !From->Contains(Edge.FromVertexID)
						|| !To || !To->Contains(Edge.ToVertexID)) bValid = false;
				const FMetasoundFrontendVertex* const* FromVertex =
					OutputVertices.Find(VertexKey(Edge.FromNodeID, Edge.FromVertexID));
				const FMetasoundFrontendVertex* const* ToVertex =
					InputVertices.Find(VertexKey(Edge.ToNodeID, Edge.ToVertexID));
				const EMetasoundFrontendVertexAccessType* FromAccess =
					OutputAccess.Find(VertexKey(Edge.FromNodeID, Edge.FromVertexID));
				const EMetasoundFrontendVertexAccessType* ToAccess =
					InputAccess.Find(VertexKey(Edge.ToNodeID, Edge.ToVertexID));
					if (!FromVertex || !ToVertex || !FromAccess || !ToAccess
					|| !FMetasoundFrontendClassVertex::CanConnectVertexAccessTypes(*FromAccess, *ToAccess)
					|| !IsMetaSoundTypeCastable((*FromVertex)->TypeName, (*ToVertex)->TypeName)) bValid = false;
					if (NodeIds.Contains(Edge.FromNodeID) && NodeIds.Contains(Edge.ToNodeID))
					{
						Adjacency.FindOrAdd(Edge.FromNodeID).Add(Edge.ToNodeID);
						++InDegree.FindOrAdd(Edge.ToNodeID);
					}
					if (bValid && ObservedPage.bRoot && Interface && !DocumentBuilder)
						DocumentBuilder = MakeUnique<FMetaSoundFrontendDocumentBuilder>(
							DocumentInterface,
							TSharedPtr<Metasound::Frontend::FDocumentModifyDelegates>(), true);
					if (ObservedPage.bRoot && DocumentBuilder
						&& DocumentBuilder->IsValidEdge(Edge, &Page.PageID)
						!= Metasound::Frontend::EInvalidEdgeReason::None) bValid = false;
			}
			TArray<FGuid> Ready;
			for (const TPair<FGuid, int32>& Pair : InDegree)
				if (Pair.Value == 0) Ready.Add(Pair.Key);
			int32 AcyclicNodeCount = 0;
			while (!Ready.IsEmpty() && WithinDeadline())
			{
				const FGuid Current = Ready.Pop(EAllowShrinking::No);
				++AcyclicNodeCount;
				if (const TArray<FGuid>* Next = Adjacency.Find(Current))
				{
					for (const FGuid& Destination : *Next)
					{
						int32* Degree = InDegree.Find(Destination);
						if (Degree && --(*Degree) == 0) Ready.Add(Destination);
					}
				}
			}
			if (AcyclicNodeCount != NodeIds.Num()) bValid = false;
			TSet<FGuid> VariableIds;
			TSet<FName> VariableNames;
			const int32 VariablesRemaining = MaxMetaSoundVariables - TraversedVariables;
			const int32 VariableLimit = FMath::Min(
				Page.Variables.Num(), FMath::Max(0, VariablesRemaining));
			if (VariableLimit != Page.Variables.Num()) bTraversalComplete = bValid = false;
			for (int32 VariableIndex = 0; VariableIndex < VariableLimit; ++VariableIndex)
			{
				if (!WithinDeadline()) break;
				const FMetasoundFrontendVariable& Variable = Page.Variables[VariableIndex];
				++TraversedVariables;
					if (!Variable.ID.IsValid() || Variable.Name.IsNone()
						|| !IsRegisteredMetaSoundType(Variable.TypeName)
						|| VariableIds.Contains(Variable.ID) || VariableNames.Contains(Variable.Name)
					|| (ObservedPage.bRoot && RootMemberNames.Contains(Variable.Name))) bValid = false;
				VariableIds.Add(Variable.ID);
				VariableNames.Add(Variable.Name);
					FoldGuid(Variable.ID);
					FoldToken(Variable.Name.ToString());
					FoldToken(Variable.TypeName.ToString());
					FoldInt(static_cast<int32>(Variable.Literal.GetType()));
					FoldInt(Variable.Literal.GetArrayNum());
					bool bDataTypeRegistered = false;
					if (!IsTypedMetaSoundLiteral(Variable.Literal, Variable.TypeName,
						bDataTypeRegistered)) bValid = false;
					if (!bDataTypeRegistered) bTraversalComplete = false;
				if (Variable.VariableNodeID.IsValid() && !NodeIds.Contains(Variable.VariableNodeID)) bValid = false;
				if (Variable.MutatorNodeID.IsValid() && !NodeIds.Contains(Variable.MutatorNodeID)) bValid = false;
				for (const FGuid& Id : Variable.AccessorNodeIDs)
				{
					if (!WithinDeadline()) break;
					if (TraversedVariableReferences >= MaxMetaSoundVariableReferences)
					{
						bTraversalComplete = bValid = false;
						break;
					}
					++TraversedVariableReferences;
					FoldGuid(Id);
					if (!NodeIds.Contains(Id)) bValid = false;
				}
				for (const FGuid& Id : Variable.DeferredAccessorNodeIDs)
				{
					if (!WithinDeadline()) break;
					if (TraversedVariableReferences >= MaxMetaSoundVariableReferences)
					{
						bTraversalComplete = bValid = false;
						break;
					}
					++TraversedVariableReferences;
					FoldGuid(Id);
					if (!NodeIds.Contains(Id)) bValid = false;
				}
			}
		}
		for (const TPair<FGuid, TSet<FGuid>>& Pair : ReferencedDefaultPageIdsByGraphClass)
		{
			const TSet<FGuid>* OwnerPages = PageIdsByOwningGraphClass.Find(Pair.Key);
			for (const FGuid& DefaultPageId : Pair.Value)
				if (!OwnerPages || !OwnerPages->Contains(DefaultPageId)) bValid = false;
		}
		AppendToken(OutCanonical, SemanticState);
		bValid &= bTraversalComplete;
		if (bOutTraversalComplete) *bOutTraversalComplete = bTraversalComplete;
		if (bOutFrontendValid) *bOutFrontendValid = bTraversalComplete && bValid;
		if (!bTraversalComplete && Snapshot) Snapshot->bComplete = false;
		Record.PrimaryCount = static_cast<int32>(FMath::Min<int64>(DeclaredNodes, MAX_int32));
		Record.SecondaryCount = static_cast<int32>(FMath::Min<int64>(DeclaredEdges, MAX_int32));
		const int64 DeclaredMembers = static_cast<int64>(
			Document.RootGraph.GetDefaultInterface().Inputs.Num())
			+ Document.RootGraph.GetDefaultInterface().Outputs.Num() + DeclaredVariables;
		Record.TertiaryCount = static_cast<int32>(FMath::Min<int64>(DeclaredMembers, MAX_int32));
		Record.Details.Add(FString::Printf(TEXT("graph_pages=%d"), Pages.Num()));
		Record.Details.Add(FString::Printf(TEXT("frontend_valid=%s"), bValid ? TEXT("true") : TEXT("false")));
		Record.Details.Add(FString::Printf(TEXT("frontend_bounded=%s"),
			bTraversalComplete ? TEXT("true") : TEXT("false")));
		Record.Details.Add(FString::Printf(TEXT("interfaces=%d"), Document.Interfaces.Num()));
		Record.Details.Add(FString::Printf(TEXT("dependencies=%d"), Document.Dependencies.Num()));
		if (!bValid && Snapshot)
		{
			AddCaptureIssue(*Snapshot, TEXT("metasound_frontend_invalid"), TEXT("error"),
				Record.Variant, Record.ObjectPath,
				TEXT("Persisted MetaSound pages contain invalid IDs/references or exceed hard graph traversal bounds."));
		}
		return true;
	}

	FString ComputeLoadedRevision(
		const UObject* Object,
		const FString* ObservedFrontendCanonical = nullptr,
		const bool bObservedFrontendComplete = true,
		const double AbsoluteDeadline = 0.0,
		const FPersistedStateWalkResult* ObservedPersisted = nullptr)
	{
		if (!IsValid(Object) || !IsSafeObjectPath(Object->GetPathName())) return FString();
		FString Canonical;
		AppendToken(Canonical, TEXT("hyperai.audio-loaded-object.v1"));
		AppendToken(Canonical, Object->GetPathName());
		AppendToken(Canonical, Object->GetClass()->GetPathName());
		AppendBool(Canonical, Object->GetPackage() && Object->GetPackage()->IsDirty());
		const FPersistedStateWalkResult Persisted = ObservedPersisted
			? *ObservedPersisted : FPersistedStateWalker(AbsoluteDeadline, true).WalkObject(Object);
		if (!Persisted.bComplete || !IsSha256(Persisted.Fingerprint)) return FString();
		AppendToken(Canonical, Persisted.Fingerprint);
		if (AbsoluteDeadline > 0.0 && FPlatformTime::Seconds() > AbsoluteDeadline)
			return FString();
		if (const USoundWave* Wave = Cast<USoundWave>(Object))
		{
			AppendDouble(Canonical, Wave->GetDuration());
			AppendDouble(Canonical, Wave->GetSampleRateForCurrentPlatform());
			AppendInt(Canonical, Wave->NumChannels);
		}
		if (ObservedFrontendCanonical)
		{
			if (!bObservedFrontendComplete) return FString();
			AppendToken(Canonical, *ObservedFrontendCanonical);
		}
		else
		{
			FHyperAIAudioRecord Unused;
			FString Frontend;
			bool bFrontendTraversalComplete = true;
			if (InspectMetaSound(Object, Unused, nullptr, Frontend,
				&bFrontendTraversalComplete, AbsoluteDeadline))
			{
				if (!bFrontendTraversalComplete) return FString();
				AppendToken(Canonical, Frontend);
			}
		}
		return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
	}

	bool AddRecord(FHyperAIAudioValueSnapshot& Snapshot, FHyperAIAudioRecord&& Record)
	{
		if (Snapshot.Records.Num() >= FHyperAIStudioAudioContracts::MaxRecords)
		{
			Snapshot.bComplete = false;
			return false;
		}
		Record.ObjectPath = Clip(Record.ObjectPath, FHyperAIStudioAudioContracts::MaxPathCharacters);
		Record.StableId = Record.Variant + TEXT(":") + Record.ObjectPath;
		Record.References.Sort();
		Record.Details.Sort();
		if (!IsSha256(Record.Revision))
		{
			Snapshot.bComplete = false;
			if (Record.bLoaded)
			{
				AddCaptureIssue(Snapshot, TEXT("loaded_revision_unavailable"), TEXT("error"),
					Record.Variant, Record.ObjectPath,
					TEXT("Loaded audio state exceeded exact revision bounds and cannot participate in CAS."));
			}
			else
			{
				AddCaptureIssue(Snapshot, TEXT("on_disk_revision_unavailable"), TEXT("error"),
					Record.Variant, Record.ObjectPath,
					TEXT("On-disk revision is empty because exact package data or saved-content hash evidence was unavailable."));
			}
		}
		Snapshot.Records.Add(MoveTemp(Record));
		return true;
	}

	bool MatchesFilter(const TArray<FString>& Values, const FString& Value)
	{
		return Values.IsEmpty() || Values.Contains(Value);
	}

	bool CaptureLoaded(
		const FHyperAIAudioInspectRequest& Request,
		FHyperAIAudioValueSnapshot& Snapshot,
		const double DeadlineSeconds)
	{
		const double Started = FPlatformTime::Seconds();
		if (Request.ObjectPaths.IsEmpty())
		{
			Snapshot.bComplete = false;
			AddCaptureIssue(Snapshot, TEXT("broad_audio_index_not_ready"), TEXT("warning"),
				TEXT("loaded_only"), FString(),
				TEXT("Broad loaded-object inspection requires a generation-cached typed index; synchronous global UObject scans are prohibited."));
			return true;
		}

			auto CaptureObject = [&](UObject* Object)
		{
			if (!IsValid(Object) || Object->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject)) return;
			const FString ObjectPath = Object->GetPathName();
			if (!IsSafeObjectPath(ObjectPath)) return;
			const FString Variant = VariantFromObject(Object);
			if (Variant.IsEmpty() || !MatchesFilter(Request.Variants, Variant)) return;
			FHyperAIAudioRecord Record;
			Record.Variant = Variant;
			Record.ObjectPath = ObjectPath;
			Record.PackageName = Object->GetPackage() ? Object->GetPackage()->GetName() : FString();
			Record.ClassPath = Object->GetClass()->GetPathName();
			Record.bLoaded = true;
			Record.bDirty = Object->GetPackage() && Object->GetPackage()->IsDirty();
			Record.bEnabled = true;
			const FPersistedStateWalkResult Persisted =
				FPersistedStateWalker(Started + DeadlineSeconds, true).WalkObject(Object);
			Record.References = Persisted.References;
			if (!Persisted.bComplete)
			{
				Snapshot.bComplete = false;
				AddCaptureIssue(Snapshot, TEXT("persisted_state_incomplete"), TEXT("error"),
					Variant, ObjectPath, TEXT("Recursive persisted state or references exceeded a hard bound or used an unsupported property kind."));
			}
			if (const USoundWave* Wave = Cast<USoundWave>(Object))
			{
				Record.DurationSeconds = Wave->GetDuration();
				Record.SampleRate = Wave->GetSampleRateForCurrentPlatform();
				Record.NumChannels = Wave->NumChannels;
			}
			if (const USoundCue* Cue = Cast<USoundCue>(Object))
			{
#if WITH_EDITORONLY_DATA
				Record.PrimaryCount = Cue->AllNodes.Num();
#endif
			}
			if (const USoundClass* SoundClass = Cast<USoundClass>(Object))
			{
				Record.PrimaryCount = SoundClass->ChildClasses.Num();
				Record.SecondaryCount = SoundClass->PassiveSoundMixModifiers.Num();
				Record.Details.Add(FString::Printf(TEXT("volume=%.9g"), SoundClass->Properties.Volume));
				Record.Details.Add(FString::Printf(TEXT("pitch=%.9g"), SoundClass->Properties.Pitch));
			}
			FString FrontendCanonical;
			bool bFrontendTraversalComplete = true;
			const bool bMetaSound = InspectMetaSound(Object, Record, &Snapshot,
				FrontendCanonical, &bFrontendTraversalComplete, Started + DeadlineSeconds);
			if (bMetaSound)
			{
				Record.Details.Add(TEXT("observation=persisted_frontend_document"));
				Record.Details.Add(TEXT("graph_hash=")
					+ FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(FrontendCanonical));
			}
			Record.Revision = ComputeLoadedRevision(Object,
				bMetaSound ? &FrontendCanonical : nullptr,
				bFrontendTraversalComplete, Started + DeadlineSeconds, &Persisted);
			const FString Family = OptionalFamilyForVariant(Variant);
			if (!Family.IsEmpty())
			{
				FHyperAIAudioPrerequisite Prerequisite;
				IsGateReady(Family, &Prerequisite);
				Record.Details.Add(TEXT("prerequisite=") + Prerequisite.State);
			}
			AddRecord(Snapshot, MoveTemp(Record));
		};

		for (const FString& ExactPath : Request.ObjectPaths)
		{
			if (FPlatformTime::Seconds() - Started > DeadlineSeconds
				|| Snapshot.Scanned >= FHyperAIStudioAudioContracts::MaxScannedObjects)
			{
				Snapshot.bComplete = false;
				AddCaptureIssue(Snapshot, TEXT("loaded_exact_bound_reached"), TEXT("warning"),
					TEXT("loaded_only"), ExactPath,
					TEXT("Exact loaded-object inspection stopped at its count/deadline bound."));
				break;
			}
			++Snapshot.Scanned;
			CaptureObject(FindObject<UObject>(nullptr, *ExactPath));
			if (FPlatformTime::Seconds() - Started > DeadlineSeconds)
			{
				Snapshot.bComplete = false;
				AddCaptureIssue(Snapshot, TEXT("loaded_exact_bound_reached"), TEXT("warning"),
					TEXT("loaded_only"), ExactPath,
					TEXT("Exact loaded-object inspection exceeded its deadline."));
				break;
			}
		}
		return true;
	}

	bool CaptureOnDisk(
		const FHyperAIAudioInspectRequest& Request,
		FHyperAIAudioValueSnapshot& Snapshot,
		const double /*DeadlineSeconds*/)
	{
		Snapshot.bComplete = false;
		AddCaptureIssue(Snapshot, TEXT("async_on_disk_class_index_required"), TEXT("warning"),
			TEXT("on_disk_index"), Request.ObjectPaths.IsEmpty() ? FString() : Request.ObjectPaths[0],
			TEXT("Hard-bounded on-disk audio class inspection requires an asynchronous generation-cached typed index; the synchronous object lookup is intentionally not called."));
		return true;
	}

	bool ValidateProjection(const TArray<FString>& Projection, FString& OutError)
	{
		static const TSet<FString> Allowed = {
			TEXT("identity"), TEXT("package"), TEXT("metrics"), TEXT("graph"),
			TEXT("references"), TEXT("details")};
		return ValidateStringSet(Projection, FHyperAIStudioAudioContracts::MaxProjectionFields,
			[](const FString& Value) { return Allowed.Contains(Value); }, OutError);
	}

	void ProjectRecord(FHyperAIAudioRecord& Record, const TArray<FString>& Projection)
	{
		if (Projection.IsEmpty()) return;
		if (!Projection.Contains(TEXT("identity"))) Record.ClassPath.Reset();
		if (!Projection.Contains(TEXT("package"))) Record.PackageName.Reset();
		if (!Projection.Contains(TEXT("metrics")))
		{
			Record.DurationSeconds = 0.0;
			Record.SampleRate = 0.0;
			Record.NumChannels = 0;
		}
		if (!Projection.Contains(TEXT("graph")))
		{
			Record.PrimaryCount = 0;
			Record.SecondaryCount = 0;
			Record.TertiaryCount = 0;
		}
		if (!Projection.Contains(TEXT("references"))) Record.References.Reset();
		if (!Projection.Contains(TEXT("details"))) Record.Details.Reset();
	}

	int32 SaturatingJsonBytesFromCharacters(const int64 Characters)
	{
		// A UTF-16 code unit can require six ASCII bytes when JSON emits a \uXXXX
		// escape. This is deliberately conservative for control characters and
		// surrogate pairs and is evaluated before a record is copied to the result.
		if (Characters < 0 || Characters > MAX_int32 / 6) return MAX_int32;
		return static_cast<int32>(Characters * 6);
	}

	int32 EstimateBytes(const FHyperAIAudioRecord& Record)
	{
		int64 Characters = 256 + Record.Variant.Len() + Record.StableId.Len()
			+ Record.ObjectPath.Len() + Record.PackageName.Len() + Record.ClassPath.Len()
			+ Record.Revision.Len();
		for (const FString& Reference : Record.References) Characters += Reference.Len() + 16;
		for (const FString& Detail : Record.Details) Characters += Detail.Len() + 16;
		return SaturatingJsonBytesFromCharacters(Characters);
	}

	int32 EstimateIssueBytes(const FHyperAIAudioIssue& Issue)
	{
		const int64 Characters = 128 + Issue.Code.Len() + Issue.Severity.Len() + Issue.Variant.Len()
			+ Issue.ObjectPath.Len() + Issue.StableId.Len() + Issue.Message.Len();
		return SaturatingJsonBytesFromCharacters(Characters);
	}

	int32 EstimatePrerequisiteBytes(const FHyperAIAudioPrerequisite& Prerequisite)
	{
		const int64 Characters = 96 + Prerequisite.Family.Len() + Prerequisite.Plugin.Len()
			+ Prerequisite.Module.Len() + Prerequisite.State.Len();
		return SaturatingJsonBytesFromCharacters(Characters);
	}

	bool ParseCursor(const FString& Cursor, const FString& Revision, int32& OutOffset)
	{
		OutOffset = 0;
		if (Cursor.IsEmpty()) return true;
		TArray<FString> Parts;
		Cursor.ParseIntoArray(Parts, TEXT("|"), false);
		return Parts.Num() == 3 && Parts[0] == TEXT("v1") && Parts[1] == Revision
			&& LexTryParseString(OutOffset, *Parts[2]) && OutOffset >= 0;
	}

	FString MakeCursor(const FString& Revision, const int32 Offset)
	{
		return FString::Printf(TEXT("v1|%s|%d"), *Revision, Offset);
	}

	bool IsCreateVariant(const FString& Variant)
	{
		return Variant.EndsWith(TEXT(".create"), ESearchCase::CaseSensitive);
	}

	bool IsRemoveVariant(const FString& Variant)
	{
		return Variant.Contains(TEXT(".remove_"), ESearchCase::CaseSensitive)
			|| Variant == TEXT("metasound.remove_member") || Variant == TEXT("metasound.remove_node")
			|| Variant == TEXT("metasound.disconnect");
	}

	bool IsMetaSoundGraphVariant(const FString& Variant)
	{
		return Variant.StartsWith(TEXT("metasound."), ESearchCase::CaseSensitive);
	}

	bool IsOptionalOperation(const FString& Variant)
	{
		const FString Family = OptionalFamilyForVariant(Variant);
		return !Family.IsEmpty() && Family != TEXT("metasound");
	}

	bool IsSafeLiteralString(const FString& Value)
	{
		if (Value.Len() > 256) return false;
		for (const TCHAR Character : Value)
		{
			if (Character < TEXT(' ') || Character == TEXT('`')) return false;
		}
		return true;
	}

	bool ValidateParameter(const FHyperAIAudioParameterValue& Parameter, FString& OutError)
	{
		static const TSet<FString> Types = {
			TEXT("bool"), TEXT("int"), TEXT("float"), TEXT("string"), TEXT("object")};
		if (!IsSafeName(Parameter.Name) || !Types.Contains(Parameter.Type)
			|| !FMath::IsFinite(Parameter.FloatValue)
			|| FMath::Abs(Parameter.FloatValue) > 1000000000000.0
			|| FMath::Abs(static_cast<int64>(Parameter.IntValue)) > 1000000000LL)
		{
			OutError = TEXT("A parameter name, type, or scalar exceeds its closed bound.");
			return false;
		}
		if (Parameter.Type == TEXT("bool"))
		{
			if (Parameter.IntValue != 0 || Parameter.FloatValue != 0.0
				|| !Parameter.StringValue.IsEmpty() || !Parameter.ObjectPath.IsEmpty())
			{
				OutError = TEXT("bool parameters prohibit every non-bool value field.");
				return false;
			}
		}
		else if (Parameter.Type == TEXT("int"))
		{
			if (Parameter.BoolValue || Parameter.FloatValue != 0.0
				|| !Parameter.StringValue.IsEmpty() || !Parameter.ObjectPath.IsEmpty())
			{
				OutError = TEXT("int parameters prohibit every non-int value field.");
				return false;
			}
		}
		else if (Parameter.Type == TEXT("float"))
		{
			if (Parameter.BoolValue || Parameter.IntValue != 0
				|| !Parameter.StringValue.IsEmpty() || !Parameter.ObjectPath.IsEmpty())
			{
				OutError = TEXT("float parameters prohibit every non-float value field.");
				return false;
			}
		}
		else if (Parameter.Type == TEXT("string"))
		{
			if (Parameter.BoolValue || Parameter.IntValue != 0 || Parameter.FloatValue != 0.0
				|| !Parameter.ObjectPath.IsEmpty() || !IsSafeLiteralString(Parameter.StringValue))
			{
				OutError = TEXT("string parameters require one bounded non-executable string value.");
				return false;
			}
		}
		else if (Parameter.BoolValue || Parameter.IntValue != 0 || Parameter.FloatValue != 0.0
			|| !Parameter.StringValue.IsEmpty()
			|| !FHyperAIStudioAudioContracts::IsCanonicalProjectAssetPath(Parameter.ObjectPath))
		{
			OutError = TEXT("object parameters require one canonical /Game asset path.");
			return false;
		}
		return true;
	}

		struct FMetaSoundConfigureBindingSpec
		{
			FString Parameter;
			FName RegistryPort;
			FName RegistryType;
			FString ParameterType;
		};

		struct FMetaSoundNodeRegistrySpec
		{
			int32 DescriptorVersion = 1;
			FName Namespace;
			FName Name;
			FName Variant;
			int32 MajorVersion = 1;
			int32 MinorVersion = 1;
			TArray<FMetaSoundConfigureBindingSpec> ConfigureBindings;
		};

		const TMap<FString, FMetaSoundNodeRegistrySpec>& GetProvenMetaSoundNodeRegistrySpecs()
		{
			// Every entry is tied to one native UE 5.8 registration key. No synthetic
			// ports or vertex identities are an authoring authority.
			static const TMap<FString, FMetaSoundNodeRegistrySpec> Specs = {
				{TEXT("oscillator"), {1, TEXT("UE"), TEXT("Sine"), TEXT("Audio"), 1, 1, {
					{TEXT("frequency"), TEXT("Frequency"), TEXT("Float"), TEXT("float")},
					{TEXT("enabled"), TEXT("Enabled"), TEXT("Bool"), TEXT("bool")}}}},
				{TEXT("gain"), {1, TEXT("UE"), TEXT("Multiply"), TEXT("Audio by Float"), 1, 1, {
					{TEXT("volume"), TEXT("AdditionalOperands"), TEXT("Float"), TEXT("float")}}}},
				{TEXT("random"), {1, TEXT("UE"), TEXT("RandomFloat"), NAME_None, 1, 1, {
					{TEXT("min_value"), TEXT("Min"), TEXT("Float"), TEXT("float")},
					{TEXT("max_value"), TEXT("Max"), TEXT("Float"), TEXT("float")}}}},
				{TEXT("math_add"), {1, TEXT("UE"), TEXT("Add"), TEXT("Float"), 1, 1, {}}},
				{TEXT("math_multiply"), {1, TEXT("UE"), TEXT("Multiply"), TEXT("Float"), 1, 1, {}}}
			};
			return Specs;
		}

		Metasound::Frontend::FNodeRegistryKey MakeMetaSoundNodeRegistryKey(
			const FMetaSoundNodeRegistrySpec& Spec)
		{
			FMetasoundFrontendVersionNumber Version;
			Version.Major = Spec.MajorVersion;
			Version.Minor = Spec.MinorVersion;
			return Metasound::Frontend::FNodeRegistryKey(
				EMetasoundFrontendClassType::External,
				FMetasoundFrontendClassName(Spec.Namespace, Spec.Name, Spec.Variant), Version);
		}

	bool BuildMetaSoundDescriptorPorts(
			const FString& NodeKind,
			const int32 DescriptorVersion,
			const FString& DynamicDataType,
			FString& OutRegistryKey,
			FString& OutInterfaceFingerprint,
			TArray<FHyperAIAudioBackendMetaPort>& OutInputs,
		TArray<FHyperAIAudioBackendMetaPort>& OutOutputs,
		FString& OutError,
		const double AbsoluteDeadline = 0.0)
		{
			OutRegistryKey.Reset();
			OutInterfaceFingerprint.Reset();
			OutInputs.Reset();
			OutOutputs.Reset();
			const FMetaSoundNodeRegistrySpec* Spec = GetProvenMetaSoundNodeRegistrySpecs().Find(NodeKind);
			if (!Spec || Spec->DescriptorVersion != DescriptorVersion || !DynamicDataType.IsEmpty())
			{
				OutError = TEXT("MetaSound node kind/version is not tied to a proven native registry class.");
				return false;
			}
			Metasound::Frontend::INodeClassRegistry* Registry =
				Metasound::Frontend::INodeClassRegistry::Get();
			const Metasound::Frontend::FNodeRegistryKey RegistryKey = MakeMetaSoundNodeRegistryKey(*Spec);
			FMetasoundFrontendClass RegisteredClass;
			if (!Registry || !RegistryKey.IsValid() || !Registry->IsNodeRegistered(RegistryKey)
				|| !Registry->FindFrontendClassFromRegistered(RegistryKey, RegisteredClass))
			{
				OutError = TEXT("Required native MetaSound node class is not registered in this UE session.");
				return false;
			}
			const Metasound::Frontend::FNodeRegistryKey ReturnedKey(
				RegisteredClass.Metadata.GetType(), RegisteredClass.Metadata.GetClassName(),
				RegisteredClass.Metadata.GetVersion());
			if (ReturnedKey != RegistryKey)
			{
				OutError = TEXT("MetaSound registry returned metadata for a different node class.");
				return false;
			}
			OutRegistryKey = RegistryKey.ToString();
			OutInterfaceFingerprint = ComputeMetaSoundRegistryInterfaceFingerprint(
				RegistryKey, RegisteredClass, AbsoluteDeadline);
			if (!IsSha256(OutInterfaceFingerprint))
			{
				OutError = TEXT("MetaSound registered class interface could not be sealed completely.");
				return false;
			}
			TSet<FGuid> VertexIds;
			Metasound::Frontend::IDataTypeRegistry& DataTypeRegistry =
				Metasound::Frontend::IDataTypeRegistry::Get();
			auto ConfigureParameterFor = [&](const FName PortName, const FName PortType)
			{
				for (const FMetaSoundConfigureBindingSpec& Binding : Spec->ConfigureBindings)
				{
					if (Binding.RegistryPort == PortName && Binding.RegistryType == PortType)
						return Binding.Parameter;
				}
				return FString();
			};
			auto AppendPort = [&](const FMetasoundFrontendClassVertex& Vertex,
				const bool bInput, TArray<FHyperAIAudioBackendMetaPort>& OutPorts)
			{
				if (Vertex.Name.IsNone() || Vertex.TypeName.IsNone() || !Vertex.VertexID.IsValid()
					|| VertexIds.Contains(Vertex.VertexID)
					|| !DataTypeRegistry.IsRegistered(Vertex.TypeName)) return false;
				VertexIds.Add(Vertex.VertexID);
				FHyperAIAudioBackendMetaPort Port;
				Port.Name = Vertex.Name.ToString();
				Port.TypeName = Vertex.TypeName;
				Port.VertexId = Vertex.VertexID;
				Port.AccessType = static_cast<int32>(Vertex.AccessType);
				Port.bLiteralSettable = bInput
					&& DataTypeRegistry.GetDesiredLiteralType(Vertex.TypeName) != Metasound::ELiteralType::Invalid;
				Port.ConfigureParameter = bInput
					? ConfigureParameterFor(Vertex.Name, Vertex.TypeName) : FString();
				OutPorts.Add(MoveTemp(Port));
				return true;
			};
			const FMetasoundFrontendClassInterface& Interface = RegisteredClass.GetDefaultInterface();
			for (const FMetasoundFrontendClassInput& Input : Interface.Inputs)
				if (!AppendPort(Input, true, OutInputs))
				{
					OutError = TEXT("Registered MetaSound input interface is incomplete or ambiguous.");
					return false;
				}
			for (const FMetasoundFrontendClassOutput& Output : Interface.Outputs)
				if (!AppendPort(Output, false, OutOutputs))
				{
					OutError = TEXT("Registered MetaSound output interface is incomplete or ambiguous.");
					return false;
				}
			for (const FMetaSoundConfigureBindingSpec& Binding : Spec->ConfigureBindings)
			{
				if (!OutInputs.ContainsByPredicate([&](const FHyperAIAudioBackendMetaPort& Port)
					{ return Port.Name == Binding.RegistryPort.ToString()
						&& Port.TypeName == Binding.RegistryType
						&& Port.ConfigureParameter == Binding.Parameter; }))
				{
					OutError = TEXT("Proven MetaSound configure binding no longer matches the registered interface.");
					return false;
				}
			}
			return true;
		}

	FString ClosedMetaSoundNodeParameterType(const FString& NodeKind, const FString& Name)
	{
			const FMetaSoundNodeRegistrySpec* Spec = GetProvenMetaSoundNodeRegistrySpecs().Find(NodeKind);
			if (!Spec) return FString();
			for (const FMetaSoundConfigureBindingSpec& Binding : Spec->ConfigureBindings)
			{
				if (Name == Binding.Parameter) return Binding.ParameterType;
			}
		return FString();
	}

	bool ParameterMatchesMetaSoundType(
		const FHyperAIAudioParameterValue& Parameter,
		const FName TypeName)
	{
		if (TypeName == TEXT("Bool")) return Parameter.Type == TEXT("bool");
		if (TypeName == TEXT("Int32")) return Parameter.Type == TEXT("int");
		if (TypeName == TEXT("Float") || TypeName == TEXT("Time")) return Parameter.Type == TEXT("float");
		if (TypeName == TEXT("String")) return Parameter.Type == TEXT("string");
		if (TypeName == TEXT("WaveAsset") || TypeName == TEXT("Object")) return Parameter.Type == TEXT("object");
		return false;
	}

	FString ClosedParameterType(const FString& Variant, const FString& Name)
	{
		// Parameters are deliberately semantic and finite: adapters may never reinterpret
		// these names as arbitrary reflected property paths.
		static const TSet<FString> BoolNames = {
			TEXT("looping"), TEXT("streaming"), TEXT("seekable"), TEXT("apply_effects"),
			TEXT("always_play"), TEXT("is_music"), TEXT("center_channel_only"),
			TEXT("apply_eq"), TEXT("attenuate"), TEXT("spatialize"), TEXT("air_absorption"),
			TEXT("limit_to_owner"), TEXT("mature"), TEXT("bypass"), TEXT("bipolar"),
			TEXT("normalize"), TEXT("enabled"), TEXT("downmix"), TEXT("spatialized"),
			TEXT("randomize"), TEXT("noise_suppression"), TEXT("echo_cancellation")};
		static const TSet<FString> IntNames = {
			TEXT("compression_quality"), TEXT("streaming_chunk_size"), TEXT("max_count"),
			TEXT("resolution"), TEXT("fft_size"), TEXT("precision"), TEXT("channels"),
			TEXT("sample_rate"), TEXT("gear_count")};
		static const TSet<FString> FloatNames = {
			TEXT("volume"), TEXT("pitch"), TEXT("subtitle_priority"),
			TEXT("volume_multiplier"), TEXT("pitch_multiplier"), TEXT("low_pass_frequency"),
			TEXT("initial_delay"), TEXT("duration"), TEXT("fade_in_time"), TEXT("fade_out_time"),
			TEXT("inner_radius"), TEXT("falloff_distance"), TEXT("db_attenuation_at_max"),
			TEXT("volume_scale"), TEXT("duck_time"), TEXT("recover_time"),
			TEXT("retrigger_time"), TEXT("default_value"), TEXT("min_value"),
			TEXT("max_value"), TEXT("attack_time"), TEXT("release_time"), TEXT("mix_value"),
			TEXT("wet_level"), TEXT("dry_level"), TEXT("mix_level"), TEXT("feedback"),
			TEXT("delay_time"), TEXT("frequency"), TEXT("q"), TEXT("analysis_period"),
			TEXT("start_time"), TEXT("spawn_rate"), TEXT("fade_time"), TEXT("blend_time"),
			TEXT("priority"), TEXT("idle_rpm"), TEXT("max_rpm"), TEXT("gear_ratio"),
			TEXT("smoothing")};
		static const TSet<FString> StringNames = {
			TEXT("compression_type"), TEXT("distance_algorithm"),
			TEXT("spatialization_algorithm"), TEXT("resolution_rule"), TEXT("gender"),
			TEXT("plurality"), TEXT("spoken_text"), TEXT("subtitle_override"), TEXT("unit"),
			TEXT("parameter_class"), TEXT("curve_type"), TEXT("analyzer"),
			TEXT("orientation"), TEXT("units")};
		static const TSet<FString> ObjectNames = {
			TEXT("attenuation"), TEXT("concurrency"), TEXT("parent"),
			TEXT("default_submix"), TEXT("bus"), TEXT("parameter"), TEXT("sound"),
			TEXT("palette"), TEXT("reverb"), TEXT("submix"), TEXT("source")};

		auto TypeForName = [&](const FString& Candidate) -> FString
		{
			if (BoolNames.Contains(Candidate)) return TEXT("bool");
			if (IntNames.Contains(Candidate)) return TEXT("int");
			if (FloatNames.Contains(Candidate)) return TEXT("float");
			if (StringNames.Contains(Candidate)) return TEXT("string");
			if (ObjectNames.Contains(Candidate)) return TEXT("object");
			return FString();
		};
		auto In = [&](const TSet<FString>& Names) -> FString
		{
			return Names.Contains(Name) ? TypeForName(Name) : FString();
		};

		if (Variant == TEXT("metasound.set_literal"))
			return Name == TEXT("value") ? TEXT("any") : FString();
		const bool bLifecycleParameters = Variant.EndsWith(TEXT(".create"))
			|| Variant.EndsWith(TEXT(".configure"))
			|| Variant == TEXT("audio_capture.configure_component")
			|| Variant == TEXT("motor_sim.configure_model");
		if (!bLifecycleParameters) return FString();

		static const TSet<FString> SoundWave = {
			TEXT("volume"), TEXT("pitch"), TEXT("subtitle_priority"),
			TEXT("compression_quality"), TEXT("streaming_chunk_size"),
			TEXT("looping"), TEXT("streaming"), TEXT("seekable"), TEXT("compression_type")};
		static const TSet<FString> SoundCue = {
			TEXT("volume_multiplier"), TEXT("pitch_multiplier"),
			TEXT("attenuation"), TEXT("concurrency")};
		static const TSet<FString> SoundClass = {
			TEXT("volume"), TEXT("pitch"), TEXT("low_pass_frequency"),
			TEXT("apply_effects"), TEXT("always_play"), TEXT("is_music"),
			TEXT("center_channel_only"), TEXT("parent"), TEXT("default_submix")};
		static const TSet<FString> SoundMix = {
			TEXT("initial_delay"), TEXT("duration"), TEXT("fade_in_time"),
			TEXT("fade_out_time"), TEXT("apply_eq")};
		static const TSet<FString> Attenuation = {
			TEXT("inner_radius"), TEXT("falloff_distance"), TEXT("db_attenuation_at_max"),
			TEXT("attenuate"), TEXT("spatialize"), TEXT("air_absorption"),
			TEXT("distance_algorithm"), TEXT("spatialization_algorithm")};
		static const TSet<FString> Concurrency = {
			TEXT("max_count"), TEXT("volume_scale"), TEXT("duck_time"),
			TEXT("recover_time"), TEXT("retrigger_time"), TEXT("limit_to_owner"),
			TEXT("resolution_rule")};
		static const TSet<FString> DialogueVoice = {TEXT("gender"), TEXT("plurality")};
		static const TSet<FString> DialogueWave = {
			TEXT("spoken_text"), TEXT("subtitle_override"), TEXT("mature")};
		static const TSet<FString> Modulation = {
			TEXT("default_value"), TEXT("min_value"), TEXT("max_value"),
			TEXT("attack_time"), TEXT("release_time"), TEXT("mix_value"),
			TEXT("bypass"), TEXT("unit"), TEXT("parameter_class"),
			TEXT("bus"), TEXT("parameter")};
		static const TSet<FString> WaveTable = {
			TEXT("resolution"), TEXT("bipolar"), TEXT("normalize"), TEXT("curve_type")};
		static const TSet<FString> Synthesis = {
			TEXT("wet_level"), TEXT("dry_level"), TEXT("mix_level"), TEXT("feedback"),
			TEXT("delay_time"), TEXT("frequency"), TEXT("q"), TEXT("bypass"),
			TEXT("enabled")};
		static const TSet<FString> Synesthesia = {
			TEXT("analysis_period"), TEXT("start_time"), TEXT("duration"),
			TEXT("fft_size"), TEXT("downmix"), TEXT("analyzer"), TEXT("sound")};
		static const TSet<FString> Soundscape = {
			TEXT("volume"), TEXT("pitch"), TEXT("spawn_rate"), TEXT("fade_time"),
			TEXT("max_count"), TEXT("enabled"), TEXT("spatialized"),
			TEXT("sound"), TEXT("palette")};
		static const TSet<FString> Utility = {
			TEXT("volume"), TEXT("pitch"), TEXT("fade_in_time"), TEXT("fade_out_time"),
			TEXT("looping"), TEXT("randomize"), TEXT("sound"),
			TEXT("attenuation"), TEXT("concurrency")};
		static const TSet<FString> GameplayVolume = {
			TEXT("priority"), TEXT("blend_time"), TEXT("enabled"),
			TEXT("reverb"), TEXT("submix")};
		static const TSet<FString> Widget = {
			TEXT("min_value"), TEXT("max_value"), TEXT("precision"),
			TEXT("enabled"), TEXT("orientation"), TEXT("units")};
		static const TSet<FString> Capture = {
			TEXT("channels"), TEXT("sample_rate"),
			TEXT("noise_suppression"), TEXT("echo_cancellation")};
		static const TSet<FString> Motor = {
			TEXT("idle_rpm"), TEXT("max_rpm"), TEXT("gear_ratio"), TEXT("gear_count"),
			TEXT("volume"), TEXT("pitch"), TEXT("smoothing"), TEXT("enabled"),
			TEXT("source")};

		if (Variant.StartsWith(TEXT("sound_wave."))) return In(SoundWave);
		if (Variant.StartsWith(TEXT("sound_cue_template."))) return In(Utility);
		if (Variant.StartsWith(TEXT("sound_cue."))) return In(SoundCue);
		if (Variant.StartsWith(TEXT("sound_class."))) return In(SoundClass);
		if (Variant.StartsWith(TEXT("sound_mix."))) return In(SoundMix);
		if (Variant.StartsWith(TEXT("sound_attenuation."))) return In(Attenuation);
		if (Variant.StartsWith(TEXT("sound_concurrency."))) return In(Concurrency);
		if (Variant.StartsWith(TEXT("dialogue_voice."))) return In(DialogueVoice);
		if (Variant.StartsWith(TEXT("dialogue_wave."))) return In(DialogueWave);
		if (Variant.StartsWith(TEXT("modulation."))) return In(Modulation);
		if (Variant.StartsWith(TEXT("wavetable."))) return In(WaveTable);
		if (Variant.StartsWith(TEXT("synthesis."))) return In(Synthesis);
		if (Variant.StartsWith(TEXT("synesthesia."))) return In(Synesthesia);
		if (Variant.StartsWith(TEXT("soundscape."))) return In(Soundscape);
		if (Variant.StartsWith(TEXT("sound_utility."))) return In(Utility);
		if (Variant.StartsWith(TEXT("audio_gameplay_volume."))) return In(GameplayVolume);
		if (Variant.StartsWith(TEXT("audio_widget."))) return In(Widget);
		if (Variant.StartsWith(TEXT("audio_capture."))) return In(Capture);
		if (Variant.StartsWith(TEXT("motor_sim."))) return In(Motor);
		return FString();
	}

	bool ValidateClosedParameterDomain(
		const FString& Variant,
		const FHyperAIAudioParameterValue& Parameter,
		FString& OutError)
	{
		if (Parameter.Type == TEXT("string"))
		{
			static const TMap<FString, TSet<FString>> EnumDomains = {
				{TEXT("compression_type"), {TEXT("default"), TEXT("pcm"), TEXT("adpcm"), TEXT("bink_audio"), TEXT("oodle")}},
				{TEXT("distance_algorithm"), {TEXT("linear"), TEXT("logarithmic"), TEXT("inverse"), TEXT("log_reverse"), TEXT("natural_sound")}},
				{TEXT("spatialization_algorithm"), {TEXT("panning"), TEXT("binaural")}},
				{TEXT("resolution_rule"), {TEXT("prevent_new"), TEXT("stop_oldest"), TEXT("stop_farthest"), TEXT("stop_lowest_priority"), TEXT("stop_quietest")}},
				{TEXT("gender"), {TEXT("neuter"), TEXT("masculine"), TEXT("feminine"), TEXT("mixed")}},
				{TEXT("plurality"), {TEXT("singular"), TEXT("plural")}},
				{TEXT("unit"), {TEXT("normalized"), TEXT("frequency"), TEXT("volume"), TEXT("time")}},
				{TEXT("parameter_class"), {TEXT("scalar"), TEXT("bipolar"), TEXT("frequency"), TEXT("volume")}},
				{TEXT("curve_type"), {TEXT("linear"), TEXT("ease_in"), TEXT("ease_out"), TEXT("sine"), TEXT("custom")}},
				{TEXT("analyzer"), {TEXT("loudness"), TEXT("constant_q"), TEXT("onset"), TEXT("spectrum")}},
				{TEXT("orientation"), {TEXT("horizontal"), TEXT("vertical")}},
				{TEXT("units"), {TEXT("none"), TEXT("linear"), TEXT("decibels"), TEXT("hertz"), TEXT("seconds"), TEXT("percent")}}};
			if (const TSet<FString>* Allowed = EnumDomains.Find(Parameter.Name))
			{
				if (!Allowed->Contains(Parameter.StringValue))
				{
					OutError = TEXT("String parameter is outside its closed enum allowlist.");
					return false;
				}
			}
			return true;
		}

		double Minimum = -1000000.0;
		double Maximum = 1000000.0;
		if (Parameter.Name == TEXT("volume") || Parameter.Name == TEXT("volume_multiplier"))
			Minimum = 0.0, Maximum = 4.0;
		else if (Parameter.Name == TEXT("pitch") || Parameter.Name == TEXT("pitch_multiplier"))
			Minimum = 0.125, Maximum = 4.0;
		else if (Parameter.Name == TEXT("compression_quality")) Minimum = 0.0, Maximum = 100.0;
		else if (Parameter.Name == TEXT("streaming_chunk_size")) Minimum = 256.0, Maximum = 1048576.0;
		else if (Parameter.Name == TEXT("channels")) Minimum = 1.0, Maximum = 64.0;
		else if (Parameter.Name == TEXT("sample_rate")) Minimum = 8000.0, Maximum = 384000.0;
		else if (Parameter.Name == TEXT("max_count") || Parameter.Name == TEXT("gear_count"))
			Minimum = 1.0, Maximum = 1024.0;
		else if (Parameter.Name == TEXT("resolution") || Parameter.Name == TEXT("fft_size"))
			Minimum = 16.0, Maximum = 65536.0;
		else if (Parameter.Name == TEXT("precision")) Minimum = 0.0, Maximum = 9.0;
		else if (Parameter.Name == TEXT("frequency") || Parameter.Name == TEXT("low_pass_frequency"))
			Minimum = 20.0, Maximum = 24000.0;
		else if (Parameter.Name == TEXT("q")) Minimum = 0.01, Maximum = 100.0;
			else if (Parameter.Name == TEXT("mix_value") || Parameter.Name == TEXT("wet_level")
				|| Parameter.Name == TEXT("dry_level") || Parameter.Name == TEXT("mix_level")
				|| Parameter.Name == TEXT("feedback")) Minimum = 0.0, Maximum = 1.0;
			else if (Parameter.Name == TEXT("volume_scale")) Minimum = 0.0, Maximum = 1.0;
		else if (Parameter.Name == TEXT("initial_delay") || Parameter.Name == TEXT("duration")
			|| Parameter.Name == TEXT("fade_in_time") || Parameter.Name == TEXT("fade_out_time")
			|| Parameter.Name == TEXT("duck_time") || Parameter.Name == TEXT("recover_time")
			|| Parameter.Name == TEXT("retrigger_time") || Parameter.Name == TEXT("attack_time")
			|| Parameter.Name == TEXT("release_time") || Parameter.Name == TEXT("delay_time")
			|| Parameter.Name == TEXT("analysis_period") || Parameter.Name == TEXT("start_time")
			|| Parameter.Name == TEXT("fade_time") || Parameter.Name == TEXT("blend_time"))
			Minimum = 0.0, Maximum = 86400.0;
		else if (Parameter.Name == TEXT("inner_radius") || Parameter.Name == TEXT("falloff_distance"))
			Minimum = 0.0, Maximum = WORLD_MAX;
		else if (Parameter.Name == TEXT("db_attenuation_at_max")) Minimum = -160.0, Maximum = 0.0;
		else if (Parameter.Name == TEXT("idle_rpm") || Parameter.Name == TEXT("max_rpm"))
			Minimum = 0.0, Maximum = 200000.0;
		else if (Parameter.Name == TEXT("gear_ratio")) Minimum = -1000.0, Maximum = 1000.0;
		else if (Parameter.Name == TEXT("smoothing")) Minimum = 0.0, Maximum = 60.0;
		else if (Parameter.Name == TEXT("priority") || Parameter.Name == TEXT("subtitle_priority"))
			Minimum = -1000.0, Maximum = 1000.0;
		else if (Parameter.Name == TEXT("spawn_rate")) Minimum = 0.0, Maximum = 10000.0;

		if (Parameter.Type == TEXT("int"))
		{
			if (Parameter.IntValue < Minimum || Parameter.IntValue > Maximum)
			{
				OutError = TEXT("Integer parameter is outside its variant-specific domain.");
				return false;
			}
		}
		else if (Parameter.Type == TEXT("float")
			&& (Parameter.FloatValue < Minimum || Parameter.FloatValue > Maximum))
		{
			OutError = TEXT("Floating parameter is outside its variant-specific domain.");
			return false;
		}
		return true;
	}

	FString ParameterCanonical(const FHyperAIAudioParameterValue& Parameter)
	{
		FString Canonical;
		AppendToken(Canonical, Parameter.Name);
		AppendToken(Canonical, Parameter.Type);
		AppendBool(Canonical, Parameter.BoolValue);
		AppendInt(Canonical, Parameter.IntValue);
		AppendDouble(Canonical, Parameter.FloatValue);
		AppendToken(Canonical, Parameter.StringValue);
		AppendToken(Canonical, Parameter.ObjectPath);
		return Canonical;
	}

	FString OperationCanonical(const FHyperAIAudioBackendOperation& Operation)
	{
		FString Canonical;
		AppendToken(Canonical, TEXT("hyperai.audio-operation.v3"));
		AppendToken(Canonical, Operation.Variant);
		AppendToken(Canonical, Operation.TargetPath);
		AppendToken(Canonical, Operation.ExpectedRevision);
		AppendToken(Canonical, Operation.ReferencePath);
		for (const FString& Source : Operation.SourcePaths) AppendToken(Canonical, Source);
		AppendToken(Canonical, Operation.MemberName);
		AppendToken(Canonical, Operation.SecondaryName);
		AppendToken(Canonical, Operation.DataType);
			AppendToken(Canonical, Operation.NodeKind);
			AppendInt(Canonical, Operation.NodeDescriptorVersion);
			AppendToken(Canonical, Operation.MetaSoundRegistryKey);
			AppendToken(Canonical, Operation.MetaSoundInterfaceFingerprint);
		AppendGuid(Canonical, Operation.NodeId);
		AppendGuid(Canonical, Operation.VertexId);
		AppendToken(Canonical, Operation.VertexName);
		AppendGuid(Canonical, Operation.FromNodeId);
		AppendGuid(Canonical, Operation.FromVertexId);
		AppendToken(Canonical, Operation.FromPortName);
		AppendGuid(Canonical, Operation.ToNodeId);
		AppendGuid(Canonical, Operation.ToVertexId);
		AppendToken(Canonical, Operation.ToPortName);
		auto AppendDescriptorPort = [&](const FString& Direction,
			const FHyperAIAudioBackendMetaPort& Port)
		{
			AppendToken(Canonical, Direction);
			AppendToken(Canonical, Port.Name);
			AppendToken(Canonical, Port.TypeName.ToString());
			AppendGuid(Canonical, Port.VertexId);
			AppendInt(Canonical, Port.AccessType);
			AppendBool(Canonical, Port.bLiteralSettable);
			AppendToken(Canonical, Port.ConfigureParameter);
		};
		for (const FHyperAIAudioBackendMetaPort& Port : Operation.DescriptorInputs)
			AppendDescriptorPort(TEXT("input"), Port);
		for (const FHyperAIAudioBackendMetaPort& Port : Operation.DescriptorOutputs)
			AppendDescriptorPort(TEXT("output"), Port);
		AppendInt(Canonical, Operation.Index);
		AppendInt(Canonical, Operation.Count);
		AppendDouble(Canonical, Operation.Value);
		AppendDouble(Canonical, Operation.SecondaryValue);
		AppendBool(Canonical, Operation.bSetEnabled);
		AppendBool(Canonical, Operation.bEnabled);
		AppendVector(Canonical, Operation.Location);
		AppendVector(Canonical, Operation.Extent);
		for (const FHyperAIAudioParameterValue& Parameter : Operation.Parameters)
			AppendToken(Canonical, ParameterCanonical(Parameter));
		AppendToken(Canonical, Operation.OptionalFamily);
		return Canonical;
	}

	struct FShadowMetaPort
	{
		FGuid NodeId;
		FGuid VertexId;
		FString Name;
		FName TypeName;
		EMetasoundFrontendVertexAccessType Access = EMetasoundFrontendVertexAccessType::Reference;
		bool bLiteralSettable = false;
		FString ConfigureParameter;
	};

		struct FShadowMetaNodeDescriptor
		{
			FString NodeKind;
			FString DynamicDataType;
			int32 Version = 0;
			FString RegistryKey;
			FString InterfaceFingerprint;
		};

	struct FShadowAudioTarget
	{
		FString ObjectPath;
		FString Family;
		FString ClassPath;
		FString ClassKind;
		bool bCreated = false;
		TMap<FString, FString> Configuration;
		TMap<FString, double> NumericConfiguration;
		TSet<FString> KnownNumericConfiguration;
		TSet<FString> TouchedNumericConfiguration;
		TMap<FString, FString> References;
		TArray<FString> OrderedItems;
		bool bOrderedStateKnown = true;
		bool bReferenceStateKnown = true;
		TMap<FString, FString> MetaMembers;
		TSet<FString> MetaInterfaces;
		TSet<FGuid> MetaNodes;
		TMap<FGuid, FShadowMetaNodeDescriptor> MetaNodeDescriptors;
		TMap<FString, FShadowMetaPort> MetaInputs;
		TMap<FString, FShadowMetaPort> MetaOutputs;
		TMap<FString, FString> MetaLiterals;
		TSet<FString> MetaEdges;
		TSet<FString> MetaConnectedInputs;
	};

	FString OperationTargetFamily(const FString& Variant)
	{
		if (Variant == TEXT("metasound.source.create")) return TEXT("metasound_source");
		if (Variant == TEXT("metasound.patch.create")) return TEXT("metasound_patch");
		if (Variant.StartsWith(TEXT("metasound."))) return TEXT("metasound");
		if (Variant.StartsWith(TEXT("sound_wave."))) return TEXT("sound_wave");
		if (Variant.StartsWith(TEXT("sound_cue_template."))) return TEXT("sound_cue_template");
		if (Variant.StartsWith(TEXT("sound_cue."))) return TEXT("sound_cue");
		if (Variant.StartsWith(TEXT("sound_class."))) return TEXT("sound_class");
		if (Variant.StartsWith(TEXT("sound_mix."))) return TEXT("sound_mix");
		if (Variant.StartsWith(TEXT("sound_attenuation."))) return TEXT("sound_attenuation");
		if (Variant.StartsWith(TEXT("sound_concurrency."))) return TEXT("sound_concurrency");
		if (Variant.StartsWith(TEXT("dialogue_wave."))) return TEXT("dialogue_wave");
		if (Variant.StartsWith(TEXT("dialogue_voice."))) return TEXT("dialogue_voice");
		return OptionalFamilyForVariant(Variant);
	}

	FString OperationClassKind(const FString& Variant)
	{
		if (Variant.StartsWith(TEXT("metasound.source."))) return TEXT("metasound_source");
		if (Variant.StartsWith(TEXT("metasound.patch."))) return TEXT("metasound_patch");
		if (Variant.StartsWith(TEXT("metasound."))) return TEXT("metasound");
		static const TArray<FString> TwoSegmentKinds = {
			TEXT("modulation.parameter"), TEXT("modulation.bus"), TEXT("modulation.mix"),
			TEXT("modulation.patch"), TEXT("modulation.generator"), TEXT("wavetable.bank"),
			TEXT("synthesis.preset"), TEXT("synthesis.effect_chain"), TEXT("synesthesia.nrt"),
			TEXT("soundscape.palette"), TEXT("soundscape.color"),
			TEXT("sound_utility.sound_simple"), TEXT("audio_gameplay_volume"),
			TEXT("audio_widget"), TEXT("audio_capture"), TEXT("motor_sim")};
		for (const FString& Kind : TwoSegmentKinds)
			if (Variant == Kind || Variant.StartsWith(Kind + TEXT("."))) return Kind;
		return OperationTargetFamily(Variant);
	}

	FString ClassKindFromEvidence(const FAudioClassEvidence& E)
	{
		if (E.Is(TEXT("/Script/AudioModulation"), TEXT("SoundModulationParameter"))) return TEXT("modulation.parameter");
		if (E.Is(TEXT("/Script/AudioModulation"), TEXT("SoundControlBus"))) return TEXT("modulation.bus");
		if (E.Is(TEXT("/Script/AudioModulation"), TEXT("SoundControlBusMix"))) return TEXT("modulation.mix");
		if (E.Is(TEXT("/Script/AudioModulation"), TEXT("SoundModulationPatch"))) return TEXT("modulation.patch");
		if (E.Is(TEXT("/Script/AudioModulation"), TEXT("SoundModulationGenerator"))) return TEXT("modulation.generator");
		if (E.Is(TEXT("/Script/WaveTable"), TEXT("WaveTableBank"))) return TEXT("wavetable.bank");
		if (E.Is(TEXT("/Script/Engine"), TEXT("SoundEffectSourcePresetChain"))) return TEXT("synthesis.effect_chain");
		if (E.Is(TEXT("/Script/Engine"), TEXT("SoundEffectSourcePreset"))
			|| E.Is(TEXT("/Script/Engine"), TEXT("SoundEffectSubmixPreset"))) return TEXT("synthesis.preset");
		if (E.Is(TEXT("/Script/AudioSynesthesia"), TEXT("AudioSynesthesiaNRT"))) return TEXT("synesthesia.nrt");
		if (E.Is(TEXT("/Script/Soundscape"), TEXT("SoundscapePalette"))) return TEXT("soundscape.palette");
		if (E.Is(TEXT("/Script/Soundscape"), TEXT("SoundscapeColor"))) return TEXT("soundscape.color");
		if (E.Is(TEXT("/Script/SoundUtilities"), TEXT("SoundSimple"))) return TEXT("sound_utility.sound_simple");
		if (E.Is(TEXT("/Script/SoundCueTemplates"), TEXT("SoundCueTemplate"))) return TEXT("sound_cue_template");
		if (E.Is(TEXT("/Script/AudioGameplayVolume"), TEXT("AudioGameplayVolume"))) return TEXT("audio_gameplay_volume");
		if (E.Is(TEXT("/Script/AudioWidgets"), TEXT("AudioMeter"))
			|| E.Is(TEXT("/Script/AudioWidgets"), TEXT("AudioRadialSlider"))) return TEXT("audio_widget");
		if (E.Is(TEXT("/Script/AudioCapture"), TEXT("AudioCapture"))) return TEXT("audio_capture");
		if (E.Is(TEXT("/Script/AudioMotorSim"), TEXT("AudioMotorSim"))
			|| E.Is(TEXT("/Script/AudioMotorSimStandardComponents"), TEXT("MotorSimComponent"))) return TEXT("motor_sim");
		const FString Family = VariantFromClassEvidence(E);
		return Family;
	}

	bool FamilyMatches(const FString& Required, const FString& Observed)
	{
		return Required == Observed
			|| (Required == TEXT("metasound") && Observed.StartsWith(TEXT("metasound_")));
	}

	FString MetaVertexKey(const FGuid& NodeId, const FGuid& VertexId)
	{
		return NodeId.ToString(EGuidFormats::Digits) + TEXT(":")
			+ VertexId.ToString(EGuidFormats::Digits);
	}

	FString MetaEdgeKey(const FGuid& FromNode, const FGuid& FromVertex,
		const FGuid& ToNode, const FGuid& ToVertex)
	{
		return FromNode.ToString(EGuidFormats::Digits) + TEXT(":")
			+ FromVertex.ToString(EGuidFormats::Digits) + TEXT("->")
			+ ToNode.ToString(EGuidFormats::Digits) + TEXT(":")
			+ ToVertex.ToString(EGuidFormats::Digits);
	}

	bool MetaWouldCycle(const FShadowAudioTarget& State, const FGuid& From, const FGuid& To)
	{
		if (From == To) return true;
		TMap<FGuid, TArray<FGuid>> Adjacency;
		for (const FString& Encoded : State.MetaEdges)
		{
			TArray<FString> Sides;
			Encoded.ParseIntoArray(Sides, TEXT("->"), false);
			if (Sides.Num() != 2) continue;
			TArray<FString> Left;
			TArray<FString> Right;
			Sides[0].ParseIntoArray(Left, TEXT(":"), false);
			Sides[1].ParseIntoArray(Right, TEXT(":"), false);
			FGuid A;
			FGuid B;
			if (Left.Num() == 2 && Right.Num() == 2
				&& FGuid::ParseExact(Left[0], EGuidFormats::Digits, A)
				&& FGuid::ParseExact(Right[0], EGuidFormats::Digits, B))
				Adjacency.FindOrAdd(A).Add(B);
		}
		TArray<FGuid> Pending = {To};
		TSet<FGuid> Seen;
		while (!Pending.IsEmpty())
		{
			const FGuid Current = Pending.Pop(EAllowShrinking::No);
			if (Current == From) return true;
			if (Seen.Contains(Current)) continue;
			Seen.Add(Current);
			if (const TArray<FGuid>* Next = Adjacency.Find(Current)) Pending.Append(*Next);
		}
		return false;
	}

	bool FindPersistedArrayItems(
		const UObject* Object,
		const TArray<FName>& CandidateNames,
		TArray<FString>& OutItems,
		const double AbsoluteDeadline)
	{
		OutItems.Reset();
		if (!Object) return false;
		for (const FName Name : CandidateNames)
		{
			if (const FArrayProperty* Property = FindFProperty<FArrayProperty>(Object->GetClass(), Name))
			{
				if (Property->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient)) continue;
				FScriptArrayHelper Helper(Property,
					Property->ContainerPtrToValuePtr<void>(Object));
				if (Helper.Num() > MaxPersistedContainerElements) return false;
				OutItems.Reserve(Helper.Num());
				for (int32 Index = 0; Index < Helper.Num(); ++Index)
				{
					if (AbsoluteDeadline > 0.0 && FPlatformTime::Seconds() > AbsoluteDeadline)
					{
						OutItems.Reset();
						return false;
					}
					const FPersistedStateWalkResult Item = FPersistedStateWalker(
						AbsoluteDeadline, true).WalkPropertyValue(
							Property->Inner, Helper.GetRawPtr(Index), Object->GetPackage());
					if (!Item.bComplete || !IsSha256(Item.Fingerprint))
					{
						OutItems.Reset();
						return false;
					}
					OutItems.Add(Item.Fingerprint);
				}
				return true;
			}
		}
		return false;
	}

	bool InitializeShadowFromLoaded(
		const UObject* Object,
		FShadowAudioTarget& Out,
		FString& OutError,
		const double AbsoluteDeadline)
	{
		if (!Object)
		{
			OutError = TEXT("Loaded shadow initialization requires an exact object.");
			return false;
		}
		Out.ObjectPath = Object->GetPathName();
		Out.Family = VariantFromObject(Object);
		Out.ClassPath = Object->GetClass()->GetPathName();
		Out.ClassKind = ClassKindFromEvidence(EvidenceFromObject(Object));
		if (Out.Family.IsEmpty())
		{
			OutError = TEXT("Loaded target has no exact supported audio family.");
			return false;
		}
		if (const USoundClass* SoundClass = Cast<USoundClass>(Object))
			if (SoundClass->ParentClass) Out.References.Add(TEXT("parent"), SoundClass->ParentClass->GetPathName());
		if (const USoundMix* Mix = Cast<USoundMix>(Object))
		{
			Out.NumericConfiguration.Add(TEXT("initial_delay"), Mix->InitialDelay);
			Out.NumericConfiguration.Add(TEXT("duration"), Mix->Duration);
			Out.NumericConfiguration.Add(TEXT("fade_in_time"), Mix->FadeInTime);
			Out.NumericConfiguration.Add(TEXT("fade_out_time"), Mix->FadeOutTime);
			Out.KnownNumericConfiguration.Add(TEXT("initial_delay"));
			Out.KnownNumericConfiguration.Add(TEXT("duration"));
			Out.KnownNumericConfiguration.Add(TEXT("fade_in_time"));
			Out.KnownNumericConfiguration.Add(TEXT("fade_out_time"));
			for (const FSoundClassAdjuster& Adjuster : Mix->SoundClassEffects)
				Out.OrderedItems.Add(Adjuster.SoundClassObject
					? Adjuster.SoundClassObject->GetPathName() : TEXT("missing_sound_class"));
		}
		if (const USoundAttenuation* Attenuation = Cast<USoundAttenuation>(Object))
		{
			Out.NumericConfiguration.Add(TEXT("inner_radius"),
				static_cast<double>(Attenuation->Attenuation.AttenuationShapeExtents.X));
			Out.NumericConfiguration.Add(TEXT("falloff_distance"),
				static_cast<double>(Attenuation->Attenuation.FalloffDistance));
			Out.KnownNumericConfiguration.Add(TEXT("inner_radius"));
			Out.KnownNumericConfiguration.Add(TEXT("falloff_distance"));
		}
		else if (Out.Family == TEXT("dialogue_wave"))
		{
			// The public operation key includes caller-stable dialogue identifiers which
			// cannot be reconstructed losslessly from legacy context mappings.
			Out.bReferenceStateKnown = false;
		}
		else if (Out.Family == TEXT("audio_modulation"))
		{
			Out.bOrderedStateKnown = FindPersistedArrayItems(Object,
				{TEXT("Stages"), TEXT("MixStages"), TEXT("Inputs"), TEXT("Outputs")},
				Out.OrderedItems, AbsoluteDeadline);
		}
		else if (Out.Family == TEXT("synthesis_effect")
			|| Out.Family == TEXT("audio_gameplay_volume") || Out.Family == TEXT("motor_sim"))
		{
			Out.bOrderedStateKnown = FindPersistedArrayItems(Object,
				{TEXT("Chain"), TEXT("ChainEntries"), TEXT("Effects"), TEXT("Mutators"), TEXT("Components")},
				Out.OrderedItems, AbsoluteDeadline);
		}

		const IMetaSoundDocumentInterface* Interface = Cast<IMetaSoundDocumentInterface>(Object);
		if (!Interface) return true;
		FHyperAIAudioRecord AuditRecord;
		FString AuditCanonical;
		bool bAuditComplete = true;
		bool bAuditValid = false;
		if (!InspectMetaSound(Object, AuditRecord, nullptr, AuditCanonical,
			&bAuditComplete, AbsoluteDeadline, &bAuditValid)
			|| !bAuditComplete || !bAuditValid)
		{
			OutError = TEXT("Loaded MetaSound failed bounded persisted frontend validation before shadow replay.");
			return false;
		}
		const FMetasoundFrontendDocument& Document = Interface->GetConstDocument();
		if (Document.Interfaces.Contains(Metasound::Frontend::SourceInterface::GetVersion()))
			Out.MetaInterfaces.Add(TEXT("source"));
		if (Document.Interfaces.Contains(Metasound::Frontend::SourceOneShotInterface::GetVersion()))
			Out.MetaInterfaces.Add(TEXT("source_one_shot"));
		const FMetasoundFrontendClassInterface& Root = Document.RootGraph.GetDefaultInterface();
		for (const FMetasoundFrontendClassInput& Input : Root.Inputs)
		{
			const FString Name = Input.Name.ToString();
			const FString Key = TEXT("input:") + Name;
			if (Out.MetaMembers.Contains(Key)
				|| Out.MetaMembers.Contains(TEXT("variable:") + Name))
			{
				OutError = TEXT("Loaded MetaSound has a duplicate input or input/variable name collision.");
				return false;
			}
			Out.MetaMembers.Add(Key, Input.TypeName.ToString());
		}
		for (const FMetasoundFrontendClassOutput& Output : Root.Outputs)
		{
			const FString Name = Output.Name.ToString();
			const FString Key = TEXT("output:") + Name;
			if (Out.MetaMembers.Contains(Key)
				|| Out.MetaMembers.Contains(TEXT("variable:") + Name))
			{
				OutError = TEXT("Loaded MetaSound has a duplicate output or output/variable name collision.");
				return false;
			}
			Out.MetaMembers.Add(Key, Output.TypeName.ToString());
		}

		TMap<FGuid, const FMetasoundFrontendClass*> Classes;
		Classes.Add(Document.RootGraph.ID, &Document.RootGraph);
		for (const FMetasoundFrontendGraphClass& Subgraph : Document.Subgraphs) Classes.Add(Subgraph.ID, &Subgraph);
		for (const FMetasoundFrontendClass& Dependency : Document.Dependencies) Classes.Add(Dependency.ID, &Dependency);
		const TArray<const FMetasoundFrontendGraph*> MutablePages = {
			Document.RootGraph.FindConstGraph(Metasound::Frontend::DefaultPageID)};
		for (const FMetasoundFrontendGraph* MutablePage : MutablePages)
		{
			if (!MutablePage)
			{
				OutError = TEXT("Loaded MetaSound default page is unavailable.");
				return false;
			}
			const FMetasoundFrontendGraph& Page = *MutablePage;
			for (const FMetasoundFrontendVariable& Variable : Page.Variables)
			{
				const FString Name = Variable.Name.ToString();
				if (Out.MetaMembers.Contains(TEXT("input:") + Name)
					|| Out.MetaMembers.Contains(TEXT("output:") + Name)
					|| Out.MetaMembers.Contains(TEXT("variable:") + Name))
				{
					OutError = TEXT("Loaded MetaSound variable name collides with an existing root member.");
					return false;
				}
				Out.MetaMembers.Add(TEXT("variable:") + Name, Variable.TypeName.ToString());
			}
			for (const FMetasoundFrontendNode& Node : Page.Nodes)
			{
				const FGuid NodeId = Node.GetID();
				if (!NodeId.IsValid() || Out.MetaNodes.Contains(NodeId))
				{
					OutError = TEXT("Loaded MetaSound has duplicate/invalid node GUIDs.");
					return false;
					}
					Out.MetaNodes.Add(NodeId);
					const FMetasoundFrontendClass* const* FoundClass = Classes.Find(Node.ClassID);
				if (!FoundClass || !*FoundClass)
				{
					OutError = TEXT("Loaded MetaSound node class GUID is unresolved.");
					return false;
					}
					const FMetasoundFrontendClassInterface& ClassInterface = (*FoundClass)->GetInterfaceForNode(Node);
					TArray<FHyperAIAudioBackendMetaPort> ProvenInputs;
					TArray<FHyperAIAudioBackendMetaPort> ProvenOutputs;
					const FMetasoundFrontendClassMetadata& ClassMetadata = (*FoundClass)->Metadata;
					const Metasound::Frontend::FNodeRegistryKey ClassRegistryKey(
						ClassMetadata.GetType(), ClassMetadata.GetClassName(), ClassMetadata.GetVersion());
					for (const TPair<FString, FMetaSoundNodeRegistrySpec>& Proven :
						GetProvenMetaSoundNodeRegistrySpecs())
					{
						if (ClassRegistryKey != MakeMetaSoundNodeRegistryKey(Proven.Value)) continue;
						Metasound::Frontend::INodeClassRegistry* Registry =
							Metasound::Frontend::INodeClassRegistry::Get();
						FMetasoundFrontendClass RegisteredClass;
						if (!Registry || !Registry->IsNodeRegistered(ClassRegistryKey)
							|| !Registry->FindFrontendClassFromRegistered(ClassRegistryKey, RegisteredClass)
							|| !IsMetaSoundClassRegistryExact(
								**FoundClass, RegisteredClass, ClassRegistryKey, AbsoluteDeadline))
						{
							OutError = TEXT("Loaded MetaSound proven node kind no longer matches its native registry class.");
							return false;
						}
						FShadowMetaNodeDescriptor Descriptor;
						Descriptor.NodeKind = Proven.Key;
						Descriptor.Version = Proven.Value.DescriptorVersion;
						if (!BuildMetaSoundDescriptorPorts(Proven.Key, Descriptor.Version, FString(),
							Descriptor.RegistryKey, Descriptor.InterfaceFingerprint,
							ProvenInputs, ProvenOutputs, OutError, AbsoluteDeadline)) return false;
						const FString ObservedInterfaceFingerprint =
							ComputeMetaSoundRegistryInterfaceFingerprint(
								ClassRegistryKey, RegisteredClass, AbsoluteDeadline);
						if (!IsSha256(ObservedInterfaceFingerprint)
							|| Descriptor.RegistryKey != ClassRegistryKey.ToString()
							|| Descriptor.InterfaceFingerprint != ObservedInterfaceFingerprint)
						{
							OutError = TEXT("MetaSound registry class changed while sealing the loaded shadow.");
							return false;
						}
						Out.MetaNodeDescriptors.Add(NodeId, MoveTemp(Descriptor));
						break;
					}
					for (const FMetasoundFrontendVertex& Vertex : Node.Interface.Inputs)
				{
					const FString ExactVertexKey = MetaVertexKey(NodeId, Vertex.VertexID);
					if (Out.MetaInputs.Contains(ExactVertexKey))
					{
						OutError = TEXT("Loaded MetaSound has duplicate exact node/input vertex identities.");
						return false;
					}
					FShadowMetaPort Port;
					Port.NodeId = NodeId;
					Port.VertexId = Vertex.VertexID;
					Port.Name = Vertex.Name.ToString();
					Port.TypeName = Vertex.TypeName;
						Port.bLiteralSettable = Vertex.TypeName != TEXT("Audio")
							&& Vertex.TypeName != TEXT("Trigger");
						if (const FMetasoundFrontendClassInput* ClassInput = ClassInterface.Inputs.FindByPredicate(
							[&](const FMetasoundFrontendClassInput& Candidate) { return Candidate.Name == Vertex.Name; }))
							Port.Access = ClassInput->AccessType;
						if (const FHyperAIAudioBackendMetaPort* ProvenPort = ProvenInputs.FindByPredicate(
							[&](const FHyperAIAudioBackendMetaPort& Candidate)
							{
								return Candidate.VertexId == Vertex.VertexID
									&& Candidate.Name == Vertex.Name.ToString()
									&& Candidate.TypeName == Vertex.TypeName;
							}))
						{
							Port.Access = static_cast<EMetasoundFrontendVertexAccessType>(ProvenPort->AccessType);
							Port.bLiteralSettable = ProvenPort->bLiteralSettable;
							Port.ConfigureParameter = ProvenPort->ConfigureParameter;
						}
					Out.MetaInputs.Add(ExactVertexKey, Port);
				}
				for (const FMetasoundFrontendVertex& Vertex : Node.Interface.Outputs)
				{
					const FString ExactVertexKey = MetaVertexKey(NodeId, Vertex.VertexID);
					if (Out.MetaOutputs.Contains(ExactVertexKey))
					{
						OutError = TEXT("Loaded MetaSound has duplicate exact node/output vertex identities.");
						return false;
					}
					FShadowMetaPort Port;
					Port.NodeId = NodeId;
					Port.VertexId = Vertex.VertexID;
					Port.Name = Vertex.Name.ToString();
					Port.TypeName = Vertex.TypeName;
					if (const FMetasoundFrontendClassOutput* ClassOutput = ClassInterface.Outputs.FindByPredicate(
						[&](const FMetasoundFrontendClassOutput& Candidate) { return Candidate.Name == Vertex.Name; }))
						Port.Access = ClassOutput->AccessType;
					Out.MetaOutputs.Add(ExactVertexKey, Port);
				}
				for (const FMetasoundFrontendVertexLiteral& Literal : Node.InputLiterals)
				{
					const FString ExactVertexKey = MetaVertexKey(NodeId, Literal.VertexID);
					if (Out.MetaLiterals.Contains(ExactVertexKey))
					{
						OutError = TEXT("Loaded MetaSound has duplicate input literals.");
						return false;
					}
					const FPersistedStateWalkResult LiteralState =
						FPersistedStateWalker(AbsoluteDeadline, false).WalkScriptStruct(
							FMetasoundFrontendLiteral::StaticStruct(), &Literal.Value);
					if (!LiteralState.bComplete || !IsSha256(LiteralState.Fingerprint))
					{
						OutError = TEXT("Loaded MetaSound literal exceeded bounded shadow hashing.");
						return false;
					}
					Out.MetaLiterals.Add(ExactVertexKey, FString::Printf(TEXT("%d:%s"),
						static_cast<int32>(Literal.Value.GetType()), *LiteralState.Fingerprint));
				}
			}
			for (const FMetasoundFrontendEdge& Edge : Page.Edges)
			{
				const FString Key = MetaEdgeKey(Edge.FromNodeID, Edge.FromVertexID, Edge.ToNodeID, Edge.ToVertexID);
				const FString ConnectedInput = MetaVertexKey(Edge.ToNodeID, Edge.ToVertexID);
				if (Out.MetaEdges.Contains(Key) || Out.MetaConnectedInputs.Contains(ConnectedInput))
				{
					OutError = TEXT("Loaded MetaSound has duplicate edges or multiply-connected inputs.");
					return false;
				}
				Out.MetaEdges.Add(Key);
				Out.MetaConnectedInputs.Add(ConnectedInput);
			}
		}
		return true;
	}

	const FShadowMetaPort* ResolveShadowMetaPort(
		const TMap<FString, FShadowMetaPort>& Ports,
		const FGuid& NodeId,
		const FGuid& RequestedVertexId,
		const FString& RequestedName,
		FGuid& OutVertexId)
	{
		OutVertexId.Invalidate();
		if (RequestedVertexId.IsValid())
		{
			const FShadowMetaPort* Port = Ports.Find(MetaVertexKey(NodeId, RequestedVertexId));
			if (!Port || !RequestedName.IsEmpty()) return nullptr;
			OutVertexId = RequestedVertexId;
			return Port;
		}
		if (RequestedName.IsEmpty()) return nullptr;
		const FShadowMetaPort* Match = nullptr;
		for (const TPair<FString, FShadowMetaPort>& Pair : Ports)
		{
			if (Pair.Value.NodeId != NodeId || Pair.Value.Name != RequestedName) continue;
			if (Match) return nullptr;
			Match = &Pair.Value;
		}
		if (Match) OutVertexId = Match->VertexId;
		return Match;
	}

	void CopyShadowDescriptorPortsToBackend(
		const FShadowAudioTarget& State,
		const FGuid& NodeId,
		FHyperAIAudioBackendOperation& Operation)
	{
			Operation.DescriptorInputs.Reset();
			Operation.DescriptorOutputs.Reset();
			if (const FShadowMetaNodeDescriptor* Descriptor = State.MetaNodeDescriptors.Find(NodeId))
			{
				Operation.MetaSoundRegistryKey = Descriptor->RegistryKey;
				Operation.MetaSoundInterfaceFingerprint = Descriptor->InterfaceFingerprint;
			}
		auto Copy = [&](const TMap<FString, FShadowMetaPort>& Ports,
			TArray<FHyperAIAudioBackendMetaPort>& OutPorts)
		{
			for (const TPair<FString, FShadowMetaPort>& Pair : Ports)
			{
				if (Pair.Value.NodeId != NodeId) continue;
				FHyperAIAudioBackendMetaPort Port;
				Port.Name = Pair.Value.Name;
				Port.TypeName = Pair.Value.TypeName;
				Port.VertexId = Pair.Value.VertexId;
				Port.AccessType = static_cast<int32>(Pair.Value.Access);
				Port.bLiteralSettable = Pair.Value.bLiteralSettable;
				Port.ConfigureParameter = Pair.Value.ConfigureParameter;
				OutPorts.Add(MoveTemp(Port));
			}
			OutPorts.Sort([](const FHyperAIAudioBackendMetaPort& Left,
				const FHyperAIAudioBackendMetaPort& Right)
			{
				return Left.Name < Right.Name;
			});
		};
		Copy(State.MetaInputs, Operation.DescriptorInputs);
		Copy(State.MetaOutputs, Operation.DescriptorOutputs);
	}

	bool ReplayShadowOperation(
		FHyperAIAudioBackendOperation& Operation,
		FShadowAudioTarget& State,
		const TMap<FString, FShadowAudioTarget>& AllStates,
		FString& OutError)
	{
		const FString RequiredFamily = OperationTargetFamily(Operation.Variant);
		if (!FamilyMatches(RequiredFamily, State.Family))
		{
			OutError = TEXT("Operation family does not match the immutable typed target shadow.");
			return false;
		}
		const FString RequiredClassKind = OperationClassKind(Operation.Variant);
		if (!(RequiredClassKind == TEXT("metasound") && State.ClassKind.StartsWith(TEXT("metasound_")))
			&& !State.ClassKind.IsEmpty() && RequiredClassKind != State.ClassKind)
		{
			OutError = TEXT("Operation class kind does not match the immutable typed target shadow.");
			return false;
		}
		for (const FHyperAIAudioParameterValue& Parameter : Operation.Parameters)
		{
			const FString Key = Operation.Variant == TEXT("metasound.configure_node")
				? TEXT("node:") + Operation.NodeId.ToString(EGuidFormats::Digits)
					+ TEXT(":") + Parameter.Name
				: Parameter.Name;
			State.Configuration.Add(Key, ParameterCanonical(Parameter));
			if (Parameter.Type == TEXT("float") || Parameter.Type == TEXT("int"))
			{
				const double NumericValue = Parameter.Type == TEXT("float")
					? Parameter.FloatValue : static_cast<double>(Parameter.IntValue);
				State.NumericConfiguration.Add(Key, NumericValue);
				State.KnownNumericConfiguration.Add(Key);
				State.TouchedNumericConfiguration.Add(Key);
			}
		}
		if (Operation.Variant.StartsWith(TEXT("sound_cue_template."))
			|| Operation.Variant.StartsWith(TEXT("sound_utility.sound_simple."))
			|| Operation.Variant.StartsWith(TEXT("soundscape.palette.")))
		{
			TArray<FString> OldSourceKeys;
			for (const TPair<FString, FString>& Pair : State.Configuration)
				if (Pair.Key.StartsWith(TEXT("source:"))) OldSourceKeys.Add(Pair.Key);
			for (const FString& Key : OldSourceKeys) State.Configuration.Remove(Key);
		}
		for (int32 SourceIndex = 0; SourceIndex < Operation.SourcePaths.Num(); ++SourceIndex)
			State.Configuration.Add(FString::Printf(TEXT("source:%d"), SourceIndex),
				Operation.SourcePaths[SourceIndex]);
		if (Operation.bSetEnabled)
			State.Configuration.Add(TEXT("enabled"), Operation.bEnabled ? TEXT("true") : TEXT("false"));
		if (Operation.Location != FVector::ZeroVector || Operation.Extent != FVector::ZeroVector)
		{
			State.Configuration.Add(TEXT("location"), Operation.Location.ToString());
			State.Configuration.Add(TEXT("extent"), Operation.Extent.ToString());
		}

		if (Operation.Variant == TEXT("sound_class.set_parent"))
		{
			if (Operation.ReferencePath == State.ObjectPath)
			{
				OutError = TEXT("SoundClass cannot parent itself.");
				return false;
			}
			FString Cursor = Operation.ReferencePath;
			TSet<FString> Seen;
			while (!Cursor.IsEmpty())
			{
				if (Cursor == State.ObjectPath)
				{
					OutError = TEXT("SoundClass parent would create a reference cycle.");
					return false;
				}
				if (Seen.Contains(Cursor)) break;
				Seen.Add(Cursor);
				if (const FShadowAudioTarget* Parent = AllStates.Find(Cursor))
					Cursor = Parent->References.FindRef(TEXT("parent"));
				else if (const USoundClass* LoadedParent = FindObject<USoundClass>(nullptr, *Cursor))
					Cursor = LoadedParent->ParentClass ? LoadedParent->ParentClass->GetPathName() : FString();
				else
				{
					OutError = TEXT("SoundClass parent ancestry must be loaded for complete cycle validation.");
					return false;
				}
			}
			State.References.Add(TEXT("parent"), Operation.ReferencePath);
		}
		else if (Operation.Variant == TEXT("sound_mix.add_adjuster")
			|| Operation.Variant == TEXT("sound_mix.remove_adjuster"))
		{
			const int32 ExistingIndex = State.OrderedItems.IndexOfByKey(Operation.ReferencePath);
			if (Operation.Variant == TEXT("sound_mix.add_adjuster"))
			{
				if (ExistingIndex != INDEX_NONE)
				{
					OutError = TEXT("SoundMix adjuster reference is duplicated.");
					return false;
				}
				State.OrderedItems.Add(Operation.ReferencePath);
			}
			else
			{
				if (ExistingIndex == INDEX_NONE)
				{
					OutError = TEXT("SoundMix adjuster removal targets a missing reference.");
					return false;
				}
				State.OrderedItems.RemoveAt(ExistingIndex);
			}
		}
		else if (!Operation.ReferencePath.IsEmpty()
			&& !Operation.Variant.StartsWith(TEXT("modulation.mix.")))
		{
			if (Operation.Variant.StartsWith(TEXT("dialogue_wave."))
				&& !State.bReferenceStateKnown)
			{
				OutError = TEXT("Loaded dialogue context keys cannot be reconstructed losslessly for duplicate/missing validation.");
				return false;
			}
			FString Key = Operation.Variant + TEXT(":") + Operation.MemberName
				+ TEXT(":") + Operation.SecondaryName;
			if (Operation.Variant.StartsWith(TEXT("dialogue_wave.")))
				Key = TEXT("dialogue_context:") + Operation.MemberName + TEXT(":") + Operation.SecondaryName;
			else if (Operation.Variant.StartsWith(TEXT("modulation.mix.")))
				Key = TEXT("modulation_stage:") + FString::FromInt(Operation.Index);
			else if (Operation.Variant == TEXT("sound_cue.set_root_wave")) Key = TEXT("root_wave");
			else if (Operation.Variant == TEXT("synesthesia.nrt.set_sound")) Key = TEXT("sound");
			if (IsRemoveVariant(Operation.Variant))
			{
				if (State.References.FindRef(Key) != Operation.ReferencePath)
				{
					OutError = TEXT("Typed reference removal targets a missing shadow entry.");
					return false;
				}
				State.References.Remove(Key);
			}
			else
			{
				if ((Operation.Variant.StartsWith(TEXT("dialogue_wave."))
					|| Operation.Variant.StartsWith(TEXT("modulation.mix.")))
					&& State.References.Contains(Key))
				{
					OutError = TEXT("Typed reference addition duplicates an existing shadow entry.");
					return false;
				}
				State.References.Add(Key, Operation.ReferencePath);
			}
		}

		if (Operation.Variant == TEXT("metasound.add_input")
			|| Operation.Variant == TEXT("metasound.add_output")
			|| Operation.Variant == TEXT("metasound.add_variable"))
		{
			const FString Kind = Operation.Variant.RightChop(14);
			const FString Key = Kind + TEXT(":") + Operation.MemberName;
			const bool bVariableCollision = Kind == TEXT("variable")
				? State.MetaMembers.Contains(TEXT("input:") + Operation.MemberName)
					|| State.MetaMembers.Contains(TEXT("output:") + Operation.MemberName)
				: State.MetaMembers.Contains(TEXT("variable:") + Operation.MemberName);
			if (State.MetaMembers.Contains(Key) || bVariableCollision)
			{
				OutError = TEXT("MetaSound member already exists in its direction or collides with a variable.");
				return false;
			}
			State.MetaMembers.Add(Key, Operation.DataType);
		}
		else if (Operation.Variant == TEXT("metasound.remove_member"))
		{
			const FString Key = Operation.DataType + TEXT(":") + Operation.MemberName;
			if (!State.MetaMembers.Contains(Key))
			{
				OutError = TEXT("MetaSound member removal targets a missing member or wrong member kind.");
				return false;
			}
			State.MetaMembers.Remove(Key);
		}
		else if (Operation.Variant == TEXT("metasound.declare_interface"))
		{
			if (State.MetaInterfaces.Contains(Operation.MemberName))
			{
				OutError = TEXT("MetaSound interface declaration is duplicated.");
				return false;
			}
			State.MetaInterfaces.Add(Operation.MemberName);
		}
			else if (Operation.Variant == TEXT("metasound.add_node"))
			{
				if (State.MetaNodes.Contains(Operation.NodeId)
					|| State.MetaNodeDescriptors.Contains(Operation.NodeId)
					|| (Operation.DescriptorInputs.IsEmpty() && Operation.DescriptorOutputs.IsEmpty())
					|| Operation.MetaSoundRegistryKey.IsEmpty()
					|| !IsSha256(Operation.MetaSoundInterfaceFingerprint))
			{
				OutError = TEXT("MetaSound node GUID already exists or its typed descriptor is empty.");
				return false;
			}
			State.MetaNodes.Add(Operation.NodeId);
			FShadowMetaNodeDescriptor Descriptor;
				Descriptor.NodeKind = Operation.NodeKind;
				Descriptor.DynamicDataType = Operation.DataType;
				Descriptor.Version = Operation.NodeDescriptorVersion;
				Descriptor.RegistryKey = Operation.MetaSoundRegistryKey;
				Descriptor.InterfaceFingerprint = Operation.MetaSoundInterfaceFingerprint;
				State.MetaNodeDescriptors.Add(Operation.NodeId, MoveTemp(Descriptor));
			auto AddDescriptorPorts = [&](const TArray<FHyperAIAudioBackendMetaPort>& Ports,
				TMap<FString, FShadowMetaPort>& ShadowPorts)
			{
				for (const FHyperAIAudioBackendMetaPort& Port : Ports)
				{
					const FString Key = MetaVertexKey(Operation.NodeId, Port.VertexId);
					if (ShadowPorts.Contains(Key)) return false;
					FShadowMetaPort ShadowPort;
					ShadowPort.NodeId = Operation.NodeId;
					ShadowPort.VertexId = Port.VertexId;
					ShadowPort.Name = Port.Name;
					ShadowPort.TypeName = Port.TypeName;
					ShadowPort.Access = static_cast<EMetasoundFrontendVertexAccessType>(Port.AccessType);
					ShadowPort.bLiteralSettable = Port.bLiteralSettable;
					ShadowPort.ConfigureParameter = Port.ConfigureParameter;
					ShadowPorts.Add(Key, MoveTemp(ShadowPort));
				}
				return true;
			};
			if (!AddDescriptorPorts(Operation.DescriptorInputs, State.MetaInputs)
				|| !AddDescriptorPorts(Operation.DescriptorOutputs, State.MetaOutputs))
			{
				OutError = TEXT("MetaSound descriptor contains duplicate exact node/vertex identities.");
				return false;
			}
		}
		else if (Operation.Variant == TEXT("metasound.configure_node"))
		{
			const FShadowMetaNodeDescriptor* Descriptor =
				State.MetaNodeDescriptors.Find(Operation.NodeId);
				if (!Descriptor || Descriptor->NodeKind != Operation.NodeKind
					|| Descriptor->Version != Operation.NodeDescriptorVersion
					|| Descriptor->RegistryKey.IsEmpty()
					|| !IsSha256(Descriptor->InterfaceFingerprint))
			{
				OutError = TEXT("MetaSound configure_node targets no matching versioned typed descriptor.");
				return false;
			}
			CopyShadowDescriptorPortsToBackend(State, Operation.NodeId, Operation);
			for (const FHyperAIAudioParameterValue& Parameter : Operation.Parameters)
			{
				const FShadowMetaPort* ConfiguredPort = nullptr;
				for (const TPair<FString, FShadowMetaPort>& Pair : State.MetaInputs)
				{
					if (Pair.Value.NodeId != Operation.NodeId
						|| Pair.Value.ConfigureParameter != Parameter.Name) continue;
					if (ConfiguredPort)
					{
						OutError = TEXT("MetaSound descriptor configuration mapping is ambiguous.");
						return false;
					}
					ConfiguredPort = &Pair.Value;
				}
				if (!ConfiguredPort || !ConfiguredPort->bLiteralSettable
					|| !ParameterMatchesMetaSoundType(Parameter, ConfiguredPort->TypeName))
				{
					OutError = TEXT("MetaSound configure_node parameter has no exact typed descriptor input.");
					return false;
				}
				State.MetaLiterals.Add(MetaVertexKey(Operation.NodeId, ConfiguredPort->VertexId),
					ParameterCanonical(Parameter));
			}
		}
		else if (Operation.Variant == TEXT("metasound.remove_node"))
		{
			if (!State.MetaNodes.Contains(Operation.NodeId))
			{
				OutError = TEXT("MetaSound node removal targets a missing node GUID.");
				return false;
			}
			for (const TPair<FString, FShadowMetaPort>& Pair : State.MetaInputs)
				if (Pair.Value.NodeId == Operation.NodeId && State.MetaConnectedInputs.Contains(Pair.Key))
				{
					OutError = TEXT("MetaSound node removal requires its persisted edges to be disconnected first.");
					return false;
				}
			for (const FString& Edge : State.MetaEdges)
				if (Edge.StartsWith(Operation.NodeId.ToString(EGuidFormats::Digits))
					|| Edge.Contains(TEXT("->") + Operation.NodeId.ToString(EGuidFormats::Digits) + TEXT(":")))
				{
					OutError = TEXT("MetaSound node removal requires its persisted edges to be disconnected first.");
					return false;
			}
			State.MetaNodes.Remove(Operation.NodeId);
			State.MetaNodeDescriptors.Remove(Operation.NodeId);
			TArray<FString> InputsToRemove;
			TArray<FString> OutputsToRemove;
			for (const TPair<FString, FShadowMetaPort>& Pair : State.MetaInputs)
				if (Pair.Value.NodeId == Operation.NodeId) InputsToRemove.Add(Pair.Key);
			for (const TPair<FString, FShadowMetaPort>& Pair : State.MetaOutputs)
				if (Pair.Value.NodeId == Operation.NodeId) OutputsToRemove.Add(Pair.Key);
			for (const FString& Id : InputsToRemove)
			{
				State.MetaInputs.Remove(Id);
				State.MetaLiterals.Remove(Id);
			}
			for (const FString& Id : OutputsToRemove) State.MetaOutputs.Remove(Id);
		}
		else if (Operation.Variant == TEXT("metasound.connect"))
		{
			FGuid ResolvedFrom;
			FGuid ResolvedTo;
			const FShadowMetaPort* From = ResolveShadowMetaPort(State.MetaOutputs,
				Operation.FromNodeId, Operation.FromVertexId, Operation.FromPortName, ResolvedFrom);
			const FShadowMetaPort* To = ResolveShadowMetaPort(State.MetaInputs,
				Operation.ToNodeId, Operation.ToVertexId, Operation.ToPortName, ResolvedTo);
			Operation.FromVertexId = ResolvedFrom;
			Operation.ToVertexId = ResolvedTo;
			const FString Key = MetaEdgeKey(Operation.FromNodeId, Operation.FromVertexId,
				Operation.ToNodeId, Operation.ToVertexId);
			if (!From || !To || From->NodeId != Operation.FromNodeId || To->NodeId != Operation.ToNodeId
				|| !IsMetaSoundTypeCastable(From->TypeName, To->TypeName)
				|| !FMetasoundFrontendClassVertex::CanConnectVertexAccessTypes(From->Access, To->Access)
				|| State.MetaEdges.Contains(Key)
				|| State.MetaConnectedInputs.Contains(MetaVertexKey(Operation.ToNodeId, Operation.ToVertexId))
				|| MetaWouldCycle(State, Operation.FromNodeId, Operation.ToNodeId))
			{
				OutError = TEXT("MetaSound edge fails GUID, port, type/access, uniqueness, or cycle invariants.");
				return false;
			}
			State.MetaEdges.Add(Key);
			State.MetaConnectedInputs.Add(MetaVertexKey(Operation.ToNodeId, Operation.ToVertexId));
		}
		else if (Operation.Variant == TEXT("metasound.disconnect"))
		{
			FGuid ResolvedFrom;
			FGuid ResolvedTo;
			if (!ResolveShadowMetaPort(State.MetaOutputs, Operation.FromNodeId,
				Operation.FromVertexId, Operation.FromPortName, ResolvedFrom)
				|| !ResolveShadowMetaPort(State.MetaInputs, Operation.ToNodeId,
					Operation.ToVertexId, Operation.ToPortName, ResolvedTo))
			{
				OutError = TEXT("MetaSound disconnect could not resolve its exact typed ports.");
				return false;
			}
			Operation.FromVertexId = ResolvedFrom;
			Operation.ToVertexId = ResolvedTo;
			const FString Key = MetaEdgeKey(Operation.FromNodeId, Operation.FromVertexId,
				Operation.ToNodeId, Operation.ToVertexId);
			if (!State.MetaEdges.Remove(Key))
			{
				OutError = TEXT("MetaSound disconnect targets a missing exact edge.");
				return false;
			}
			State.MetaConnectedInputs.Remove(MetaVertexKey(Operation.ToNodeId, Operation.ToVertexId));
		}
		else if (Operation.Variant == TEXT("metasound.set_literal"))
		{
			FGuid ResolvedVertex;
			const FShadowMetaPort* Input = ResolveShadowMetaPort(State.MetaInputs,
				Operation.NodeId, Operation.VertexId, Operation.VertexName, ResolvedVertex);
			Operation.VertexId = ResolvedVertex;
			if (!State.MetaNodes.Contains(Operation.NodeId) || !Input || Input->NodeId != Operation.NodeId
				|| !Input->bLiteralSettable || Operation.Parameters.Num() != 1
				|| !ParameterMatchesMetaSoundType(Operation.Parameters[0], Input->TypeName))
			{
				OutError = TEXT("MetaSound literal targets no exact literal-settable typed node input.");
				return false;
			}
			State.MetaLiterals.Add(MetaVertexKey(Operation.NodeId, Operation.VertexId),
				Operation.Parameters.IsEmpty() ? FString() : ParameterCanonical(Operation.Parameters[0]));
		}

		const bool bOrderedAdd = Operation.Variant == TEXT("modulation.mix.add_stage")
			|| Operation.Variant == TEXT("synthesis.effect_chain.add_effect")
			|| Operation.Variant == TEXT("audio_gameplay_volume.add_mutator")
			|| Operation.Variant == TEXT("motor_sim.add_component");
		const bool bOrderedRemove = Operation.Variant == TEXT("modulation.mix.remove_stage")
			|| Operation.Variant == TEXT("synthesis.effect_chain.remove_effect")
			|| Operation.Variant == TEXT("audio_gameplay_volume.remove_mutator")
			|| Operation.Variant == TEXT("motor_sim.remove_component");
		if (bOrderedAdd || bOrderedRemove)
		{
			if (!State.bOrderedStateKnown)
			{
				OutError = TEXT("Persisted ordered collection could not be observed without an optional typed adapter.");
				return false;
			}
			if (bOrderedAdd)
			{
				const int32 InsertAt = Operation.Index < 0 ? State.OrderedItems.Num() : Operation.Index;
				if (InsertAt < 0 || InsertAt > State.OrderedItems.Num())
				{
					OutError = TEXT("Ordered audio insertion index is outside the immutable shadow.");
					return false;
				}
				const FString Item = Operation.NodeKind.IsEmpty()
					? Operation.ReferencePath : Operation.NodeKind;
				if (!Item.IsEmpty() && State.OrderedItems.Contains(Item))
				{
					OutError = TEXT("Ordered audio insertion duplicates an existing semantic entry.");
					return false;
				}
				State.OrderedItems.Insert(Item, InsertAt);
			}
			else
			{
				if (!State.OrderedItems.IsValidIndex(Operation.Index))
				{
					OutError = TEXT("Ordered audio removal index is absent from the immutable shadow.");
					return false;
				}
				State.OrderedItems.RemoveAt(Operation.Index);
			}
		}
		return true;
	}

	bool ValidateFinalCoupledState(const FShadowAudioTarget& State, FString& OutError)
	{
		auto ValidateGroup = [&](const TArray<FString>& Keys,
			const TCHAR* Label,
			TFunctionRef<bool(const TArray<double>&)> Predicate)
		{
			bool bTouched = false;
			bool bAllKnown = true;
			TArray<double> Values;
			Values.Reserve(Keys.Num());
			for (const FString& Key : Keys)
			{
				bTouched |= State.TouchedNumericConfiguration.Contains(Key);
				const double* Value = State.NumericConfiguration.Find(Key);
				const bool bKnown = Value && State.KnownNumericConfiguration.Contains(Key)
					&& FMath::IsFinite(*Value);
				bAllKnown &= bKnown;
				Values.Add(bKnown ? *Value : 0.0);
			}
			if (!bAllKnown)
			{
				OutError = FString::Printf(TEXT("Coupled state '%s' has no exact complete persisted/final tuple%s."),
					Label, bTouched ? TEXT(" after a coupled field was touched") : TEXT(""));
				return false;
			}
			if (!Predicate(Values))
			{
				OutError = FString::Printf(TEXT("Final coupled state '%s' violates its typed invariant."), Label);
				return false;
			}
			return true;
		};

		if (State.ClassKind == TEXT("modulation.parameter")
			&& !ValidateGroup({TEXT("min_value"), TEXT("default_value"), TEXT("max_value")},
				TEXT("modulation.min_default_max"), [](const TArray<double>& V)
				{
					return V[0] <= V[1] && V[1] <= V[2];
				})) return false;
		if (State.ClassKind == TEXT("audio_widget")
			&& !ValidateGroup({TEXT("min_value"), TEXT("max_value")},
				TEXT("widget.min_max"), [](const TArray<double>& V) { return V[0] <= V[1]; }))
			return false;
		if (State.ClassKind == TEXT("motor_sim"))
		{
			if (!ValidateGroup({TEXT("idle_rpm"), TEXT("max_rpm")},
				TEXT("motor.idle_max_rpm"), [](const TArray<double>& V) { return V[0] <= V[1]; }))
				return false;
			if (!ValidateGroup({TEXT("gear_count"), TEXT("gear_ratio")},
				TEXT("motor.gear_count_ratio"), [](const TArray<double>& V)
				{
					return V[0] <= 1.0 || !FMath::IsNearlyZero(V[1]);
				})) return false;
		}
		if (State.ClassKind == TEXT("sound_mix")
			&& !ValidateGroup({TEXT("duration"), TEXT("fade_in_time"), TEXT("fade_out_time")},
				TEXT("sound_mix.duration_fades"), [](const TArray<double>& V)
				{
					return V[0] < 0.0 || V[1] + V[2] <= V[0];
				})) return false;
		if (State.ClassKind == TEXT("sound_attenuation")
			&& !ValidateGroup({TEXT("inner_radius"), TEXT("falloff_distance")},
				TEXT("attenuation.radius_falloff"), [](const TArray<double>& V)
				{
					return V[0] + V[1] <= static_cast<double>(WORLD_MAX);
				})) return false;
		if (State.ClassKind == TEXT("synesthesia.nrt")
			&& !ValidateGroup({TEXT("start_time"), TEXT("duration")},
				TEXT("synesthesia.start_duration"), [](const TArray<double>& V)
				{
					return V[0] + V[1] <= 86400.0;
				})) return false;

		for (const TPair<FGuid, FShadowMetaNodeDescriptor>& Pair : State.MetaNodeDescriptors)
		{
			if (Pair.Value.NodeKind != TEXT("random")) continue;
			const FString Prefix = TEXT("node:") + Pair.Key.ToString(EGuidFormats::Digits) + TEXT(":");
			if (!ValidateGroup({Prefix + TEXT("min_value"), Prefix + TEXT("max_value")},
				TEXT("metasound.random.min_max"), [](const TArray<double>& V) { return V[0] <= V[1]; }))
				return false;
		}
		return true;
	}

	FString ShadowCanonical(const FShadowAudioTarget& State)
	{
		FString Canonical;
		AppendToken(Canonical, TEXT("hyperai.audio-shadow.v3"));
		AppendToken(Canonical, State.ObjectPath);
		AppendToken(Canonical, State.Family);
		AppendToken(Canonical, State.ClassPath);
		AppendToken(Canonical, State.ClassKind);
		AppendBool(Canonical, State.bCreated);
		AppendBool(Canonical, State.bOrderedStateKnown);
		AppendBool(Canonical, State.bReferenceStateKnown);
		TArray<FString> Tokens;
		for (const TPair<FString, FString>& Pair : State.Configuration)
			Tokens.Add(TEXT("config:") + Pair.Key + TEXT("=") + Pair.Value);
		for (const TPair<FString, double>& Pair : State.NumericConfiguration)
			Tokens.Add(TEXT("numeric:") + Pair.Key + TEXT("=")
				+ FString::Printf(TEXT("%.17g"), Pair.Value));
		for (const FString& Key : State.KnownNumericConfiguration)
			Tokens.Add(TEXT("numeric_known:") + Key);
		for (const FString& Key : State.TouchedNumericConfiguration)
			Tokens.Add(TEXT("numeric_touched:") + Key);
		for (const TPair<FString, FString>& Pair : State.References)
			Tokens.Add(TEXT("reference:") + Pair.Key + TEXT("=") + Pair.Value);
		for (const TPair<FString, FString>& Pair : State.MetaMembers)
			Tokens.Add(TEXT("member:") + Pair.Key + TEXT("=") + Pair.Value);
		for (const FString& Interface : State.MetaInterfaces) Tokens.Add(TEXT("interface:") + Interface);
		for (const FGuid& Node : State.MetaNodes) Tokens.Add(TEXT("node:") + Node.ToString(EGuidFormats::Digits));
			for (const TPair<FGuid, FShadowMetaNodeDescriptor>& Pair : State.MetaNodeDescriptors)
				Tokens.Add(TEXT("descriptor:") + Pair.Key.ToString(EGuidFormats::Digits) + TEXT("=")
					+ Pair.Value.NodeKind + TEXT(":") + Pair.Value.DynamicDataType + TEXT(":")
					+ FString::FromInt(Pair.Value.Version) + TEXT(":") + Pair.Value.RegistryKey
					+ TEXT(":") + Pair.Value.InterfaceFingerprint);
		for (const TPair<FString, FShadowMetaPort>& Pair : State.MetaInputs)
			Tokens.Add(TEXT("input:") + Pair.Key + TEXT("=")
				+ Pair.Value.Name + TEXT(":")
				+ Pair.Value.TypeName.ToString() + TEXT(":")
				+ FString::FromInt(static_cast<int32>(Pair.Value.Access)) + TEXT(":")
				+ (Pair.Value.bLiteralSettable ? TEXT("literal") : TEXT("no_literal")) + TEXT(":")
				+ Pair.Value.ConfigureParameter);
		for (const TPair<FString, FShadowMetaPort>& Pair : State.MetaOutputs)
			Tokens.Add(TEXT("output:") + Pair.Key + TEXT("=")
				+ Pair.Value.Name + TEXT(":")
				+ Pair.Value.TypeName.ToString() + TEXT(":")
				+ FString::FromInt(static_cast<int32>(Pair.Value.Access)));
		for (const TPair<FString, FString>& Pair : State.MetaLiterals)
			Tokens.Add(TEXT("literal:") + Pair.Key + TEXT("=") + Pair.Value);
		for (const FString& Edge : State.MetaEdges) Tokens.Add(TEXT("edge:") + Edge);
		for (const FString& Input : State.MetaConnectedInputs)
			Tokens.Add(TEXT("connected_input:") + Input);
		Tokens.Sort();
		FString StateHash = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
		auto Fold = [&](const FString& Token)
		{
			FString Step;
			AppendToken(Step, StateHash);
			AppendToken(Step, Token);
			StateHash = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Step);
		};
		for (const FString& Token : Tokens) Fold(Token);
		for (const FString& Ordered : State.OrderedItems) Fold(TEXT("ordered:") + Ordered);
		return StateHash;
	}

	struct FTargetExistenceEvidence
	{
		bool bRegistryAvailable = false;
		UE::AssetRegistry::EExists State = UE::AssetRegistry::EExists::Unknown;
	};

	bool IsCreateAbsenceProven(const FTargetExistenceEvidence& Evidence)
	{
		return Evidence.bRegistryAvailable
			&& Evidence.State == UE::AssetRegistry::EExists::DoesNotExist;
	}

	FTargetExistenceEvidence QueryTargetExistenceOnDisk(const FString& Path)
	{
		FTargetExistenceEvidence Evidence;
		FAssetRegistryModule* Module = FModuleManager::GetModulePtr<FAssetRegistryModule>(TEXT("AssetRegistry"));
		if (!Module || !FPackageName::IsValidObjectPath(Path)) return Evidence;
		const FString PackageName = FSoftObjectPath(Path).GetLongPackageName();
		if (PackageName.IsEmpty() || FindPackage(nullptr, *PackageName)) return Evidence;
		IAssetRegistry& Registry = Module->Get();
		if (Registry.IsGathering()) return Evidence;
		Evidence.bRegistryAvailable = true;
		FAssetPackageData PackageData;
		Evidence.State = Registry.TryGetAssetPackageData(
			FName(*PackageName), PackageData, /*bFailIfLockHeld=*/true);
		return Evidence;
	}

	bool IsUsableSavedHash(const FString& SavedHash)
	{
		if (SavedHash.Len() < 40 || SavedHash.Len() > 64) return false;
		bool bAnyNonZero = false;
		for (const TCHAR Character : SavedHash)
		{
			if (!FChar::IsHexDigit(Character)) return false;
			bAnyNonZero |= Character != TEXT('0');
		}
		return bAnyNonZero;
	}

	FString ComputeOnDiskEvidenceRevision(
		const FString& ObjectPath,
		const FString& PackageName,
		const FString& AssetClassPath,
		const int64 DiskSize,
		const FString& SavedHash,
		const bool bHasPackageData)
	{
		if (!bHasPackageData || DiskSize <= 0 || !IsSafeObjectPath(ObjectPath)
			|| PackageName.IsEmpty() || AssetClassPath.IsEmpty()
			|| !IsUsableSavedHash(SavedHash)) return FString();
		FString Canonical;
		AppendToken(Canonical, TEXT("hyperai.audio-reference-metadata.v2"));
		AppendToken(Canonical, ObjectPath);
		AppendToken(Canonical, PackageName);
		AppendToken(Canonical, AssetClassPath);
		AppendInt(Canonical, DiskSize);
		AppendToken(Canonical, SavedHash.ToLower());
		return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
	}

	FString LoadedVariant(const FString& Path)
	{
		if (const UObject* Loaded = FindObject<UObject>(nullptr, *Path))
			return VariantFromObject(Loaded);
		return FString();
	}

	bool LoadedClassEvidence(const FString& Path, FAudioClassEvidence& Out)
	{
		if (const UObject* Loaded = FindObject<UObject>(nullptr, *Path))
		{
			Out = EvidenceFromObject(Loaded);
			return Out.Exact.IsValid();
		}
		return false;
	}

	bool IsObjectParameterCompatible(const FString& Name, const FAudioClassEvidence& E)
	{
		if (Name == TEXT("attenuation")) return E.Is(TEXT("/Script/Engine"), TEXT("SoundAttenuation"));
		if (Name == TEXT("concurrency")) return E.Is(TEXT("/Script/Engine"), TEXT("SoundConcurrency"));
		if (Name == TEXT("parent")) return E.Is(TEXT("/Script/Engine"), TEXT("SoundClass"));
		if (Name == TEXT("default_submix") || Name == TEXT("submix"))
			return E.Is(TEXT("/Script/Engine"), TEXT("SoundSubmixBase"));
		if (Name == TEXT("reverb")) return E.Is(TEXT("/Script/Engine"), TEXT("ReverbEffect"));
		if (Name == TEXT("bus")) return E.Is(TEXT("/Script/AudioModulation"), TEXT("SoundControlBus"));
		if (Name == TEXT("parameter"))
			return E.Is(TEXT("/Script/AudioModulation"), TEXT("SoundModulationParameter"));
		if (Name == TEXT("palette"))
			return E.Is(TEXT("/Script/Soundscape"), TEXT("SoundscapePalette"));
		if (Name == TEXT("sound") || Name == TEXT("source"))
		{
			return E.Is(TEXT("/Script/Engine"), TEXT("SoundBase"))
				|| E.Is(TEXT("/Script/MetasoundEngine"), TEXT("MetaSoundSource"));
		}
		if (Name == TEXT("value"))
		{
			// The closed object-literal surface is intentionally limited to audio
			// proxy assets; arbitrary UObject/class construction remains impossible.
			return E.Is(TEXT("/Script/Engine"), TEXT("SoundWave"))
				|| E.Is(TEXT("/Script/Engine"), TEXT("SoundSourceBus"))
				|| E.Is(TEXT("/Script/MetasoundEngine"), TEXT("MetaSoundSource"));
		}
		return false;
	}

	bool IsReferenceVariantCompatible(const FString& OperationVariant, const FString& Observed)
	{
		if (OperationVariant == TEXT("sound_cue.set_root_wave")
			|| OperationVariant == TEXT("synesthesia.nrt.set_sound"))
			return Observed == TEXT("sound_wave");
		if (OperationVariant == TEXT("sound_class.set_parent")
			|| OperationVariant == TEXT("sound_mix.add_adjuster")
			|| OperationVariant == TEXT("sound_mix.remove_adjuster"))
			return Observed == TEXT("sound_class");
		if (OperationVariant == TEXT("dialogue_wave.add_context")
			|| OperationVariant == TEXT("dialogue_wave.remove_context"))
			return Observed == TEXT("dialogue_voice");
		if (OperationVariant == TEXT("modulation.mix.add_stage"))
			return Observed == TEXT("audio_modulation");
		return false;
	}

	bool IsSourceVariantCompatible(const FString& OperationVariant, const FString& Observed)
	{
		if (OperationVariant.StartsWith(TEXT("sound_cue_template."))
			|| OperationVariant.StartsWith(TEXT("sound_utility.sound_simple.")))
		{
			return Observed == TEXT("sound_wave") || Observed == TEXT("sound_cue")
				|| Observed == TEXT("metasound_source");
		}
		if (OperationVariant.StartsWith(TEXT("soundscape.palette.")))
			return Observed == TEXT("soundscape");
		return false;
	}

	bool IsObjectCompatible(const FString& Variant, const UObject* Object)
	{
		if (!IsValid(Object)) return false;
		const FString Observed = VariantFromObject(Object);
		if (Variant.StartsWith(TEXT("metasound."))) return Observed.StartsWith(TEXT("metasound_"));
		if (Variant.StartsWith(TEXT("sound_wave."))) return Observed == TEXT("sound_wave");
		if (Variant.StartsWith(TEXT("sound_cue_template."))) return Observed == TEXT("sound_cue_template");
		if (Variant.StartsWith(TEXT("sound_cue."))) return Observed == TEXT("sound_cue");
		if (Variant.StartsWith(TEXT("sound_class."))) return Observed == TEXT("sound_class");
		if (Variant.StartsWith(TEXT("sound_mix."))) return Observed == TEXT("sound_mix");
		if (Variant.StartsWith(TEXT("sound_attenuation."))) return Observed == TEXT("sound_attenuation");
		if (Variant.StartsWith(TEXT("sound_concurrency."))) return Observed == TEXT("sound_concurrency");
		if (Variant.StartsWith(TEXT("dialogue_wave."))) return Observed == TEXT("dialogue_wave");
		if (Variant.StartsWith(TEXT("dialogue_voice."))) return Observed == TEXT("dialogue_voice");
		if (Variant.StartsWith(TEXT("motor_sim."))) return Observed == TEXT("motor_sim");
		const FString Family = OptionalFamilyForVariant(Variant);
		return !Family.IsEmpty() && Observed == Family;
	}

	bool ValidateTargetCas(
		const FString& Variant,
		const FString& TargetPath,
		const FString& ExpectedRevision,
		FString& OutCurrentRevision,
		FString& OutError,
		const double AbsoluteDeadline = 0.0)
	{
		OutCurrentRevision.Reset();
		if (!IsSafeObjectPath(TargetPath))
		{
			OutError = TEXT("TargetPath must be one exact bounded /Game object path.");
			return false;
		}
		UObject* Loaded = FindObject<UObject>(nullptr, *TargetPath);
		if (IsCreateVariant(Variant))
		{
			if (!ExpectedRevision.IsEmpty())
			{
				OutError = TEXT("Create variants prohibit ExpectedRevision.");
				return false;
			}
			if (Loaded)
			{
				OutError = TEXT("Create target already exists in loaded state.");
				return false;
			}
			const FTargetExistenceEvidence Existence = QueryTargetExistenceOnDisk(TargetPath);
			if (!IsCreateAbsenceProven(Existence))
			{
				OutError = TEXT("Create target absence requires a nonblocking DoesNotExist result for the entire target package and no loaded containing package.");
				return false;
			}
			return true;
		}
		if (!Loaded)
		{
			OutError = TEXT("Existing audio mutation targets must already be loaded for complete CAS; no load was attempted.");
			return false;
		}
		if (!IsObjectCompatible(Variant, Loaded))
		{
			OutError = TEXT("Loaded target class does not match the closed audio operation family.");
			return false;
		}
		OutCurrentRevision = ComputeLoadedRevision(
			Loaded, nullptr, true, AbsoluteDeadline);
		if (!IsSha256(ExpectedRevision) || ExpectedRevision != OutCurrentRevision)
		{
			OutError = TEXT("ExpectedRevision does not match the exact loaded audio target revision.");
			return false;
		}
		return true;
	}

	bool BuildPreparedArtifact(
		const FHyperAIAudioApplyPlanRequest& Request,
		const TArray<FHyperAIAudioPrerequisite>& Prerequisites,
		const FString& EffectTarget,
		const TSharedRef<const FHyperAIAudioTypedArtifactPayload, ESPMode::ThreadSafe>& Payload,
		FHyperAIStudioPreparedTypedArtifact& OutPrepared,
		FString& OutError)
	{
		FHyperAIStudioTypedArtifactContract Contract;
		Contract.Binding.PackId = FHyperAIStudioAudioContracts::PackId;
		Contract.Binding.ToolName = TEXT("hyper_audio_apply_plan");
		Contract.Binding.VariantId = TEXT("apply_external_effect");
		Contract.Binding.ExpectedSafety = EHyperAIStudioDomainSafety::ExternalEffect;
		Contract.Binding.CanonicalProjectId = FHyperAIStudioExtensionRuntime::GetCanonicalProjectId();
		Contract.Binding.ExpectedAdapterFingerprint = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
			TEXT("hyperai.audio.source-adapter.v1|audio_metasound"));
		Contract.Binding.ExpectedAdapterGeneration = 1;
		Contract.Binding.ExpectedRegistryEpoch = 1;
		Contract.Binding.Prerequisites.PackId = FHyperAIStudioAudioContracts::PackId;
		Contract.Binding.Prerequisites.bPackEnabled = true;
		Contract.Binding.Prerequisites.Revision = 1;
		for (const FHyperAIAudioPrerequisite& Prerequisite : Prerequisites)
		{
			Contract.Binding.Prerequisites.Observations.Add({Prerequisite.Family,
				Prerequisite.State == TEXT("available")
					? EHyperAIStudioDomainPrerequisiteState::Available
					: Prerequisite.State == TEXT("disabled")
						? EHyperAIStudioDomainPrerequisiteState::Disabled
						: EHyperAIStudioDomainPrerequisiteState::Missing});
		}
		Contract.Binding.Prerequisites.Fingerprint =
			FHyperAIStudioDomainAdapterRegistry::ComputePrerequisiteFingerprint(
				Contract.Binding.Prerequisites);
		Contract.Binding.Admission.PackId = FHyperAIStudioAudioContracts::PackId;
		Contract.Binding.Admission.bPackAdmitted = false;
		Contract.Binding.Admission.Revision = 1;
		Contract.Binding.Admission.Fingerprint =
			FHyperAIStudioDomainAdapterRegistry::ComputeAdmissionFingerprint(
				Contract.Binding.Admission);
		Contract.ArtifactTypeId = Payload->GetTypeId();
		Contract.ArtifactSchemaFingerprint = Payload->GetSchemaFingerprint();
		Contract.ArtifactSemanticFingerprint = Payload->GetSemanticFingerprint();
		Contract.EffectTarget = EffectTarget;
		Contract.DeadlineMs = Request.DeadlineMs;
		Contract.MaxNativeOperations = FHyperAIStudioAudioContracts::MaxOperations;
		Contract.MaxGameThreadMs = 200;
		Contract.MaxOutputBytes = 128 * 1024;
		Contract.MaxResultBytes = 256;
		Contract.StageLifetimeMs = 15000;
		Contract.bCompileOnce = true;
		Contract.bSaveOnce = true;
		Contract.bValidateOnce = true;
		Contract.bVerifyFreshOnce = true;
		return FHyperAIStudioTypedArtifactExecutor::Prepare(Contract, OutPrepared, OutError);
	}
}

int32 FHyperAIAudioTypedArtifactPayload::GetBoundedByteSize() const
{
	int64 Characters = TypeId.Len() + SchemaFingerprint.Len() + PackId.Len()
		+ BaseRevision.Len() + 64;
	for (const FString& Operation : CanonicalOperations) Characters += Operation.Len() + 16;
	for (const FString& Shadow : CanonicalShadowStates) Characters += Shadow.Len() + 16;
	return Characters > MAX_int32 / static_cast<int32>(sizeof(TCHAR))
		? MAX_int32 : static_cast<int32>(Characters * sizeof(TCHAR));
}

FString FHyperAIAudioTypedArtifactPayload::GetSemanticFingerprint() const
{
	using namespace HyperAIStudio::Audio::Private;
	FString Canonical;
	AppendToken(Canonical, TEXT("hyperai.audio-typed-artifact.v1"));
	AppendToken(Canonical, PackId);
	AppendToken(Canonical, TEXT("external_effect"));
	AppendToken(Canonical, BaseRevision);
	for (const FString& Operation : CanonicalOperations) AppendToken(Canonical, Operation);
	for (const FString& Shadow : CanonicalShadowStates) AppendToken(Canonical, Shadow);
	return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
}

TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe>
FHyperAIAudioTypedArtifactPayload::CloneImmutable() const
{
	TSharedRef<FHyperAIAudioTypedArtifactPayload, ESPMode::ThreadSafe> Clone =
		MakeShared<FHyperAIAudioTypedArtifactPayload, ESPMode::ThreadSafe>();
	Clone->TypeId = TypeId;
	Clone->SchemaFingerprint = SchemaFingerprint;
	Clone->PackId = PackId;
	Clone->BaseRevision = BaseRevision;
	Clone->CanonicalOperations = CanonicalOperations;
	Clone->CanonicalShadowStates = CanonicalShadowStates;
	return StaticCastSharedRef<const IHyperAIStudioTypedArtifactPayload>(Clone);
}

FString FHyperAIStudioAudioContracts::GetQualifiedToolsetName()
{
	return TEXT("HyperAIStudioAudio.HyperAIStudioAudioToolset");
}

const TArray<FHyperAIAudioManifestEntry>& FHyperAIStudioAudioContracts::GetManifest()
{
	static const TArray<FHyperAIAudioManifestEntry> Manifest = {
		{TEXT("hyper_audio_inspect"), GetQualifiedToolsetName()},
		{TEXT("hyper_audio_apply_plan"), GetQualifiedToolsetName()},
		{TEXT("hyper_audio_validate"), GetQualifiedToolsetName()}};
	return Manifest;
}

bool FHyperAIStudioAudioContracts::IsRegistrationAllowed(const bool bAllowSourceCandidateForDev)
{
	const TArray<FHyperAIAudioManifestEntry>& Manifest = GetManifest();
	if (Manifest.Num() != 3) return false;
	TSet<FString> Unique;
	TArray<FString> Names;
	for (const FHyperAIAudioManifestEntry& Entry : Manifest)
	{
		if (Entry.Name.IsEmpty() || Entry.QualifiedToolset != GetQualifiedToolsetName()
			|| Unique.Contains(Entry.Name)) return false;
		Unique.Add(Entry.Name);
		Names.Add(Entry.Name);
	}
	return FHyperAIStudioExtensionRuntime::IsExactGeneratedCohortRegistrationAllowed(
		PackId, AtomicCohortId, Names, bAllowSourceCandidateForDev);
}

bool FHyperAIStudioAudioContracts::IsInspectVariant(const FString& Variant)
{
	static const TSet<FString> Variants = {
		TEXT("sound_wave"), TEXT("sound_cue"), TEXT("sound_class"), TEXT("sound_mix"),
		TEXT("sound_attenuation"), TEXT("sound_concurrency"), TEXT("dialogue_wave"),
		TEXT("dialogue_voice"), TEXT("metasound_source"), TEXT("metasound_patch"),
		TEXT("audio_modulation"), TEXT("wave_table"), TEXT("synthesis_effect"),
		TEXT("synesthesia_nrt"), TEXT("soundscape"), TEXT("sound_utility"),
		TEXT("sound_cue_template"), TEXT("audio_gameplay_volume"), TEXT("audio_widget"),
		TEXT("audio_capture"), TEXT("motor_sim")};
	return Variants.Contains(Variant);
}

bool FHyperAIStudioAudioContracts::IsCanonicalProjectAssetPath(const FString& Path)
{
	using namespace HyperAIStudio::Audio::Private;
	if (!IsSafeObjectPath(Path) || !FPackageName::IsValidObjectPath(Path)) return false;
	const FSoftObjectPath Reference(Path);
	const FString PackageName = Reference.GetLongPackageName();
	const FString AssetName = Reference.GetAssetName();
	return Reference.IsValid() && FPackageName::IsValidLongPackageName(PackageName)
		&& !AssetName.IsEmpty() && Path == PackageName + TEXT(".") + AssetName;
}

FString FHyperAIStudioAudioContracts::ComputeLoadedTargetRevision(const FString& ExactObjectPath)
{
	using namespace HyperAIStudio::Audio::Private;
	if (!IsSafeObjectPath(ExactObjectPath)) return FString();
	return ComputeLoadedRevision(FindObject<UObject>(nullptr, *ExactObjectPath));
}

#if WITH_DEV_AUTOMATION_TESTS
FString FHyperAIStudioAudioContracts::ComputeOnDiskEvidenceRevisionForTest(
	const FString& ObjectPath,
	const FString& PackageName,
	const FString& ClassPathValue,
	const int64 DiskSize,
	const FString& SavedHash,
	const bool bHasPackageData)
{
	return HyperAIStudio::Audio::Private::ComputeOnDiskEvidenceRevision(
		ObjectPath, PackageName, ClassPathValue, DiskSize, SavedHash, bHasPackageData);
}

FString FHyperAIStudioAudioContracts::ClassifyClassEvidenceForTest(
	const FString& ExactClassPath,
	const TArray<FString>& AncestorClassPaths)
{
	using namespace HyperAIStudio::Audio::Private;
	FAudioClassEvidence Evidence;
	auto Parse = [](const FString& Value) -> FTopLevelAssetPath
	{
		return FTopLevelAssetPath(FName(*FPackageName::ObjectPathToPackageName(Value)),
			FName(*FPackageName::ObjectPathToObjectName(Value)));
	};
	Evidence.Exact = Parse(ExactClassPath);
	for (const FString& Ancestor : AncestorClassPaths) Evidence.Ancestors.Add(Parse(Ancestor));
	return VariantFromClassEvidence(Evidence);
}

bool FHyperAIStudioAudioContracts::IsClassEvidenceCompatibleForTest(
	const FString& ParameterName,
	const FString& ExactClassPath,
	const TArray<FString>& AncestorClassPaths)
{
	using namespace HyperAIStudio::Audio::Private;
	FAudioClassEvidence Evidence;
	auto Parse = [](const FString& Value) -> FTopLevelAssetPath
	{
		return FTopLevelAssetPath(FName(*FPackageName::ObjectPathToPackageName(Value)),
			FName(*FPackageName::ObjectPathToObjectName(Value)));
	};
	Evidence.Exact = Parse(ExactClassPath);
	for (const FString& Ancestor : AncestorClassPaths) Evidence.Ancestors.Add(Parse(Ancestor));
	return IsObjectParameterCompatible(ParameterName, Evidence);
}

bool FHyperAIStudioAudioContracts::ValidateMetaSoundDocumentForTest(
	const FMetasoundFrontendDocument& Document,
	FString& OutRevision,
	bool& bOutTraversalComplete)
{
	using namespace HyperAIStudio::Audio::Private;
	FHyperAIAudioRecord Record;
	FString Canonical;
	bool bValid = false;
	bOutTraversalComplete = true;
	const bool bRecognized = InspectMetaSound(nullptr, Record, nullptr, Canonical,
		&bOutTraversalComplete, 0.0, &bValid, &Document);
	OutRevision = bOutTraversalComplete
		? FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical) : FString();
	return bRecognized && bOutTraversalComplete && bValid && IsSha256(OutRevision);
}

bool FHyperAIStudioAudioContracts::IsCreateExistenceStateAdmittedForTest(
	const int32 ExistsState,
	const bool bRegistryAvailable)
{
	using namespace HyperAIStudio::Audio::Private;
	FTargetExistenceEvidence Evidence;
	Evidence.bRegistryAvailable = bRegistryAvailable;
	if (ExistsState < static_cast<int32>(UE::AssetRegistry::EExists::DoesNotExist)
		|| ExistsState > static_cast<int32>(UE::AssetRegistry::EExists::Unknown)) return false;
	Evidence.State = static_cast<UE::AssetRegistry::EExists>(ExistsState);
	return IsCreateAbsenceProven(Evidence);
}

bool FHyperAIStudioAudioContracts::IsSparseContainerShapeBoundedForTest(
	const int32 Num,
	const int32 MaxIndex)
{
	return HyperAIStudio::Audio::Private::IsSparseContainerShapeBounded(Num, MaxIndex);
}

bool FHyperAIStudioAudioContracts::ValidateCoupledTupleForTest(
	const FString& ClassKind,
	const TMap<FString, double>& KnownValues,
	const TSet<FString>& TouchedKeys,
	FString& OutError)
{
	HyperAIStudio::Audio::Private::FShadowAudioTarget State;
	State.ClassKind = ClassKind;
	State.NumericConfiguration = KnownValues;
	for (const TPair<FString, double>& Pair : KnownValues)
		State.KnownNumericConfiguration.Add(Pair.Key);
	State.TouchedNumericConfiguration = TouchedKeys;
	return HyperAIStudio::Audio::Private::ValidateFinalCoupledState(State, OutError);
}
#endif

bool FHyperAIStudioAudioContracts::ValidateOperationShape(
	const FHyperAIAudioPlanOperation& In,
	FHyperAIAudioBackendOperation& Out,
	FString& OutError)
{
	using namespace HyperAIStudio::Audio::Private;
	Out = {};
	OutError.Reset();
	static const TSet<FString> Variants = {
		TEXT("sound_wave.configure"),
		TEXT("sound_cue.create"), TEXT("sound_cue.configure"), TEXT("sound_cue.set_root_wave"),
		TEXT("sound_class.create"), TEXT("sound_class.configure"), TEXT("sound_class.set_parent"),
		TEXT("sound_mix.create"), TEXT("sound_mix.configure"),
		TEXT("sound_mix.add_adjuster"), TEXT("sound_mix.remove_adjuster"),
		TEXT("sound_attenuation.create"), TEXT("sound_attenuation.configure"),
		TEXT("sound_concurrency.create"), TEXT("sound_concurrency.configure"),
		TEXT("dialogue_voice.create"), TEXT("dialogue_voice.configure"),
		TEXT("dialogue_wave.create"), TEXT("dialogue_wave.configure"),
		TEXT("dialogue_wave.add_context"), TEXT("dialogue_wave.remove_context"),
		TEXT("metasound.source.create"), TEXT("metasound.patch.create"),
		TEXT("metasound.add_input"), TEXT("metasound.add_output"),
		TEXT("metasound.add_variable"), TEXT("metasound.remove_member"),
		TEXT("metasound.add_node"), TEXT("metasound.configure_node"), TEXT("metasound.remove_node"),
		TEXT("metasound.connect"), TEXT("metasound.disconnect"),
		TEXT("metasound.set_literal"), TEXT("metasound.declare_interface"),
		TEXT("modulation.parameter.create"), TEXT("modulation.parameter.configure"),
		TEXT("modulation.bus.create"), TEXT("modulation.bus.configure"),
		TEXT("modulation.mix.create"), TEXT("modulation.mix.configure"),
		TEXT("modulation.mix.add_stage"), TEXT("modulation.mix.remove_stage"),
		TEXT("modulation.patch.create"), TEXT("modulation.patch.configure"),
		TEXT("modulation.generator.create"), TEXT("modulation.generator.configure"),
		TEXT("wavetable.bank.create"), TEXT("wavetable.bank.configure"),
		TEXT("synthesis.preset.create"), TEXT("synthesis.preset.configure"),
		TEXT("synthesis.effect_chain.create"), TEXT("synthesis.effect_chain.configure"),
		TEXT("synthesis.effect_chain.add_effect"), TEXT("synthesis.effect_chain.remove_effect"),
		TEXT("synesthesia.nrt.create"), TEXT("synesthesia.nrt.configure"),
		TEXT("synesthesia.nrt.set_sound"),
		TEXT("soundscape.palette.create"), TEXT("soundscape.palette.configure"),
		TEXT("soundscape.color.create"), TEXT("soundscape.color.configure"),
		TEXT("sound_utility.sound_simple.create"), TEXT("sound_utility.sound_simple.configure"),
		TEXT("sound_cue_template.create"), TEXT("sound_cue_template.configure"),
		TEXT("audio_gameplay_volume.create"), TEXT("audio_gameplay_volume.configure"),
		TEXT("audio_gameplay_volume.add_mutator"), TEXT("audio_gameplay_volume.remove_mutator"),
		TEXT("audio_widget.configure"), TEXT("audio_capture.configure_component"),
		TEXT("motor_sim.configure_model"), TEXT("motor_sim.add_component"),
		TEXT("motor_sim.remove_component")};
	if (!Variants.Contains(In.Variant))
	{
		OutError = TEXT("Operation is not in the closed audio schema; playback, mic capture, raw DSP, ")
			TEXT("arbitrary nodes/classes, and ordinary asset CRUD are delegated or prohibited.");
		return false;
	}
	if (!IsSafeObjectPath(In.TargetPath) || In.SourcePaths.Num() > MaxPaths
		|| In.Parameters.Num() > MaxParametersPerOperation
		|| !IsFiniteVector(In.Location) || !IsFiniteVector(In.Extent)
		|| !FMath::IsFinite(In.Value) || !FMath::IsFinite(In.SecondaryValue)
		|| FMath::Abs(In.Value) > 1000000000000.0
		|| FMath::Abs(In.SecondaryValue) > 1000000000000.0
		|| In.Count < 0 || In.Count > 1000000 || In.Index < -1 || In.Index > 1000000)
	{
		OutError = TEXT("Operation paths, geometry, scalars, indexes, or collections exceed hard bounds.");
		return false;
	}
	if ((!In.ReferencePath.IsEmpty() && !IsCanonicalProjectAssetPath(In.ReferencePath))
		|| (!In.MemberName.IsEmpty() && !IsSafeName(In.MemberName))
		|| (!In.SecondaryName.IsEmpty() && !IsSafeName(In.SecondaryName))
		|| (!In.DataType.IsEmpty() && !IsSafeName(In.DataType))
		|| (!In.NodeKind.IsEmpty() && !IsSafeName(In.NodeKind))
		|| (!In.VertexName.IsEmpty() && !IsSafeName(In.VertexName))
		|| (!In.FromPortName.IsEmpty() && !IsSafeName(In.FromPortName))
		|| (!In.ToPortName.IsEmpty() && !IsSafeName(In.ToPortName)))
	{
		OutError = TEXT("Reference and symbolic fields must use canonical project assets and bounded identifiers.");
		return false;
	}
	TSet<FString> UniqueSources;
	for (const FString& Source : In.SourcePaths)
	{
		if (!IsCanonicalProjectAssetPath(Source) || UniqueSources.Contains(Source))
		{
			OutError = TEXT("SourcePaths must contain unique canonical /Game asset paths.");
			return false;
		}
		UniqueSources.Add(Source);
	}
	TSet<FString> UniqueParameters;
	for (const FHyperAIAudioParameterValue& Parameter : In.Parameters)
	{
		if (UniqueParameters.Contains(Parameter.Name) || !ValidateParameter(Parameter, OutError))
		{
			if (OutError.IsEmpty()) OutError = TEXT("Parameter names must be unique per operation.");
			return false;
		}
		const FString ExpectedType = In.Variant == TEXT("metasound.configure_node")
			? ClosedMetaSoundNodeParameterType(In.NodeKind, Parameter.Name)
			: ClosedParameterType(In.Variant, Parameter.Name);
		if (ExpectedType.IsEmpty()
			|| (ExpectedType != TEXT("any") && ExpectedType != Parameter.Type))
		{
			OutError = TEXT("A parameter is not admitted by this operation's closed semantic schema or uses the wrong type.");
			return false;
		}
		if (!ValidateClosedParameterDomain(In.Variant, Parameter, OutError)) return false;
		UniqueParameters.Add(Parameter.Name);
	}
	const FHyperAIAudioParameterValue* MinParameter = In.Parameters.FindByPredicate(
		[](const FHyperAIAudioParameterValue& Parameter) { return Parameter.Name == TEXT("min_value"); });
	const FHyperAIAudioParameterValue* MaxParameter = In.Parameters.FindByPredicate(
		[](const FHyperAIAudioParameterValue& Parameter) { return Parameter.Name == TEXT("max_value"); });
	if (MinParameter && MaxParameter && MinParameter->FloatValue > MaxParameter->FloatValue)
	{
		OutError = TEXT("min_value must not exceed max_value in one typed operation.");
		return false;
	}
	if ((IsCreateVariant(In.Variant) && !In.ExpectedRevision.IsEmpty())
		|| (!In.ExpectedRevision.IsEmpty() && !IsSha256(In.ExpectedRevision)))
	{
		OutError = TEXT("Create operations prohibit CAS; edits accept only an empty same-plan-create CAS or canonical sha256 revision.");
		return false;
	}

	const bool bUsesReference = In.Variant == TEXT("sound_cue.set_root_wave")
		|| In.Variant == TEXT("sound_class.set_parent")
		|| In.Variant == TEXT("sound_mix.add_adjuster")
		|| In.Variant == TEXT("sound_mix.remove_adjuster")
		|| In.Variant == TEXT("dialogue_wave.add_context")
		|| In.Variant == TEXT("dialogue_wave.remove_context")
		|| In.Variant == TEXT("modulation.mix.add_stage")
		|| In.Variant == TEXT("synesthesia.nrt.set_sound");
	if (!bUsesReference && !In.ReferencePath.IsEmpty())
	{
		OutError = TEXT("ReferencePath is not part of this closed operation variant.");
		return false;
	}
	const bool bUsesSources = In.Variant.StartsWith(TEXT("sound_cue_template."))
		|| In.Variant.StartsWith(TEXT("sound_utility.sound_simple."))
		|| In.Variant.StartsWith(TEXT("soundscape.palette."));
	if (!bUsesSources && !In.SourcePaths.IsEmpty())
	{
		OutError = TEXT("SourcePaths are not part of this closed operation variant.");
		return false;
	}
	const bool bUsesMemberName = In.Variant == TEXT("metasound.add_input")
		|| In.Variant == TEXT("metasound.add_output")
		|| In.Variant == TEXT("metasound.add_variable")
		|| In.Variant == TEXT("metasound.remove_member")
		|| In.Variant == TEXT("metasound.declare_interface")
		|| In.Variant == TEXT("dialogue_wave.add_context")
		|| In.Variant == TEXT("dialogue_wave.remove_context");
	if (!bUsesMemberName && !In.MemberName.IsEmpty())
	{
		OutError = TEXT("MemberName is not part of this closed operation variant.");
		return false;
	}
	const bool bUsesSecondaryName = In.Variant == TEXT("dialogue_wave.add_context")
		|| In.Variant == TEXT("dialogue_wave.remove_context");
	if (!bUsesSecondaryName && !In.SecondaryName.IsEmpty())
	{
		OutError = TEXT("SecondaryName is not part of this closed operation variant.");
		return false;
	}
	const bool bUsesDataType = In.Variant == TEXT("metasound.add_input")
		|| In.Variant == TEXT("metasound.add_output")
		|| In.Variant == TEXT("metasound.add_variable")
		|| In.Variant == TEXT("metasound.remove_member")
		|| In.Variant == TEXT("metasound.add_node");
	if (!bUsesDataType && !In.DataType.IsEmpty())
	{
		OutError = TEXT("DataType is not part of this closed operation variant.");
		return false;
	}
	const bool bUsesNodeKind = In.Variant == TEXT("metasound.add_node")
		|| In.Variant == TEXT("metasound.configure_node")
		|| In.Variant == TEXT("synthesis.effect_chain.add_effect")
		|| In.Variant == TEXT("audio_gameplay_volume.add_mutator")
		|| In.Variant == TEXT("motor_sim.add_component");
	if (!bUsesNodeKind && !In.NodeKind.IsEmpty())
	{
		OutError = TEXT("NodeKind is not part of this closed semantic operation variant.");
		return false;
	}
	const bool bUsesIndex = In.Variant == TEXT("modulation.mix.add_stage")
		|| In.Variant == TEXT("modulation.mix.remove_stage")
		|| In.Variant == TEXT("synthesis.effect_chain.add_effect")
		|| In.Variant == TEXT("synthesis.effect_chain.remove_effect")
		|| In.Variant == TEXT("audio_gameplay_volume.add_mutator")
		|| In.Variant == TEXT("audio_gameplay_volume.remove_mutator")
		|| In.Variant == TEXT("motor_sim.add_component")
		|| In.Variant == TEXT("motor_sim.remove_component");
	if ((!bUsesIndex && In.Index != -1) || In.Count != 0)
	{
		OutError = TEXT("Index or Count is not part of this closed operation variant.");
		return false;
	}
	const bool bUsesScalars = In.Variant == TEXT("sound_mix.add_adjuster")
		|| In.Variant == TEXT("sound_mix.remove_adjuster")
		|| In.Variant == TEXT("modulation.mix.add_stage")
		|| In.Variant == TEXT("modulation.mix.remove_stage")
		|| In.Variant == TEXT("synthesis.effect_chain.add_effect")
		|| In.Variant == TEXT("audio_gameplay_volume.add_mutator")
		|| In.Variant == TEXT("motor_sim.add_component");
	if (!bUsesScalars && (In.Value != 0.0 || In.SecondaryValue != 0.0))
	{
		OutError = TEXT("Value scalars are not part of this closed operation variant.");
		return false;
	}
	if (bUsesScalars)
	{
		double MinValue = 0.0;
		double MaxValue = 1.0;
		double MinSecondary = 0.0;
		double MaxSecondary = 1.0;
		if (In.Variant.StartsWith(TEXT("sound_mix.")))
		{
			MaxValue = 4.0;
			MinSecondary = 0.0;
			MaxSecondary = 8.0;
		}
		else if (In.Variant.StartsWith(TEXT("motor_sim.")))
		{
			MinValue = MinSecondary = -200000.0;
			MaxValue = MaxSecondary = 200000.0;
		}
		else if (In.Variant.StartsWith(TEXT("audio_gameplay_volume.")))
		{
			MinSecondary = 0.0;
			MaxSecondary = 86400.0;
		}
		if (In.Value < MinValue || In.Value > MaxValue
			|| In.SecondaryValue < MinSecondary || In.SecondaryValue > MaxSecondary)
		{
			OutError = TEXT("Operation scalar is outside its variant-specific semantic domain.");
			return false;
		}
	}
	const bool bLifecycleConfigure = IsCreateVariant(In.Variant)
		|| In.Variant.EndsWith(TEXT(".configure"))
		|| In.Variant == TEXT("audio_capture.configure_component")
		|| In.Variant == TEXT("motor_sim.configure_model");
	if ((!bLifecycleConfigure && (In.bSetEnabled || In.bEnabled))
		|| (!In.bSetEnabled && In.bEnabled))
	{
		OutError = TEXT("Enabled-state fields are absent or ambiguous for this operation variant.");
		return false;
	}
	const bool bUsesGeometry = In.Variant == TEXT("audio_gameplay_volume.create");
	if (!bUsesGeometry && (In.Location != FVector::ZeroVector || In.Extent != FVector::ZeroVector))
	{
		OutError = TEXT("Location/Extent are reserved for typed Audio Gameplay Volume creation.");
		return false;
	}

	const bool bUsesNodeId = In.Variant == TEXT("metasound.add_node")
		|| In.Variant == TEXT("metasound.configure_node")
		|| In.Variant == TEXT("metasound.remove_node")
		|| In.Variant == TEXT("metasound.set_literal");
	const bool bUsesVertexId = In.Variant == TEXT("metasound.set_literal");
	const bool bUsesVertexName = In.Variant == TEXT("metasound.set_literal");
	const bool bUsesConnectionIds = In.Variant == TEXT("metasound.connect")
		|| In.Variant == TEXT("metasound.disconnect");
	if ((!bUsesNodeId && In.NodeId.IsValid()) || (!bUsesVertexId && In.VertexId.IsValid())
		|| (!bUsesVertexName && !In.VertexName.IsEmpty())
		|| (!bUsesConnectionIds && (In.FromNodeId.IsValid() || In.FromVertexId.IsValid()
			|| In.ToNodeId.IsValid() || In.ToVertexId.IsValid()
			|| !In.FromPortName.IsEmpty() || !In.ToPortName.IsEmpty())))
	{
		OutError = TEXT("Persisted MetaSound GUID fields are not part of this operation variant.");
		return false;
	}
	const bool bUsesDescriptorVersion = In.Variant == TEXT("metasound.add_node")
		|| In.Variant == TEXT("metasound.configure_node");
	if ((!bUsesDescriptorVersion && In.NodeDescriptorVersion != 0)
		|| (bUsesDescriptorVersion && In.NodeDescriptorVersion != 1))
	{
		OutError = TEXT("MetaSound typed node descriptors require exact schema version 1; other variants prohibit it.");
		return false;
	}

	static const TSet<FString> MetaDataTypes = {
		TEXT("bool"), TEXT("int32"), TEXT("float"), TEXT("string"), TEXT("trigger"),
		TEXT("audio"), TEXT("object"), TEXT("wave_asset"), TEXT("time")};
	static const TSet<FString> SynthesisEffectKinds = {
		TEXT("bit_crusher"), TEXT("chorus"), TEXT("delay"), TEXT("distortion"),
		TEXT("dynamics"), TEXT("filter"), TEXT("foldback"), TEXT("phaser"),
		TEXT("ring_modulation"), TEXT("stereo_delay"), TEXT("wave_shaper")};
	static const TSet<FString> VolumeMutatorKinds = {
		TEXT("reverb"), TEXT("submix_send"), TEXT("submix_override"), TEXT("filter"),
		TEXT("toggle")};
	static const TSet<FString> MotorComponentKinds = {
		TEXT("rpm_curve"), TEXT("throttle_state"), TEXT("velocity_sync"),
		TEXT("rev_limiter"), TEXT("resistance"), TEXT("boost"), TEXT("reverse")};

	if ((In.Variant == TEXT("sound_cue.set_root_wave")
		|| In.Variant == TEXT("sound_class.set_parent")
		|| In.Variant == TEXT("sound_mix.add_adjuster")
		|| In.Variant == TEXT("sound_mix.remove_adjuster")
		|| In.Variant == TEXT("dialogue_wave.add_context")
		|| In.Variant == TEXT("dialogue_wave.remove_context")
		|| In.Variant == TEXT("modulation.mix.add_stage")
		|| In.Variant == TEXT("synesthesia.nrt.set_sound"))
		&& !IsCanonicalProjectAssetPath(In.ReferencePath))
	{
		OutError = TEXT("This typed reference operation requires one canonical /Game asset path.");
		return false;
	}
	if ((In.Variant == TEXT("dialogue_wave.add_context")
		|| In.Variant == TEXT("dialogue_wave.remove_context"))
		&& (!IsSafeName(In.MemberName) || !IsSafeName(In.SecondaryName)))
	{
		OutError = TEXT("Dialogue contexts require bounded speaker and target identifiers plus one DialogueVoice reference.");
		return false;
	}
	if ((In.Variant == TEXT("modulation.mix.remove_stage")
		|| In.Variant == TEXT("synthesis.effect_chain.remove_effect")
		|| In.Variant == TEXT("audio_gameplay_volume.remove_mutator")
		|| In.Variant == TEXT("motor_sim.remove_component"))
		&& In.Index < 0)
	{
		OutError = TEXT("Removal from a persisted ordered audio collection requires a non-negative exact index.");
		return false;
	}
	if ((In.Variant == TEXT("metasound.add_input") || In.Variant == TEXT("metasound.add_output")
		|| In.Variant == TEXT("metasound.add_variable"))
		&& (!IsSafeName(In.MemberName) || !MetaDataTypes.Contains(In.DataType)))
	{
		OutError = TEXT("MetaSound members require a bounded name and one closed data type.");
		return false;
	}
	if (In.Variant == TEXT("metasound.remove_member")
		&& (!IsSafeName(In.MemberName)
			|| (In.DataType != TEXT("input") && In.DataType != TEXT("output")
				&& In.DataType != TEXT("variable"))))
	{
		OutError = TEXT("MetaSound member removal requires input, output, or variable plus a bounded member name.");
		return false;
	}
		if (In.Variant == TEXT("metasound.add_node")
			&& (!In.NodeId.IsValid()
				|| !GetProvenMetaSoundNodeRegistrySpecs().Contains(In.NodeKind)
				|| !In.DataType.IsEmpty()))
		{
			OutError = TEXT("MetaSound add_node requires one stable GUID and a proven native registry-backed node kind/version.");
			return false;
		}
		if (In.Variant == TEXT("metasound.configure_node")
			&& (!In.NodeId.IsValid()
				|| !GetProvenMetaSoundNodeRegistrySpecs().Contains(In.NodeKind)
				|| In.Parameters.IsEmpty()))
	{
		OutError = TEXT("MetaSound configure_node requires one existing descriptor GUID/kind/version and at least one typed descriptor parameter.");
		return false;
	}
	if ((In.Variant == TEXT("metasound.remove_node")) && !In.NodeId.IsValid())
	{
		OutError = TEXT("MetaSound node removal requires one exact persisted node GUID.");
		return false;
	}
	if ((In.Variant == TEXT("metasound.connect") || In.Variant == TEXT("metasound.disconnect"))
		&& (!In.FromNodeId.IsValid() || !In.ToNodeId.IsValid()
			|| (In.FromVertexId.IsValid() == !In.FromPortName.IsEmpty())
			|| (In.ToVertexId.IsValid() == !In.ToPortName.IsEmpty())))
	{
		OutError = TEXT("MetaSound connections require exact node GUIDs and exactly one vertex GUID or descriptor port name per endpoint.");
		return false;
	}
	if (In.Variant == TEXT("metasound.set_literal")
		&& (!In.NodeId.IsValid() || (In.VertexId.IsValid() == !In.VertexName.IsEmpty())
			|| In.Parameters.Num() != 1))
	{
		OutError = TEXT("MetaSound literal update requires one node GUID, exactly one vertex GUID or descriptor port name, and one typed parameter.");
		return false;
	}
	if (In.Variant == TEXT("metasound.declare_interface")
		&& In.MemberName != TEXT("source") && In.MemberName != TEXT("source_one_shot"))
	{
		OutError = TEXT("Only the standard source and source_one_shot MetaSound interfaces are admitted.");
		return false;
	}
	if (In.Variant == TEXT("synthesis.effect_chain.add_effect")
		&& !SynthesisEffectKinds.Contains(In.NodeKind))
	{
		OutError = TEXT("Synthesis effect construction accepts only closed semantic effect kinds.");
		return false;
	}
	if (In.Variant == TEXT("audio_gameplay_volume.add_mutator")
		&& !VolumeMutatorKinds.Contains(In.NodeKind))
	{
		OutError = TEXT("Audio Gameplay Volume mutators use a closed semantic allowlist.");
		return false;
	}
	if (In.Variant == TEXT("motor_sim.add_component")
		&& !MotorComponentKinds.Contains(In.NodeKind))
	{
		OutError = TEXT("MotorSim component construction uses a closed semantic allowlist.");
		return false;
	}
	if (In.Variant == TEXT("audio_gameplay_volume.create")
		&& (In.Extent.X <= 0.0 || In.Extent.Y <= 0.0 || In.Extent.Z <= 0.0
			|| FMath::Abs(In.Location.X) > WORLD_MAX || FMath::Abs(In.Location.Y) > WORLD_MAX
			|| FMath::Abs(In.Location.Z) > WORLD_MAX || In.Extent.X > WORLD_MAX
			|| In.Extent.Y > WORLD_MAX || In.Extent.Z > WORLD_MAX))
	{
		OutError = TEXT("Audio Gameplay Volume location/extent must be finite, positive, and within UE world bounds.");
		return false;
	}
	if (IsCreateVariant(In.Variant) && In.Variant != TEXT("audio_gameplay_volume.create")
		&& !IsCanonicalProjectAssetPath(In.TargetPath))
	{
		OutError = TEXT("Audio asset creation requires a canonical /Game package.object target.");
		return false;
	}

	const FString OptionalFamily = OptionalFamilyForVariant(In.Variant);
	if (!OptionalFamily.IsEmpty())
	{
		FHyperAIAudioPrerequisite Prerequisite;
		if (!IsGateReady(OptionalFamily, &Prerequisite))
		{
			OutError = FString::Printf(
				TEXT("Optional family '%s' is %s; its plugin/module was not loaded implicitly."),
				*OptionalFamily, *Prerequisite.State);
			return false;
		}
	}

	Out.Variant = In.Variant;
	Out.TargetPath = In.TargetPath;
	Out.ExpectedRevision = In.ExpectedRevision;
	Out.ReferencePath = In.ReferencePath;
	Out.SourcePaths = In.SourcePaths;
	Out.MemberName = In.MemberName;
	Out.SecondaryName = In.SecondaryName;
	Out.DataType = In.DataType;
	Out.NodeKind = In.NodeKind;
	Out.NodeDescriptorVersion = In.NodeDescriptorVersion;
	Out.NodeId = In.NodeId;
	Out.VertexId = In.VertexId;
	Out.VertexName = In.VertexName;
	Out.FromNodeId = In.FromNodeId;
	Out.FromVertexId = In.FromVertexId;
	Out.FromPortName = In.FromPortName;
	Out.ToNodeId = In.ToNodeId;
	Out.ToVertexId = In.ToVertexId;
	Out.ToPortName = In.ToPortName;
	Out.Index = In.Index;
	Out.Count = In.Count;
	Out.Value = In.Value;
	Out.SecondaryValue = In.SecondaryValue;
	Out.bSetEnabled = In.bSetEnabled;
	Out.bEnabled = In.bEnabled;
	Out.Location = In.Location;
	Out.Extent = In.Extent;
	Out.Parameters = In.Parameters;
	Out.Parameters.Sort([](const FHyperAIAudioParameterValue& Left,
		const FHyperAIAudioParameterValue& Right)
	{
		return Left.Name < Right.Name;
	});
		if (In.Variant == TEXT("metasound.add_node")
			&& !BuildMetaSoundDescriptorPorts(Out.NodeKind,
				Out.NodeDescriptorVersion, Out.DataType, Out.MetaSoundRegistryKey,
				Out.MetaSoundInterfaceFingerprint, Out.DescriptorInputs,
				Out.DescriptorOutputs, OutError)) return false;
	Out.OptionalFamily = OptionalFamily;
	return true;
}

bool FHyperAIStudioAudioContracts::Capture(
	const FHyperAIAudioInspectRequest& Request,
	FHyperAIAudioValueSnapshot& Out,
	FString& OutError)
{
	using namespace HyperAIStudio::Audio::Private;
	Out = {};
	Out.Scope = Request.Scope;
	OutError.Reset();
	if (Request.Scope != TEXT("loaded_only") && Request.Scope != TEXT("on_disk_index")
		&& Request.Scope != TEXT("loaded_and_on_disk"))
	{
		OutError = TEXT("Scope must be loaded_only, on_disk_index, or loaded_and_on_disk.");
		return false;
	}
	if (Request.PageSize < 1 || Request.PageSize > MaxPageSize
		|| Request.MaxOutputBytes < MinOutputBytes || Request.MaxOutputBytes > MaxOutputBytes
		|| Request.DeadlineMs < 1 || Request.DeadlineMs > MaxInspectDeadlineMs
		|| Request.Cursor.Len() > 192)
	{
		OutError = TEXT("Paging, output, cursor, or deadline is outside hard bounds.");
		return false;
	}
	if (!ValidateStringSet(Request.Variants, MaxVariants,
		[](const FString& Value) { return IsInspectVariant(Value); }, OutError)
		|| !ValidateStringSet(Request.ObjectPaths, MaxPaths,
		[](const FString& Value) { return IsCanonicalProjectAssetPath(Value); }, OutError)
		|| !ValidateProjection(Request.Projection, OutError)) return false;

	CollectPrerequisites(Out.Prerequisites);
	for (const FString& Variant : Request.Variants)
	{
		const FString Family = OptionalFamilyForVariant(Variant);
		FHyperAIAudioPrerequisite Prerequisite;
		if (!Family.IsEmpty() && !IsGateReady(Family, &Prerequisite))
		{
			Out.bComplete = false;
			AddCaptureIssue(Out, TEXT("optional_audio_prerequisite_unavailable"), TEXT("error"),
				Variant, FString(), FString::Printf(
					TEXT("Requested family '%s' is %s; no module load was attempted."),
					*Family, *Prerequisite.State));
		}
	}
	const double TotalDeadline = static_cast<double>(Request.DeadlineMs) / 1000.0;
	const double PerSourceDeadline = Request.Scope == TEXT("loaded_and_on_disk")
		? TotalDeadline * 0.5 : TotalDeadline;
	if ((Request.Scope == TEXT("loaded_only") || Request.Scope == TEXT("loaded_and_on_disk"))
		&& !CaptureLoaded(Request, Out, PerSourceDeadline)) return false;
	if ((Request.Scope == TEXT("on_disk_index") || Request.Scope == TEXT("loaded_and_on_disk"))
		&& !CaptureOnDisk(Request, Out, PerSourceDeadline)) return false;
	Out.Records.Sort([](const FHyperAIAudioRecord& Left, const FHyperAIAudioRecord& Right)
	{
		if (Left.StableId != Right.StableId) return Left.StableId < Right.StableId;
		if (Left.bLoaded != Right.bLoaded) return Left.bLoaded;
		return Left.Revision < Right.Revision;
	});
	for (int32 Index = Out.Records.Num() - 1; Index > 0; --Index)
	{
		if (Out.Records[Index].StableId == Out.Records[Index - 1].StableId)
			Out.Records.RemoveAt(Index);
	}
	ComputeSnapshotRevision(Out);
	return IsSha256(Out.Revision);
}

FString FHyperAIStudioAudioContracts::ComputeSnapshotRevision(FHyperAIAudioValueSnapshot& Snapshot)
{
	using namespace HyperAIStudio::Audio::Private;
	TArray<FHyperAIAudioRecord> Records = Snapshot.Records;
	Records.Sort([](const FHyperAIAudioRecord& Left, const FHyperAIAudioRecord& Right)
	{
		if (Left.StableId != Right.StableId) return Left.StableId < Right.StableId;
		return Left.Revision < Right.Revision;
	});
	TArray<FHyperAIAudioPrerequisite> Prerequisites = Snapshot.Prerequisites;
	Prerequisites.Sort([](const FHyperAIAudioPrerequisite& Left, const FHyperAIAudioPrerequisite& Right)
	{
		return Left.Family < Right.Family;
	});
	FString Canonical;
	AppendToken(Canonical, TEXT("hyperai.audio-snapshot.v1"));
	AppendToken(Canonical, Snapshot.Scope);
	AppendBool(Canonical, Snapshot.bComplete);
	AppendInt(Canonical, Snapshot.Scanned);
	for (const FHyperAIAudioPrerequisite& Prerequisite : Prerequisites)
	{
		AppendToken(Canonical, Prerequisite.Family);
		AppendToken(Canonical, Prerequisite.Plugin);
		AppendToken(Canonical, Prerequisite.Module);
		AppendToken(Canonical, Prerequisite.State);
	}
	for (const FHyperAIAudioRecord& Record : Records) AppendToken(Canonical, Record.Revision);
	Snapshot.Revision = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
	if (!IsSha256(Snapshot.Revision))
	{
		Snapshot.Revision.Reset();
		Snapshot.bComplete = false;
	}
	return Snapshot.Revision;
}

TArray<FHyperAIAudioIssue> FHyperAIStudioAudioContracts::ValidateSnapshot(
	const FHyperAIAudioValueSnapshot& Snapshot,
	const int32 MaxIssueCount,
	bool& bOutTruncated)
{
	using namespace HyperAIStudio::Audio::Private;
	TArray<FHyperAIAudioIssue> Issues;
	bOutTruncated = false;
	const int32 Maximum = FMath::Clamp(MaxIssueCount, 1, MaxIssues);
	for (const FHyperAIAudioIssue& Existing : Snapshot.Issues)
	{
		AddIssue(Issues, Maximum, bOutTruncated, *Existing.Code, *Existing.Severity,
			Existing.Variant, Existing.ObjectPath, Existing.StableId,
			Existing.OperationIndex, Existing.Message);
	}
	if (!Snapshot.bComplete || !IsSha256(Snapshot.Revision))
	{
		AddIssue(Issues, Maximum, bOutTruncated, TEXT("revision_incomplete"), TEXT("error"),
			TEXT("snapshot"), FString(), FString(), -1,
			TEXT("Independent validation requires a complete bounded audio source revision."));
	}
	if (Snapshot.Records.IsEmpty())
	{
		AddIssue(Issues, Maximum, bOutTruncated, TEXT("no_matching_audio_records"), TEXT("warning"),
			TEXT("snapshot"), FString(), FString(), -1,
			TEXT("No loaded/on-disk audio records matched the closed request."));
	}
	for (const FHyperAIAudioRecord& Record : Snapshot.Records)
	{
		if (!IsSha256(Record.Revision))
		{
			AddIssue(Issues, Maximum, bOutTruncated, TEXT("record_revision_missing"), TEXT("error"),
				Record.Variant, Record.ObjectPath, Record.StableId, -1,
				TEXT("Record cannot participate in exact revision-CAS."));
		}
		if (Record.Variant == TEXT("sound_wave") && Record.bLoaded
			&& (Record.DurationSeconds < 0.0 || Record.NumChannels < 1 || Record.SampleRate <= 0.0))
		{
			AddIssue(Issues, Maximum, bOutTruncated, TEXT("sound_wave_metrics_invalid"), TEXT("warning"),
				Record.Variant, Record.ObjectPath, Record.StableId, -1,
				TEXT("Loaded SoundWave reports invalid duration, channel, or sample-rate metadata."));
		}
		if (Record.Variant == TEXT("sound_cue") && Record.bLoaded && Record.PrimaryCount == 0)
		{
			AddIssue(Issues, Maximum, bOutTruncated, TEXT("sound_cue_has_no_nodes"), TEXT("warning"),
				Record.Variant, Record.ObjectPath, Record.StableId, -1,
				TEXT("Loaded SoundCue has no persisted editor nodes."));
		}
		if (Record.Variant.StartsWith(TEXT("metasound_")) && Record.bLoaded
			&& Record.Details.Contains(TEXT("frontend_valid=false")))
		{
			AddIssue(Issues, Maximum, bOutTruncated, TEXT("metasound_frontend_invalid"), TEXT("error"),
				Record.Variant, Record.ObjectPath, Record.StableId, -1,
				TEXT("Persisted MetaSound frontend graph failed node, port, variable, or connection invariants."));
		}
		if (Record.bDirty)
		{
			AddIssue(Issues, Maximum, bOutTruncated, TEXT("unsaved_loaded_audio_state"), TEXT("info"),
				Record.Variant, Record.ObjectPath, Record.StableId, -1,
				TEXT("Record includes unsaved loaded state; on-disk metadata may differ."));
		}
	}
	if (Snapshot.Scope.Contains(TEXT("on_disk")))
	{
		AddIssue(Issues, Maximum, bOutTruncated, TEXT("on_disk_index_deferred"), TEXT("info"),
			TEXT("snapshot"), FString(), FString(), -1,
			TEXT("On-disk class validation is deferred until a hard-bounded asynchronous typed index is available; no synchronous object lookup or asset load was attempted."));
	}
	return Issues;
}

FHyperAIAudioInspectReport UHyperAIStudioAudioToolset::hyper_audio_inspect(
	const FHyperAIAudioInspectRequest& Request)
{
	using namespace HyperAIStudio::Audio::Private;
	FHyperAIAudioInspectReport Report;
	Report.ObservationScope = Request.Scope;
	FHyperAIAudioValueSnapshot Snapshot;
	if (!FHyperAIStudioAudioContracts::Capture(Request, Snapshot, Report.Diagnostic))
	{
		Report.Status = TEXT("invalid_request_or_capture_failed");
		return Report;
	}
	Report.Revision = Snapshot.Revision;
	Report.bRevisionComplete = Snapshot.bComplete;
	Report.SourceObjectsScanned = Snapshot.Scanned;
	Report.TotalRecords = Snapshot.Records.Num();
	int32 Offset = 0;
	if (!ParseCursor(Request.Cursor, Snapshot.Revision, Offset) || Offset > Snapshot.Records.Num())
	{
		Report.Status = TEXT("stale_or_invalid_cursor");
		Report.Diagnostic = TEXT("Cursor is malformed or bound to a different audio revision.");
		return Report;
	}
	int64 EstimatedBytes = 2048;
	for (const FHyperAIAudioPrerequisite& Prerequisite : Snapshot.Prerequisites)
	{
		const int64 Bytes = EstimatePrerequisiteBytes(Prerequisite);
		if (EstimatedBytes + Bytes > Request.MaxOutputBytes)
		{
			Report.bTruncated = true;
			Report.Status = TEXT("output_budget_too_small");
			Report.Diagnostic = TEXT("Prerequisite evidence cannot fit within MaxOutputBytes.");
			return Report;
		}
		EstimatedBytes += Bytes;
		Report.Prerequisites.Add(Prerequisite);
	}
	for (const FHyperAIAudioIssue& Issue : Snapshot.Issues)
	{
		const int64 Bytes = EstimateIssueBytes(Issue);
		if (EstimatedBytes + Bytes > Request.MaxOutputBytes)
		{
			Report.bTruncated = true;
			Report.Status = TEXT("output_budget_too_small");
			Report.Diagnostic = TEXT("Issue evidence cannot fit within MaxOutputBytes.");
			return Report;
		}
		EstimatedBytes += Bytes;
		Report.Issues.Add(Issue);
	}
	const int32 End = FMath::Min(Offset + Request.PageSize, Snapshot.Records.Num());
	for (int32 Index = Offset; Index < End; ++Index)
	{
		FHyperAIAudioRecord Record = Snapshot.Records[Index];
		ProjectRecord(Record, Request.Projection);
		const int64 RecordBytes = EstimateBytes(Record);
		if (EstimatedBytes + RecordBytes > Request.MaxOutputBytes)
		{
			Report.bTruncated = true;
			break;
		}
		EstimatedBytes += RecordBytes;
		Report.Records.Add(MoveTemp(Record));
	}
	Report.ReturnedRecords = Report.Records.Num();
	const int32 NextOffset = Offset + Report.ReturnedRecords;
	Report.bTruncated |= NextOffset < Snapshot.Records.Num();
	if (Report.bTruncated) Report.NextCursor = MakeCursor(Snapshot.Revision, NextOffset);
	if (Report.Records.IsEmpty() && Offset < Snapshot.Records.Num())
	{
		Report.Status = TEXT("output_budget_too_small");
		Report.Diagnostic = TEXT("The selected audio projection cannot fit one record in MaxOutputBytes.");
		return Report;
	}
	Report.bOk = true;
	Report.Status = Snapshot.bComplete ? TEXT("ok") : TEXT("ok_incomplete_revision");
	Report.Diagnostic = TEXT("Bounded audio records and prerequisite evidence were captured without loading assets or optional modules.");
	return Report;
}

FHyperAIAudioValidateReport UHyperAIStudioAudioToolset::hyper_audio_validate(
	const FHyperAIAudioValidateRequest& Request)
{
	FHyperAIAudioValidateReport Report;
	if (Request.MaxIssues < 1 || Request.MaxIssues > FHyperAIStudioAudioContracts::MaxIssues
		|| Request.DeadlineMs < 1
		|| Request.DeadlineMs > FHyperAIStudioAudioContracts::MaxInspectDeadlineMs)
	{
		Report.Status = TEXT("invalid_validation_bound");
		Report.Diagnostic = TEXT("MaxIssues or DeadlineMs is outside hard bounds.");
		return Report;
	}
	FHyperAIAudioInspectRequest Inspect;
	Inspect.Scope = Request.Scope;
	Inspect.Variants = Request.Variants;
	Inspect.ObjectPaths = Request.ObjectPaths;
	Inspect.PageSize = FHyperAIStudioAudioContracts::MaxPageSize;
	Inspect.MaxOutputBytes = FHyperAIStudioAudioContracts::MaxOutputBytes;
	Inspect.DeadlineMs = Request.DeadlineMs;
	FHyperAIAudioValueSnapshot Snapshot;
	if (!FHyperAIStudioAudioContracts::Capture(Inspect, Snapshot, Report.Diagnostic))
	{
		Report.Status = TEXT("invalid_request_or_capture_failed");
		return Report;
	}
	Report.Revision = Snapshot.Revision;
	Report.bRevisionComplete = Snapshot.bComplete;
	Report.Prerequisites = Snapshot.Prerequisites;
	Report.Issues = FHyperAIStudioAudioContracts::ValidateSnapshot(
		Snapshot, Request.MaxIssues, Report.bTruncated);
	for (const FHyperAIAudioIssue& Issue : Report.Issues)
	{
		if (Issue.Severity == TEXT("error")) ++Report.ErrorCount;
		else if (Issue.Severity == TEXT("warning")) ++Report.WarningCount;
		else ++Report.InfoCount;
	}
	Report.bOk = true;
	Report.bValid = Report.ErrorCount == 0 && Snapshot.bComplete;
	Report.Status = Report.bValid ? TEXT("valid") : TEXT("invalid");
	Report.Diagnostic = TEXT("Independent Sound-asset, reference, optional-prerequisite, and persisted MetaSound frontend invariants were evaluated.");
	return Report;
}

FHyperAIAudioApplyPlanReport FHyperAIStudioAudioContracts::BuildPlan(
	const FHyperAIAudioApplyPlanRequest& Request)
{
	using namespace HyperAIStudio::Audio::Private;
	FHyperAIAudioApplyPlanReport Report;
	Report.bDryRun = Request.bDryRun;
	Report.OperationId = Clip(Request.OperationId, 128);
	if (Request.bDryRun && (!Request.OperationId.IsEmpty() || !Request.ExpectedPlanHash.IsEmpty()))
	{
		Report.Status = TEXT("unexpected_submission_fields");
		Report.Diagnostic = TEXT("Dry-run audio planning prohibits operation_id and expected_plan_hash submission fields.");
		return Report;
	}
	if (Request.Operations.IsEmpty() || Request.Operations.Num() > MaxOperations)
	{
		Report.Status = TEXT("invalid_operation_count");
		Report.Diagnostic = TEXT("An audio plan requires 1..64 closed operations.");
		return Report;
	}
	if (Request.DeadlineMs < 1 || Request.DeadlineMs > MaxApplyDeadlineMs)
	{
		Report.Status = TEXT("invalid_deadline");
		Report.Diagnostic = TEXT("Audio plan DeadlineMs is outside the shared synchronous bound.");
		return Report;
	}
	const double PlanStarted = FPlatformTime::Seconds();
	const double PlanDeadline = PlanStarted
		+ static_cast<double>(Request.DeadlineMs) / 1000.0;
	CollectPrerequisites(Report.Prerequisites);
	TArray<FHyperAIAudioBackendOperation> Operations;
	TArray<FString> BaseTokens;
	TMap<FString, FString> TargetBaseRevisions;
	TSet<FString> CreatedTargets;
	TMap<FString, FShadowAudioTarget> ShadowTargets;
	TSet<FString> RequiredFamilies;
	int32 TotalParameters = 0;
	for (int32 Index = 0; Index < Request.Operations.Num(); ++Index)
	{
		if (FPlatformTime::Seconds() > PlanDeadline)
		{
			Report.Status = TEXT("deadline_exceeded");
			Report.Diagnostic = TEXT("Bounded audio planning stopped before its synchronous deadline.");
			return Report;
		}
		FHyperAIAudioBackendOperation Operation;
		FString Error;
		if (!ValidateOperationShape(Request.Operations[Index], Operation, Error))
		{
			bool bTruncated = false;
			AddIssue(Report.Issues, MaxIssues, bTruncated, TEXT("invalid_operation"), TEXT("error"),
				Request.Operations[Index].Variant, Request.Operations[Index].TargetPath,
				FString(), Index, Error);
			Report.Status = TEXT("invalid_operation");
			Report.Diagnostic = TEXT("One audio operation failed its closed schema or prerequisite gate.");
			return Report;
		}
		TotalParameters += Operation.Parameters.Num();
		if (TotalParameters > MaxTotalParameters)
		{
			Report.Status = TEXT("aggregate_parameter_bound_exceeded");
			Report.Diagnostic = TEXT("Aggregate typed audio parameters exceed the hard plan bound.");
			return Report;
		}
		FString CurrentRevision;
		if (const FString* ExistingBase = TargetBaseRevisions.Find(Operation.TargetPath))
		{
			if (CreatedTargets.Contains(Operation.TargetPath))
			{
				if (!Operation.ExpectedRevision.IsEmpty() || IsCreateVariant(Operation.Variant))
				{
					Report.Status = TEXT("invalid_planned_target_precondition");
					Report.Diagnostic = TEXT("Operations after a create use the planned target with empty CAS and cannot create it again.");
					return Report;
				}
			}
			else if (Operation.ExpectedRevision != *ExistingBase)
			{
				Report.Status = TEXT("inconsistent_target_cas");
				Report.Diagnostic = TEXT("Repeated operations on one loaded audio target must echo one exact base revision.");
				return Report;
			}
			CurrentRevision = *ExistingBase;
		}
		else
		{
			if (!ValidateTargetCas(Operation.Variant, Operation.TargetPath,
				Operation.ExpectedRevision, CurrentRevision, Error, PlanDeadline))
			{
				bool bTruncated = false;
				AddIssue(Report.Issues, MaxIssues, bTruncated, TEXT("cas_failed"), TEXT("error"),
					Operation.Variant, Operation.TargetPath, FString(), Index, Error);
				Report.Status = TEXT("cas_failed");
				Report.Diagnostic = TEXT("An audio target existence/revision precondition failed without loading it.");
				return Report;
			}
			TargetBaseRevisions.Add(Operation.TargetPath,
				CurrentRevision.IsEmpty() ? FString() : CurrentRevision);
			if (IsCreateVariant(Operation.Variant)) CreatedTargets.Add(Operation.TargetPath);
			BaseTokens.Add(Operation.TargetPath + TEXT("=")
				+ (CurrentRevision.IsEmpty() ? TEXT("absent") : CurrentRevision));
		}
		if (!ShadowTargets.Contains(Operation.TargetPath))
		{
			FShadowAudioTarget State;
			State.ObjectPath = Operation.TargetPath;
			if (IsCreateVariant(Operation.Variant))
			{
				State.bCreated = true;
				State.Family = OperationTargetFamily(Operation.Variant);
				State.ClassKind = OperationClassKind(Operation.Variant);
				State.ClassPath = TEXT("planned:") + State.ClassKind;
			}
			else if (!InitializeShadowFromLoaded(
				FindObject<UObject>(nullptr, *Operation.TargetPath), State, Error, PlanDeadline))
			{
				Report.Status = TEXT("shadow_state_unavailable");
				Report.Diagnostic = Clip(Error);
				return Report;
			}
			ShadowTargets.Add(Operation.TargetPath, MoveTemp(State));
		}
		if (!Operation.ReferencePath.IsEmpty())
		{
			if (FPlatformTime::Seconds() > PlanDeadline)
			{
				Report.Status = TEXT("deadline_exceeded");
				Report.Diagnostic = TEXT("Audio reference validation exceeded the plan deadline.");
				return Report;
			}
			const FShadowAudioTarget* PlannedReference = ShadowTargets.Find(Operation.ReferencePath);
			const UObject* LoadedReference = FindObject<UObject>(nullptr, *Operation.ReferencePath);
			FString ReferenceRevision = PlannedReference && PlannedReference->bCreated
				? ShadowCanonical(*PlannedReference)
				: LoadedReference
					? ComputeLoadedRevision(LoadedReference, nullptr, true, PlanDeadline)
					: FString();
			if (!IsSha256(ReferenceRevision))
			{
				Report.Status = TEXT("reference_unavailable");
				Report.Diagnostic = TEXT("A typed audio reference must be planned in this batch or already loaded; no blocking object-index lookup was attempted.");
				return Report;
			}
			const FString ReferenceVariant = PlannedReference && PlannedReference->bCreated
				? PlannedReference->Family : LoadedVariant(Operation.ReferencePath);
			if (!IsReferenceVariantCompatible(Operation.Variant, ReferenceVariant))
			{
				Report.Status = TEXT("reference_type_mismatch");
				Report.Diagnostic = TEXT("A typed audio reference does not match the operation's closed asset family.");
				return Report;
			}
			BaseTokens.Add(Operation.ReferencePath + TEXT("=") + ReferenceRevision);
		}
		for (const FString& Source : Operation.SourcePaths)
		{
			if (FPlatformTime::Seconds() > PlanDeadline)
			{
				Report.Status = TEXT("deadline_exceeded");
				Report.Diagnostic = TEXT("Audio source validation exceeded the plan deadline.");
				return Report;
			}
			const FShadowAudioTarget* PlannedSource = ShadowTargets.Find(Source);
			const UObject* LoadedSource = FindObject<UObject>(nullptr, *Source);
			const FString SourceRevision = PlannedSource && PlannedSource->bCreated
				? ShadowCanonical(*PlannedSource)
				: LoadedSource
					? ComputeLoadedRevision(LoadedSource, nullptr, true, PlanDeadline)
					: FString();
			if (!IsSha256(SourceRevision))
			{
				Report.Status = TEXT("source_unavailable");
				Report.Diagnostic = TEXT("A typed audio source must be planned in this batch or already loaded; no blocking object-index lookup was attempted.");
				return Report;
			}
			const FString SourceVariant = PlannedSource && PlannedSource->bCreated
				? PlannedSource->Family : LoadedVariant(Source);
			if (!IsSourceVariantCompatible(Operation.Variant, SourceVariant))
			{
				Report.Status = TEXT("source_type_mismatch");
				Report.Diagnostic = TEXT("A typed audio source does not match the operation's closed source family.");
				return Report;
			}
			BaseTokens.Add(Source + TEXT("=") + SourceRevision);
		}
		for (const FHyperAIAudioParameterValue& Parameter : Operation.Parameters)
		{
			if (Parameter.Type != TEXT("object")) continue;
			if (FPlatformTime::Seconds() > PlanDeadline)
			{
				Report.Status = TEXT("deadline_exceeded");
				Report.Diagnostic = TEXT("Audio object-parameter validation exceeded the plan deadline.");
				return Report;
			}
			const FShadowAudioTarget* PlannedParameter = ShadowTargets.Find(Parameter.ObjectPath);
			const UObject* LoadedParameterObject =
				FindObject<UObject>(nullptr, *Parameter.ObjectPath);
			const FString ParameterRevision = PlannedParameter && PlannedParameter->bCreated
				? ShadowCanonical(*PlannedParameter)
				: LoadedParameterObject
					? ComputeLoadedRevision(LoadedParameterObject, nullptr, true, PlanDeadline)
					: FString();
			if (!IsSha256(ParameterRevision))
			{
				Report.Status = TEXT("object_parameter_unavailable");
				Report.Diagnostic = TEXT("A closed object parameter must be planned in this batch or already loaded; no blocking object-index lookup was attempted.");
				return Report;
			}
			FAudioClassEvidence ParameterClass;
			bool bParameterCompatible = false;
			if (PlannedParameter && PlannedParameter->bCreated)
			{
				const FString Family = PlannedParameter->Family;
				if (Operation.Variant == TEXT("metasound.configure_node")
					&& Operation.NodeKind == TEXT("wave_player") && Parameter.Name == TEXT("sound"))
					bParameterCompatible = Family == TEXT("sound_wave");
				else bParameterCompatible = (Parameter.Name == TEXT("attenuation") && Family == TEXT("sound_attenuation"))
					|| (Parameter.Name == TEXT("concurrency") && Family == TEXT("sound_concurrency"))
					|| (Parameter.Name == TEXT("parent") && Family == TEXT("sound_class"))
					|| ((Parameter.Name == TEXT("sound") || Parameter.Name == TEXT("source"))
						&& (Family == TEXT("sound_wave") || Family == TEXT("sound_cue")
							|| Family == TEXT("metasound_source")))
					|| (Parameter.Name == TEXT("value")
						&& (Family == TEXT("sound_wave") || Family == TEXT("metasound_source")));
			}
			else if (LoadedClassEvidence(Parameter.ObjectPath, ParameterClass))
			{
				bParameterCompatible = Operation.Variant == TEXT("metasound.configure_node")
					&& Operation.NodeKind == TEXT("wave_player") && Parameter.Name == TEXT("sound")
					? ParameterClass.Is(TEXT("/Script/Engine"), TEXT("SoundWave"))
					: IsObjectParameterCompatible(Parameter.Name, ParameterClass);
			}
			if (!bParameterCompatible)
			{
				Report.Status = TEXT("object_parameter_type_mismatch");
				Report.Diagnostic = TEXT("A closed object parameter does not match its semantic audio asset class.");
				return Report;
			}
			BaseTokens.Add(Parameter.ObjectPath + TEXT("=") + ParameterRevision);
		}
		// Replay copy-on-write: every accepted operation produces a new typed state;
		// a rejected operation can never partially mutate the target shadow used by
		// later semantic checks or by the immutable prepared artifact.
		const FShadowAudioTarget* CurrentShadow = ShadowTargets.Find(Operation.TargetPath);
		FShadowAudioTarget NextShadow;
		if (CurrentShadow) NextShadow = *CurrentShadow;
		if (!CurrentShadow || !ReplayShadowOperation(Operation, NextShadow, ShadowTargets, Error))
		{
			bool bTruncated = false;
			AddIssue(Report.Issues, MaxIssues, bTruncated, TEXT("semantic_replay_failed"), TEXT("error"),
				Operation.Variant, Operation.TargetPath, FString(), Index,
				Error.IsEmpty() ? TEXT("Typed target shadow was unavailable.") : Error);
			Report.Status = TEXT("semantic_replay_failed");
			Report.Diagnostic = TEXT("Ordered immutable audio shadow replay rejected a family, GUID, member, edge, index, reference, or cycle invariant.");
			return Report;
		}
		ShadowTargets.Add(Operation.TargetPath, MoveTemp(NextShadow));
		if (!Operation.OptionalFamily.IsEmpty())
		{
			RequiredFamilies.Add(Operation.OptionalFamily);
			if (Operation.OptionalFamily == TEXT("motor_sim_components"))
				RequiredFamilies.Add(TEXT("motor_sim"));
		}
		++Report.Effects.OperationCount;
		if (IsCreateVariant(Operation.Variant)) ++Report.Effects.CreateCount;
		else if (IsRemoveVariant(Operation.Variant)) ++Report.Effects.RemoveCount;
		else ++Report.Effects.UpdateCount;
		if (IsMetaSoundGraphVariant(Operation.Variant)) ++Report.Effects.MetaSoundGraphOperationCount;
		if (IsOptionalOperation(Operation.Variant)) ++Report.Effects.OptionalPluginOperationCount;
		Report.Effects.bMayRemovePersistedData |= IsRemoveVariant(Operation.Variant);
		Operations.Add(MoveTemp(Operation));
	}
	for (const TPair<FString, FShadowAudioTarget>& Pair : ShadowTargets)
	{
		FString CoupledError;
		if (!ValidateFinalCoupledState(Pair.Value, CoupledError))
		{
			bool bTruncated = false;
			AddIssue(Report.Issues, MaxIssues, bTruncated, TEXT("coupled_invariant_failed"),
				TEXT("error"), TEXT("final_state"), Pair.Key, FString(), -1, CoupledError);
			Report.Status = TEXT("coupled_invariant_failed");
			Report.Diagnostic = TEXT("Final exact coupled audio state was unavailable or invalid after complete ordered replay.");
			return Report;
		}
	}
	if (FPlatformTime::Seconds() > PlanDeadline)
	{
		Report.Status = TEXT("deadline_exceeded");
		Report.Diagnostic = TEXT("Audio plan normalization exceeded the synchronous deadline.");
		return Report;
	}
	BaseTokens.Sort();
	FString BaseCanonical;
	AppendToken(BaseCanonical, TEXT("hyperai.audio-base.v1"));
	for (const FString& Token : BaseTokens) AppendToken(BaseCanonical, Token);
	Report.BaseRevision = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(BaseCanonical);
	if (!IsSha256(Report.BaseRevision))
	{
		Report.Status = TEXT("base_revision_unavailable");
		Report.Diagnostic = TEXT("A deterministic audio base revision could not be produced.");
		return Report;
	}

	TSharedRef<FHyperAIAudioTypedArtifactPayload, ESPMode::ThreadSafe> Payload =
		MakeShared<FHyperAIAudioTypedArtifactPayload, ESPMode::ThreadSafe>();
	Payload->TypeId = TEXT("hyperai.payload.audio.plan.v1");
	Payload->SchemaFingerprint = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("hyperai.schema.audio.plan.v2:descriptor-v1-composite-vertices-final-coupled-state:closed-no-playback-no-mic-no-dsp-no-script-no-file"));
	Payload->PackId = PackId;
	Payload->BaseRevision = Report.BaseRevision;
	for (const FHyperAIAudioBackendOperation& Operation : Operations)
		Payload->CanonicalOperations.Add(OperationCanonical(Operation));
	TArray<FString> ShadowPaths;
	ShadowTargets.GetKeys(ShadowPaths);
	ShadowPaths.Sort();
	for (const FString& ShadowPath : ShadowPaths)
	{
		const FString ShadowHash = ShadowCanonical(ShadowTargets.FindChecked(ShadowPath));
		if (!IsSha256(ShadowHash))
		{
			Report.Status = TEXT("shadow_hash_unavailable");
			Report.Diagnostic = TEXT("Final typed audio shadow exceeded deterministic hash bounds.");
			return Report;
		}
		Payload->CanonicalShadowStates.Add(ShadowPath + TEXT("=") + ShadowHash);
	}
	const FString BeforeClone = Payload->GetSemanticFingerprint();
	const TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> Detached =
		Payload->CloneImmutable();
	if (!IsSha256(BeforeClone) || &Detached.Get() == &Payload.Get()
		|| Detached->GetSemanticFingerprint() != BeforeClone)
	{
		Report.Status = TEXT("immutable_payload_failed");
		Report.Diagnostic = TEXT("Typed audio plan could not produce an exact detached immutable snapshot.");
		return Report;
	}
	TArray<FHyperAIAudioPrerequisite> UsedPrerequisites;
	for (const FHyperAIAudioPrerequisite& Prerequisite : Report.Prerequisites)
	{
		if (RequiredFamilies.Contains(Prerequisite.Family)) UsedPrerequisites.Add(Prerequisite);
	}
	if (UsedPrerequisites.IsEmpty())
	{
		FHyperAIAudioPrerequisite Core;
		Core.Family = TEXT("engine_audio_assets");
		Core.Module = TEXT("Engine");
		Core.State = TEXT("available");
		UsedPrerequisites.Add(MoveTemp(Core));
	}
	FHyperAIStudioPreparedTypedArtifact Prepared;
	FString PrepareError;
	const FString EffectTarget = Operations.Num() == 1
		? Operations[0].TargetPath : TEXT("project-audio-cohort");
	if (!BuildPreparedArtifact(Request, UsedPrerequisites, EffectTarget, Payload, Prepared, PrepareError))
	{
		Report.Status = TEXT("shared_prepare_rejected");
		Report.Diagnostic = Clip(PrepareError);
		return Report;
	}
	Report.PlanHash = Prepared.PlanHash;
	Report.AuthorizationPlanHash = Prepared.AuthorizationPlanHash;
	Report.CapabilityHash = Prepared.CapabilityHash;
	Report.EffectFingerprint = Prepared.EffectFingerprint;
	Report.PreparedContractFingerprint = Prepared.ContractFingerprint;
	if (Request.bDryRun)
	{
		Report.bOk = true;
		Report.Status = TEXT("valid_requires_trusted_authorization");
		Report.Diagnostic = TEXT("External-effect audio plan, CAS, optional gates, effect summary, ")
			TEXT("and shared typed-artifact hashes validated without mutation, playback, or capture.");
		return Report;
	}
	if (!FHyperAIStudioExtensionRuntime::IsValidOperationId(Request.OperationId))
	{
		Report.Status = TEXT("invalid_operation_id");
		Report.Diagnostic = TEXT("Non-dry-run audio submission requires a valid shared-journal operation_id.");
		return Report;
	}
	if (!IsSha256(Request.ExpectedPlanHash) || Request.ExpectedPlanHash != Report.PlanHash)
	{
		Report.Status = TEXT("expected_plan_hash_mismatch");
		Report.Diagnostic = TEXT("Non-dry-run submission must echo the exact shared dry-run plan hash.");
		return Report;
	}
	Report.bOk = false;
	Report.bExecutionSubmitted = false;
	Report.Status = TEXT("staged_backend_required");
	Report.Diagnostic = TEXT("No audio effect was staged: the async host, server-issued trusted grant, ")
		TEXT("real typed adapter, independent validator, and durable journal are required.");
	return Report;
}

FHyperAIAudioApplyPlanReport UHyperAIStudioAudioToolset::hyper_audio_apply_plan(
	const FHyperAIAudioApplyPlanRequest& Request)
{
	return FHyperAIStudioAudioContracts::BuildPlan(Request);
}

void FHyperAIStudioAudioRegistration::Startup()
{
	if (bStarted) return;
	bStarted = true;
	PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(
		this, &FHyperAIStudioAudioRegistration::RegisterAfterEngineInit);
	if (GIsRunning && GEngine && UObjectInitialized()) RegisterAfterEngineInit();
}

void FHyperAIStudioAudioRegistration::Shutdown()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}
	if (bOwnsRegistration && IsInGameThread() && UObjectInitialized())
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::UnregisterOwned(
			UHyperAIStudioAudioToolset::StaticClass(),
			FHyperAIStudioAudioContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioAudio, Error,
				TEXT("Audio owned-registration shutdown failed closed: %s"), *Error);
		}
		else bOwnsRegistration = false;
	}
	bStarted = false;
}

bool FHyperAIStudioAudioRegistration::IsRegistered() const
{
	const bool bDev = FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled();
	return FHyperAIStudioAudioContracts::IsRegistrationAllowed(bDev)
		&& FHyperAIStudioCapabilityRuntimeIndex::IsOwned(
			UHyperAIStudioAudioToolset::StaticClass(),
			FHyperAIStudioAudioContracts::GetQualifiedToolsetName());
}

void FHyperAIStudioAudioRegistration::RegisterAfterEngineInit()
{
	if (!bStarted || !IsInGameThread() || IsEngineExitRequested() || !UObjectInitialized()) return;
	const bool bDev = FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled();
	if (!FHyperAIStudioAudioContracts::IsRegistrationAllowed(bDev)) return;
	if (FHyperAIStudioCapabilityRuntimeIndex::IsOwned(
		UHyperAIStudioAudioToolset::StaticClass(),
		FHyperAIStudioAudioContracts::GetQualifiedToolsetName()))
	{
		bOwnsRegistration = true;
		return;
	}
	FString Error;
	if (!FHyperAIStudioCapabilityRuntimeIndex::RegisterOwnedToolsetClass(
		UHyperAIStudioAudioToolset::StaticClass(),
		FHyperAIStudioAudioContracts::GetQualifiedToolsetName(), Error))
	{
		UE_LOG(LogHyperAIStudioAudio, Error,
			TEXT("Audio three-tool cohort registration failed closed: %s"), *Error);
		return;
	}
	bOwnsRegistration = true;
}
