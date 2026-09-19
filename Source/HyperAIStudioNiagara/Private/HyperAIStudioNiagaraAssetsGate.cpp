// Games by Hyper 2026.

#include "HyperAIStudioNiagaraAssetsGate.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "HyperAIStudioAsyncJobHost.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "Misc/PackageName.h"
#include "NiagaraComponent.h"
#include "NiagaraDataChannel.h"
#include "NiagaraDataChannelAsset.h"
#include "NiagaraDataChannelVariable.h"
#include "NiagaraDataChannel_GameplayBurst.h"
#include "NiagaraDataChannel_Global.h"
#include "NiagaraDataChannel_Islands.h"
#include "NiagaraEffectType.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraSimCache.h"
#include "NiagaraSimCacheCompare.h"
#include "NiagaraSystem.h"
#include "PreviewScene.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/StrongObjectPtr.h"

namespace HyperAIStudio::Niagara::AssetsGate
{
	namespace
	{
		constexpr int32 MaxCaptures = 4;
		constexpr double CaptureLifetimeSeconds = 600.0;
		constexpr double FrameBudgetSeconds = 0.06;

		bool Fail(FString& OutStatus, FString& OutDiagnostic, const TCHAR* Status, const FString& Diagnostic)
		{
			OutStatus = Status;
			OutDiagnostic = Diagnostic;
			return false;
		}

		bool ParseFloat(const FString& Value, float& Out)
		{
			double Parsed = 0.0;
			if (!LexTryParseString(Parsed, *Value.TrimStartAndEnd()) || !FMath::IsFinite(Parsed)) return false;
			Out = static_cast<float>(Parsed);
			return FMath::IsFinite(Out);
		}

		bool ParseBool(const FString& Value, bool& Out)
		{
			const FString Trimmed = Value.TrimStartAndEnd();
			if (Trimmed.Equals(TEXT("true"), ESearchCase::IgnoreCase) || Trimmed == TEXT("1")) { Out = true; return true; }
			if (Trimmed.Equals(TEXT("false"), ESearchCase::IgnoreCase) || Trimmed == TEXT("0")) { Out = false; return true; }
			return false;
		}

		bool IsIdentifier(const FString& Value)
		{
			if (Value.IsEmpty() || Value.Len() > 64 || !(FChar::IsAlpha(Value[0]) || Value[0] == TEXT('_'))) return false;
			for (const TCHAR Char : Value)
			{
				if (!FChar::IsAlnum(Char) && Char != TEXT('_')) return false;
			}
			return true;
		}

		bool IsGameObjectPath(const FString& Path)
		{
			if (!Path.StartsWith(TEXT("/Game/")) || Path.Len() > 512 || !FPackageName::IsValidObjectPath(Path)) return false;
			const FString Package = FPackageName::ObjectPathToPackageName(Path);
			return FPackageName::GetLongPackageAssetName(Package) == FPackageName::ObjectPathToObjectName(Path);
		}

		bool IsAbsent(const FString& Path)
		{
			return !FSoftObjectPath(Path).ResolveObject()
				&& !FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(Path));
		}

		template <typename EnumType>
		bool ParseEnum(const FString& Value, const TArray<TPair<const TCHAR*, EnumType>>& Table, EnumType& Out)
		{
			for (const TPair<const TCHAR*, EnumType>& Row : Table)
			{
				if (Value.TrimStartAndEnd().Equals(Row.Key, ESearchCase::IgnoreCase)) { Out = Row.Value; return true; }
			}
			return false;
		}

		template <typename EnumType>
		FString EnumName(const EnumType Value, const TArray<TPair<const TCHAR*, EnumType>>& Table)
		{
			for (const TPair<const TCHAR*, EnumType>& Row : Table)
			{
				if (Row.Value == Value) return Row.Key;
			}
			return TEXT("other");
		}

		const TArray<TPair<const TCHAR*, ENiagaraScalabilityUpdateFrequency>>& UpdateFrequencies()
		{
			static const TArray<TPair<const TCHAR*, ENiagaraScalabilityUpdateFrequency>> Table = {
				{TEXT("spawn_only"), ENiagaraScalabilityUpdateFrequency::SpawnOnly}, {TEXT("low"), ENiagaraScalabilityUpdateFrequency::Low},
				{TEXT("medium"), ENiagaraScalabilityUpdateFrequency::Medium}, {TEXT("high"), ENiagaraScalabilityUpdateFrequency::High},
				{TEXT("continuous"), ENiagaraScalabilityUpdateFrequency::Continuous}};
			return Table;
		}

		const TArray<TPair<const TCHAR*, ENiagaraCullReaction>>& CullReactions()
		{
			static const TArray<TPair<const TCHAR*, ENiagaraCullReaction>> Table = {
				{TEXT("kill"), ENiagaraCullReaction::Deactivate}, {TEXT("kill_and_clear"), ENiagaraCullReaction::DeactivateImmediate},
				{TEXT("asleep"), ENiagaraCullReaction::DeactivateResume}, {TEXT("asleep_and_clear"), ENiagaraCullReaction::DeactivateImmediateResume}};
			return Table;
		}

		const TArray<TPair<const TCHAR*, const FNiagaraTypeDefinition*>>& ChannelTypes()
		{
			static const TArray<TPair<const TCHAR*, const FNiagaraTypeDefinition*>> Table = {
				{TEXT("float"), &FNiagaraTypeDefinition::GetFloatDef()}, {TEXT("int32"), &FNiagaraTypeDefinition::GetIntDef()},
				{TEXT("bool"), &FNiagaraTypeDefinition::GetBoolDef()}, {TEXT("vector2"), &FNiagaraTypeDefinition::GetVec2Def()},
				{TEXT("vector"), &FNiagaraTypeDefinition::GetVec3Def()}, {TEXT("vector4"), &FNiagaraTypeDefinition::GetVec4Def()},
				{TEXT("position"), &FNiagaraTypeDefinition::GetPositionDef()}, {TEXT("linear_color"), &FNiagaraTypeDefinition::GetColorDef()},
				{TEXT("quat"), &FNiagaraTypeDefinition::GetQuatDef()}};
			return Table;
		}

		UClass* ChannelClass(const FString& Kind)
		{
			if (Kind == TEXT("global")) return UNiagaraDataChannel_Global::StaticClass();
			if (Kind == TEXT("islands")) return UNiagaraDataChannel_Islands::StaticClass();
			if (Kind == TEXT("gameplay_burst")) return UNiagaraDataChannel_GameplayBurst::StaticClass();
			return nullptr;
		}

		/** "Name:type,..." into validated pairs. */
		bool ParseChannelVariables(const FString& Value, TArray<TPair<FName, const FNiagaraTypeDefinition*>>& Out, FString& OutError)
		{
			TArray<FString> Parts;
			Value.ParseIntoArray(Parts, TEXT(","));
			TSet<FString> Seen;
			if (Parts.IsEmpty() || Parts.Num() > 32)
			{
				OutError = TEXT("value lists 1-32 variables as 'Name:type', comma separated.");
				return false;
			}
			for (const FString& Part : Parts)
			{
				FString Name;
				FString Type;
				const FNiagaraTypeDefinition* Def = nullptr;
				bool bDuplicate = false;
				if (!Part.TrimStartAndEnd().Split(TEXT(":"), &Name, &Type) || !IsIdentifier(Name.TrimStartAndEnd())
					|| !ParseEnum(Type, ChannelTypes(), Def))
				{
					OutError = FString::Printf(TEXT("'%s' must be Name:type with type float, int32, bool, vector2, vector, vector4, position, linear_color or quat."), *Part.Left(64));
					return false;
				}
				Seen.Add(Name.TrimStartAndEnd().ToLower(), &bDuplicate);
				if (bDuplicate)
				{
					OutError = FString::Printf(TEXT("Variable %s is listed twice."), *Name);
					return false;
				}
				Out.Add({FName(*Name.TrimStartAndEnd()), Def});
			}
			return true;
		}

		/** Repeatable only when the System and every enabled emitter use deterministic random streams. */
		bool IsFullyDeterministic(UNiagaraSystem& System)
		{
			if (!System.NeedsDeterminism()) return false;
			for (const FNiagaraEmitterHandle& Handle : System.GetEmitterHandles())
			{
				const FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData();
				if (Handle.GetIsEnabled() && Data && !Data->bDeterminism) return false;
			}
			return true;
		}

		FString ContentKeyOf(const UObject& Asset)
		{
			const UPackage* Package = Asset.GetOutermost();
			FString Canonical = TEXT("hyperai.niagara.capture-key.v1\n") + Asset.GetPathName() + TEXT("\n");
			Canonical += Package ? LexToString(Package->GetSavedHash()) : FString();
			Canonical += Package && Package->IsDirty() ? TEXT("\ndirty") : TEXT("\nclean");
			return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
		}

		UPackage* NewAssetPackage(const FString& ObjectPath)
		{
			return CreatePackage(*FPackageName::ObjectPathToPackageName(ObjectPath));
		}

		void Register(UObject& Asset)
		{
			FAssetRegistryModule::AssetCreated(&Asset);
			Asset.MarkPackageDirty();
		}

		/**
		 * One capture. The component runs in its own preview world with a fixed seed and is advanced by hand, so the
		 * recording does not depend on the editor viewport or frame rate.
		 */
		struct FCapture
		{
			FString Id;
			FString SystemPath;
			FString SystemKey;
			int32 FramesRequested = 0;
			int32 FramesWritten = 0;
			float DeltaSeconds = 1.f / 60.f;
			bool bDeterministic = false;
			FString State = TEXT("capturing");
			FString Error;
			double StartedSeconds = 0.0;
			bool bBaked = false;
			TUniquePtr<FPreviewScene> Scene;
			TStrongObjectPtr<UNiagaraComponent> Component;
			TStrongObjectPtr<UNiagaraSimCache> Cache;

			void ReleaseSimulation()
			{
				if (Component.IsValid())
				{
					Component->DeactivateImmediate();
				}
				Component.Reset();
				Scene.Reset();
			}
		};

		TMap<FString, TSharedRef<FCapture>>& Captures()
		{
			static TMap<FString, TSharedRef<FCapture>> Map;
			return Map;
		}

		void PruneCaptures()
		{
			const double Now = FPlatformTime::Seconds();
			for (auto It = Captures().CreateIterator(); It; ++It)
			{
				if (It.Value()->bBaked || Now - It.Value()->StartedSeconds > CaptureLifetimeSeconds)
				{
					It.Value()->ReleaseSimulation();
					It.RemoveCurrent();
				}
			}
		}

		void FillReport(const FCapture& Capture, FHyperAINiagaraSimCacheReport& Out)
		{
			Out.CaptureId = Capture.Id;
			Out.State = Capture.State;
			Out.FramesRequested = Capture.FramesRequested;
			Out.FramesCaptured = Capture.FramesWritten;
			Out.bDeterministic = Capture.bDeterministic;
			Out.Error = Capture.Error;
			if (Capture.State == TEXT("complete") && Capture.Cache.IsValid())
			{
				const UNiagaraSimCache& Cache = *Capture.Cache.Get();
				const TArray<FName> Emitters = Cache.GetEmitterNames();
				const int32 LastFrame = Cache.GetNumFrames() - 1;
				for (int32 Index = 0; Index < Emitters.Num(); ++Index)
				{
					Out.EmitterNames.Add(Emitters[Index].ToString());
					Out.LastFrameParticleCounts.Add(LastFrame >= 0 ? Cache.GetEmitterNumInstances(Index, LastFrame) : 0);
				}
			}
		}
	}

	bool IsAssetOp(const FString& Kind)
	{
		return Kind == TEXT("set_effect_type") || Kind == TEXT("create_effect_type") || Kind == TEXT("set_effect_type_setting")
			|| Kind == TEXT("create_data_channel") || Kind == TEXT("bake_sim_cache");
	}

	bool ValidateAssetOp(const FHyperAINiagaraEditOp& Op, const FString& SystemPath, const TSet<FString>& PlannedAssets,
		FString& OutStatus, FString& OutDiagnostic)
	{
		OutStatus.Reset();
		OutDiagnostic.Reset();
		if (!Op.EmitterName.IsEmpty() || !Op.ScriptName.IsEmpty() || !Op.ModuleName.IsEmpty() || !Op.InputNameStack.IsEmpty())
		{
			return Fail(OutStatus, OutDiagnostic, TEXT("invalid_op_address"), FString::Printf(TEXT("%s takes no emitter, script or module address."), *Op.Kind));
		}
		if (Op.Kind == TEXT("set_effect_type"))
		{
			if (Op.AssetPath.IsEmpty() || PlannedAssets.Contains(Op.AssetPath)) return true;
			return Cast<UNiagaraEffectType>(FSoftObjectPath(Op.AssetPath).TryLoad()) ? true
				: Fail(OutStatus, OutDiagnostic, TEXT("effect_type_not_found"), FString::Printf(TEXT("No Niagara effect type at %s; leave asset_path empty to clear it."), *Op.AssetPath));
		}
		if (Op.Kind == TEXT("create_effect_type") || Op.Kind == TEXT("create_data_channel") || Op.Kind == TEXT("bake_sim_cache"))
		{
			if (!IsGameObjectPath(Op.AssetPath) || PlannedAssets.Contains(Op.AssetPath) || !IsAbsent(Op.AssetPath))
			{
				return Fail(OutStatus, OutDiagnostic, TEXT("create_target_invalid"),
					FString::Printf(TEXT("%s needs asset_path, a new /Game/Folder/Name.Name path with nothing there yet."), *Op.Kind));
			}
		}
		if (Op.Kind == TEXT("create_effect_type")) return true;
		if (Op.Kind == TEXT("create_data_channel"))
		{
			TArray<TPair<FName, const FNiagaraTypeDefinition*>> Variables;
			FString Error;
			if (!ChannelClass(Op.ValueType))
				return Fail(OutStatus, OutDiagnostic, TEXT("invalid_data_channel_type"), TEXT("value_type must be global, islands or gameplay_burst."));
			return ParseChannelVariables(Op.Value, Variables, Error) ? true
				: Fail(OutStatus, OutDiagnostic, TEXT("invalid_data_channel_variables"), Error);
		}
		if (Op.Kind == TEXT("bake_sim_cache"))
		{
			PruneCaptures();
			const TSharedRef<FCapture>* Capture = Captures().Find(Op.Name);
			if (!Capture || (*Capture)->State != TEXT("complete") || (*Capture)->bBaked)
				return Fail(OutStatus, OutDiagnostic, TEXT("capture_not_ready"), TEXT("name must be the capture_id of a finished, unbaked hyper_niagara_validate sim_cache_capture."));
			const UObject* System = FSoftObjectPath(SystemPath).ResolveObject();
			if ((*Capture)->SystemPath != SystemPath || !System || ContentKeyOf(*System) != (*Capture)->SystemKey)
				return Fail(OutStatus, OutDiagnostic, TEXT("capture_stale"), TEXT("The capture came from a different System, or the System changed since; capture again."));
			return true;
		}
		// set_effect_type_setting
		if (!PlannedAssets.Contains(Op.AssetPath) && !Cast<UNiagaraEffectType>(FSoftObjectPath(Op.AssetPath).TryLoad()))
		{
			return Fail(OutStatus, OutDiagnostic, TEXT("effect_type_not_found"), FString::Printf(TEXT("No Niagara effect type at %s."), *Op.AssetPath));
		}
		float Number = 0.f;
		bool bFlag = false;
		ENiagaraScalabilityUpdateFrequency Frequency = ENiagaraScalabilityUpdateFrequency::SpawnOnly;
		ENiagaraCullReaction Reaction = ENiagaraCullReaction::Deactivate;
		const bool bValid =
			(Op.Name == TEXT("update_frequency") && ParseEnum(Op.Value, UpdateFrequencies(), Frequency))
			|| (Op.Name == TEXT("cull_reaction") && ParseEnum(Op.Value, CullReactions(), Reaction))
			|| (Op.Name == TEXT("allow_culling_for_local_players") && ParseBool(Op.Value, bFlag))
			|| ((Op.Name == TEXT("max_distance") || Op.Name == TEXT("max_instances") || Op.Name == TEXT("max_system_instances"))
				&& ParseFloat(Op.Value, Number) && Number >= 0.f && Number <= 1000000.f);
		return bValid ? true : Fail(OutStatus, OutDiagnostic, TEXT("invalid_effect_type_setting"),
			TEXT("name is update_frequency (spawn_only|low|medium|high|continuous), cull_reaction (kill|kill_and_clear|asleep|asleep_and_clear), allow_culling_for_local_players (true|false), or max_distance / max_instances / max_system_instances (a number; 0 turns that cull off)."));
	}

	bool ApplyAssetOp(UNiagaraSystem& System, const FHyperAINiagaraEditOp& Op, FString& OutStatus, FString& OutError)
	{
		OutStatus = TEXT("applied");
		if (Op.Kind == TEXT("set_effect_type"))
		{
			UNiagaraEffectType* EffectType = Op.AssetPath.IsEmpty() ? nullptr
				: Cast<UNiagaraEffectType>(FSoftObjectPath(Op.AssetPath).ResolveObject());
			if (!Op.AssetPath.IsEmpty() && !EffectType)
			{
				OutError = FString::Printf(TEXT("The effect type %s is not loaded."), *Op.AssetPath);
				return false;
			}
			System.SetEffectType(EffectType);
			OutStatus = EffectType ? TEXT("effect_type_set") : TEXT("effect_type_cleared");
			return true;
		}
		if (Op.Kind == TEXT("create_effect_type"))
		{
			UPackage* Package = NewAssetPackage(Op.AssetPath);
			UNiagaraEffectType* EffectType = Package ? NewObject<UNiagaraEffectType>(Package,
				FName(*FPackageName::ObjectPathToObjectName(Op.AssetPath)), RF_Public | RF_Standalone | RF_Transactional) : nullptr;
			if (!EffectType)
			{
				OutError = TEXT("Could not create the effect type.");
				return false;
			}
			Register(*EffectType);
			OutStatus = TEXT("effect_type_created");
			return true;
		}
		if (Op.Kind == TEXT("set_effect_type_setting"))
		{
			UNiagaraEffectType* EffectType = Cast<UNiagaraEffectType>(FSoftObjectPath(Op.AssetPath).ResolveObject());
			if (!EffectType)
			{
				OutError = FString::Printf(TEXT("The effect type %s is not loaded."), *Op.AssetPath);
				return false;
			}
			EffectType->Modify();
			float Number = 0.f;
			bool bFlag = false;
			if (Op.Name == TEXT("update_frequency")) ParseEnum(Op.Value, UpdateFrequencies(), EffectType->UpdateFrequency);
			else if (Op.Name == TEXT("cull_reaction")) ParseEnum(Op.Value, CullReactions(), EffectType->CullReaction);
			else if (Op.Name == TEXT("allow_culling_for_local_players") && ParseBool(Op.Value, bFlag)) EffectType->bAllowCullingForLocalPlayers = bFlag;
			else if (ParseFloat(Op.Value, Number))
			{
				// Budgets apply to every detail level; an effect type with none gets one covering all platforms.
				TArray<FNiagaraSystemScalabilitySettings>& Levels = EffectType->SystemScalabilitySettings.Settings;
				if (Levels.IsEmpty()) Levels.AddDefaulted();
				for (FNiagaraSystemScalabilitySettings& Level : Levels)
				{
					if (Op.Name == TEXT("max_distance")) { Level.bCullByDistance = Number > 0.f; Level.MaxDistance = Number; }
					else if (Op.Name == TEXT("max_instances")) { Level.bCullMaxInstanceCount = Number > 0.f; Level.MaxInstances = FMath::RoundToInt(Number); }
					else { Level.bCullPerSystemMaxInstanceCount = Number > 0.f; Level.MaxSystemInstances = FMath::RoundToInt(Number); }
				}
			}
			FPropertyChangedEvent Event(nullptr);
			EffectType->PostEditChangeProperty(Event);
			OutStatus = TEXT("effect_type_setting:") + Op.Name;
			return true;
		}
		if (Op.Kind == TEXT("create_data_channel"))
		{
			TArray<TPair<FName, const FNiagaraTypeDefinition*>> Variables;
			UClass* Class = ChannelClass(Op.ValueType);
			if (!Class || !ParseChannelVariables(Op.Value, Variables, OutError)) return false;
			// Both properties are private with no setter; the Details panel writes them through reflection, and so does
			// this, for exactly these two names. The audit test fails if either is renamed or retyped.
			FObjectProperty* ChannelProperty = FindFProperty<FObjectProperty>(UNiagaraDataChannelAsset::StaticClass(), TEXT("DataChannel"));
			FArrayProperty* VariablesProperty = FindFProperty<FArrayProperty>(UNiagaraDataChannel::StaticClass(), TEXT("ChannelVariables"));
			const FStructProperty* Inner = VariablesProperty ? CastField<FStructProperty>(VariablesProperty->Inner) : nullptr;
			if (!ChannelProperty || !Inner || Inner->Struct != FNiagaraDataChannelVariable::StaticStruct())
			{
				OutError = TEXT("This engine build changed the data channel layout; data channels cannot be created here.");
				return false;
			}
			UPackage* Package = NewAssetPackage(Op.AssetPath);
			UNiagaraDataChannelAsset* Asset = Package ? NewObject<UNiagaraDataChannelAsset>(Package,
				FName(*FPackageName::ObjectPathToObjectName(Op.AssetPath)), RF_Public | RF_Standalone | RF_Transactional) : nullptr;
			UNiagaraDataChannel* Channel = Asset ? NewObject<UNiagaraDataChannel>(Asset, Class, NAME_None, RF_Transactional) : nullptr;
			if (!Channel)
			{
				OutError = TEXT("Could not create the data channel.");
				return false;
			}
			ChannelProperty->SetObjectPropertyValue_InContainer(Asset, Channel);
			FScriptArrayHelper Array(VariablesProperty, VariablesProperty->ContainerPtrToValuePtr<void>(Channel));
			for (const TPair<FName, const FNiagaraTypeDefinition*>& Variable : Variables)
			{
				FNiagaraDataChannelVariable* Added = reinterpret_cast<FNiagaraDataChannelVariable*>(Array.GetRawPtr(Array.AddValue()));
				Added->SetType(FNiagaraDataChannelVariable::ToDataChannelType(*Variable.Value));
				Added->SetName(Variable.Key);
			}
			FPropertyChangedEvent ChannelEvent(VariablesProperty);
			Channel->PostEditChangeProperty(ChannelEvent);
			FPropertyChangedEvent AssetEvent(ChannelProperty);
			Asset->PostEditChangeProperty(AssetEvent);
			Register(*Asset);
			OutStatus = FString::Printf(TEXT("data_channel_created:%d"), Variables.Num());
			return true;
		}
		if (Op.Kind == TEXT("bake_sim_cache"))
		{
			const TSharedRef<FCapture>* Capture = Captures().Find(Op.Name);
			if (!Capture || (*Capture)->State != TEXT("complete") || (*Capture)->bBaked || !(*Capture)->Cache.IsValid())
			{
				OutError = TEXT("The capture is gone or already baked; capture again.");
				return false;
			}
			UPackage* Package = NewAssetPackage(Op.AssetPath);
			UNiagaraSimCache* Cache = (*Capture)->Cache.Get();
			if (!Package || !Cache->Rename(*FPackageName::ObjectPathToObjectName(Op.AssetPath), Package, REN_DontCreateRedirectors | REN_NonTransactional))
			{
				OutError = TEXT("Could not move the capture into its package.");
				return false;
			}
			Cache->ClearFlags(RF_Transient);
			Cache->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
			Register(*Cache);
			(*Capture)->bBaked = true;
			OutStatus = FString::Printf(TEXT("sim_cache_baked:%d_frames"), Cache->GetNumFrames());
			return true;
		}
		OutError = TEXT("Unsupported asset op.");
		return false;
	}

	TArray<FString> SideAssetPaths(const TArray<FHyperAINiagaraEditOp>& Ops)
	{
		TArray<FString> Out;
		for (const FHyperAINiagaraEditOp& Op : Ops)
		{
			if (IsAssetOp(Op.Kind) && Op.Kind != TEXT("set_effect_type") && !Op.AssetPath.IsEmpty())
			{
				Out.AddUnique(Op.AssetPath);
			}
		}
		return Out;
	}

	bool DescribeAsset(const UObject& Asset, FHyperAINiagaraAssetRecord& OutRecord)
	{
		OutRecord.Path = Asset.GetPathName();
		auto Add = [&OutRecord](const TCHAR* Key, const FString& Value) { OutRecord.Details.Add({Key, Value}); };
		if (const UNiagaraEffectType* EffectType = Cast<UNiagaraEffectType>(&Asset))
		{
			OutRecord.Kind = TEXT("effect_type");
			Add(TEXT("update_frequency"), EnumName(EffectType->UpdateFrequency, UpdateFrequencies()));
			Add(TEXT("cull_reaction"), EnumName(EffectType->CullReaction, CullReactions()));
			Add(TEXT("allow_culling_for_local_players"), EffectType->bAllowCullingForLocalPlayers ? TEXT("true") : TEXT("false"));
			const TArray<FNiagaraSystemScalabilitySettings>& Levels = EffectType->GetSystemScalabilitySettings().Settings;
			Add(TEXT("detail_levels"), FString::FromInt(Levels.Num()));
			if (!Levels.IsEmpty())
			{
				Add(TEXT("max_distance"), Levels[0].bCullByDistance ? FString::SanitizeFloat(Levels[0].MaxDistance) : TEXT("off"));
				Add(TEXT("max_instances"), Levels[0].bCullMaxInstanceCount ? FString::FromInt(Levels[0].MaxInstances) : TEXT("off"));
				Add(TEXT("max_system_instances"), Levels[0].bCullPerSystemMaxInstanceCount ? FString::FromInt(Levels[0].MaxSystemInstances) : TEXT("off"));
			}
			return true;
		}
		if (const UNiagaraDataChannelAsset* Channel = Cast<UNiagaraDataChannelAsset>(&Asset))
		{
			OutRecord.Kind = TEXT("data_channel");
			const UNiagaraDataChannel* Data = Channel->Get();
			Add(TEXT("type"), !Data ? TEXT("none") : Data->IsA<UNiagaraDataChannel_Global>() ? TEXT("global")
				: Data->IsA<UNiagaraDataChannel_Islands>() ? TEXT("islands")
				: Data->IsA<UNiagaraDataChannel_GameplayBurst>() ? TEXT("gameplay_burst") : Data->GetClass()->GetName());
			if (Data)
			{
				for (const FNiagaraDataChannelVariable& Variable : Data->GetVariables())
				{
					Add(TEXT("variable"), Variable.GetName().ToString() + TEXT(":") + Variable.GetType().GetName());
				}
			}
			return true;
		}
		if (const UNiagaraSimCache* Cache = Cast<UNiagaraSimCache>(&Asset))
		{
			OutRecord.Kind = TEXT("sim_cache");
			Add(TEXT("frames"), FString::FromInt(Cache->GetNumFrames()));
			for (const FName Emitter : Cache->GetEmitterNames())
			{
				Add(TEXT("emitter"), Emitter.ToString());
			}
			return true;
		}
		return false;
	}

	FString GetEffectTypePath(const UNiagaraSystem& System)
	{
		const UNiagaraEffectType* EffectType = System.GetEffectType();
		return EffectType ? EffectType->GetPathName() : FString();
	}

	FHyperAINiagaraValidateReport RunCapturePolicy(const FHyperAINiagaraValidateRequest& Request)
	{
		FHyperAINiagaraValidateReport Report;
		Report.Policy = Request.Policy;
		auto Reject = [&Report](const TCHAR* Status, const FString& Diagnostic)
		{
			Report.bOk = false;
			Report.Status = Status;
			Report.Diagnostic = Diagnostic;
			return Report;
		};
		PruneCaptures();
		if (!Request.CaptureId.IsEmpty())
		{
			const TSharedRef<FCapture>* Found = Captures().Find(Request.CaptureId);
			if (!Found) return Reject(TEXT("capture_not_found"), TEXT("No capture with that id; captures expire after 10 minutes or once baked."));
			const FCapture& Capture = **Found;
			FillReport(Capture, Report.SimCache);
			Report.bOk = Capture.State != TEXT("failed");
			Report.Status = Capture.State == TEXT("complete") ? TEXT("capture_complete") : Capture.State == TEXT("failed") ? TEXT("capture_failed") : TEXT("capturing");
			Report.Diagnostic = Capture.State == TEXT("complete")
				? TEXT("Bake it with an apply_plan bake_sim_cache op to make a golden cache, or pass golden_sim_cache_path to compare.")
				: Capture.Error;
			if (Capture.State != TEXT("complete") || Request.GoldenSimCachePath.IsEmpty())
			{
				return Report;
			}
			const UNiagaraSimCache* Golden = Cast<UNiagaraSimCache>(FSoftObjectPath(Request.GoldenSimCachePath).TryLoad());
			if (!Golden) return Reject(TEXT("golden_not_found"), FString::Printf(TEXT("No Niagara sim cache at %s."), *Request.GoldenSimCachePath));
			// Experimental in UE 5.8; kept here so an API change stays in this file.
			FNiagaraSimCacheCompare Compare;
			Compare.DefaultFloatTolerance = FMath::Clamp(Request.FloatTolerance, 0.f, 10.f);
			Compare.bExcludeNoneDeterministicVariables = true;
			Compare.bIncludeDataInterfaces = false;
			FString Differences;
			Report.SimCache.bCompared = true;
			Report.SimCache.GoldenPath = Request.GoldenSimCachePath;
			Report.SimCache.Tolerance = Compare.DefaultFloatTolerance;
			Report.SimCache.bMatch = Compare.Compare(*Golden, *Capture.Cache.Get(), Differences);
			TArray<FString> Lines;
			Differences.ParseIntoArrayLines(Lines);
			Lines.SetNum(FMath::Min(Lines.Num(), 32));
			for (FString& Line : Lines) Line = Line.Left(256);
			Report.SimCache.Differences = MoveTemp(Lines);
			Report.bValid = Report.SimCache.bMatch;
			Report.Status = Report.SimCache.bMatch ? TEXT("matches_golden") : TEXT("differs_from_golden");
			Report.Diagnostic = Report.SimCache.bMatch
				? TEXT("The simulation matches the golden cache within tolerance.")
				: FString(TEXT("The simulation differs from the golden cache.")) + (Capture.bDeterministic ? TEXT("")
					: TEXT(" The System is not fully deterministic, so run-to-run noise can cause this; enable Determinism on the System and on every emitter."));
			return Report;
		}

		UNiagaraSystem* System = Cast<UNiagaraSystem>(FSoftObjectPath(Request.TargetPath).ResolveObject());
		if (!System) return Reject(TEXT("target_not_loaded"), TEXT("The System must be loaded; inspect it first."));
		if (System->HasAnyGPUEmitters())
			return Reject(TEXT("gpu_sim_capture_unsupported"), TEXT("GPU emitters need a readback that does not fit a bounded editor tick; only CPU systems can be captured."));
		if (System->HasOutstandingCompilationRequests())
			return Reject(TEXT("compile_in_progress"), TEXT("The System is still compiling; capture once it finishes."));
		if (Captures().Num() >= MaxCaptures)
			return Reject(TEXT("too_many_captures"), FString::Printf(TEXT("At most %d captures can be open; bake or wait for older ones to expire."), MaxCaptures));
		const int32 Frames = FMath::Clamp(Request.CaptureFrames, 1, 120);
		const float Delta = FMath::Clamp(Request.CaptureDeltaSeconds, 1.f / 240.f, 1.f / 15.f);

		const TSharedRef<FCapture> Capture = MakeShared<FCapture>();
		Capture->Id = TEXT("capture-") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12).ToLower();
		Capture->SystemPath = Request.TargetPath;
		Capture->SystemKey = ContentKeyOf(*System);
		Capture->FramesRequested = Frames;
		Capture->DeltaSeconds = Delta;
		Capture->bDeterministic = IsFullyDeterministic(*System);
		Capture->StartedSeconds = FPlatformTime::Seconds();
		Capture->Scene = MakeUnique<FPreviewScene>(FPreviewScene::ConstructionValues());
		Capture->Component.Reset(NewObject<UNiagaraComponent>(GetTransientPackage(), NAME_None, RF_Transient));
		Capture->Component->SetAutoActivate(false);
		Capture->Component->SetAsset(System);
		Capture->Component->SetForceSolo(true);
		Capture->Component->SetRandomSeedOffset(0);
		Capture->Scene->AddComponent(Capture->Component.Get(), FTransform::Identity);
		Capture->Component->Activate(/*bReset=*/true);
		Capture->Cache.Reset(NewObject<UNiagaraSimCache>(GetTransientPackage(), NAME_None, RF_Transient));
		FNiagaraSimCacheCreateParameters Parameters;
		if (!Capture->Cache->BeginWrite(Parameters, Capture->Component.Get()))
		{
			Capture->ReleaseSimulation();
			return Reject(TEXT("capture_not_started"), TEXT("The sim cache refused to start recording this System."));
		}
		Captures().Add(Capture->Id, Capture);

		// Frames are advanced and recorded a few at a time so no poll exceeds the job host's budget.
		FHyperAIStudioAsyncJobRequest Job;
		Job.PackId = FHyperAIStudioNiagaraContracts::PackId;
		Job.ToolName = TEXT("hyper_niagara_validate");
		Job.Target = Request.TargetPath;
		Job.DeadlineMs = 5 * 60 * 1000;
		Job.PollIntervalMs = 50;
		FString JobId;
		FString JobError;
		TWeakPtr<FCapture> Weak = Capture;
		FHyperAIStudioAsyncJobHost::Start(Job, [Weak](FString& OutProgress, FString& OutDiagnostic)
		{
			const TSharedPtr<FCapture> Pinned = Weak.Pin();
			if (!Pinned || !Pinned->Component.IsValid() || !Pinned->Cache.IsValid())
			{
				OutDiagnostic = TEXT("The capture was discarded.");
				return EHyperAIStudioAsyncJobPoll::Failed;
			}
			const double Started = FPlatformTime::Seconds();
			while (Pinned->FramesWritten < Pinned->FramesRequested && FPlatformTime::Seconds() - Started < FrameBudgetSeconds)
			{
				Pinned->Component->AdvanceSimulation(1, Pinned->DeltaSeconds);
				if (!Pinned->Cache->WriteFrame(Pinned->Component.Get()))
				{
					Pinned->State = TEXT("failed");
					Pinned->Error = FString::Printf(TEXT("Recording frame %d failed."), Pinned->FramesWritten);
					Pinned->ReleaseSimulation();
					OutDiagnostic = Pinned->Error;
					return EHyperAIStudioAsyncJobPoll::Failed;
				}
				++Pinned->FramesWritten;
			}
			if (Pinned->FramesWritten < Pinned->FramesRequested)
			{
				OutProgress = FString::Printf(TEXT("%d/%d frames"), Pinned->FramesWritten, Pinned->FramesRequested);
				return EHyperAIStudioAsyncJobPoll::Running;
			}
			Pinned->Cache->EndWrite();
			Pinned->State = TEXT("complete");
			Pinned->ReleaseSimulation();
			OutDiagnostic = FString::Printf(TEXT("Captured %d frames as %s."), Pinned->FramesWritten, *Pinned->Id);
			return EHyperAIStudioAsyncJobPoll::Completed;
		}, JobId, JobError);
		if (!JobError.IsEmpty())
		{
			Capture->State = TEXT("failed");
			Capture->Error = JobError;
			Capture->ReleaseSimulation();
		}
		FillReport(*Capture, Report.SimCache);
		Report.bOk = JobError.IsEmpty();
		Report.Status = JobError.IsEmpty() ? TEXT("capture_started") : TEXT("capture_not_started");
		Report.Diagnostic = JobError.IsEmpty()
			? TEXT("Recording in the background. Call hyper_niagara_validate again with policy sim_cache_capture and this capture_id until it is complete.")
			: JobError;
		return Report;
	}

	FString GetCaptureSystemKey(const FString& CaptureId)
	{
		const TSharedRef<FCapture>* Capture = Captures().Find(CaptureId);
		return Capture ? (*Capture)->SystemKey : FString();
	}
}
