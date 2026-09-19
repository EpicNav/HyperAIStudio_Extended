// Games by Hyper 2026.

// The only translation unit in the plugin that includes NiagaraExternalSystemEditorUtilities.h. Epic marks that
// header "EXPERIMENTAL! DO NOT USE EXCEPT FOR TESTING!", and its one in-engine caller is Epic's own NiagaraToolsets
// plugin (NiagaraToolset_System.cpp). Keep every FNiagaraExt_* type inside this file.

#include "HyperAIStudioNiagaraExternalEditGate.h"

#include "Misc/EngineVersionComparison.h"
#include "NiagaraEmitter.h"
#include "NiagaraExternalSystemEditorUtilities.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"
#include "ScopedTransaction.h"
#include "HyperAIStudioNiagaraAssetsGate.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/SoftObjectPath.h"

#include <type_traits>

#define LOCTEXT_NAMESPACE "HyperAIStudioNiagaraExternalEditGate"

static_assert(ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION == 8,
	"The Niagara external-edit gate is pinned to the audited UE 5.8 UNiagaraExternalEditUtilities API. "
	"Re-audit NiagaraExternalSystemEditorUtilities.h before raising this pin.");

// A silent signature change in the experimental API becomes a compile error here instead of a runtime surprise.
static_assert(std::is_same_v<decltype(&UNiagaraExternalEditUtilities::GetSystemSummary),
	void (*)(UNiagaraSystem*, FNiagaraExt_SystemSummary&, FNiagaraExternalEditContext&)>);
static_assert(std::is_same_v<decltype(&UNiagaraExternalEditUtilities::GetEmitterTopology),
	void (*)(const FNiagaraExt_StackItemReference&, FNiagaraExt_EmitterTopology&, FNiagaraExternalEditContext&)>);
static_assert(std::is_same_v<decltype(&UNiagaraExternalEditUtilities::GetSystemCompileState),
	void (*)(UNiagaraSystem*, FNiagaraExt_SystemCompileState&, FNiagaraExternalEditContext&)>);
static_assert(std::is_same_v<decltype(&UNiagaraExternalEditUtilities::GetStackIssues),
	void (*)(UNiagaraSystem*, FNiagaraExt_StackIssues&, FNiagaraExternalEditContext&)>);
static_assert(std::is_same_v<decltype(&UNiagaraExternalEditUtilities::SetModuleEnabled),
	void (*)(const FNiagaraExt_StackItemReference&, bool, FNiagaraExternalEditContext&)>);
static_assert(std::is_same_v<decltype(&UNiagaraExternalEditUtilities::AddModule),
	void (*)(const FNiagaraExt_StackItemReference&, const UNiagaraScript*, FNiagaraExt_ModuleTopology&, FNiagaraExternalEditContext&)>);
static_assert(std::is_same_v<decltype(&UNiagaraExternalEditUtilities::AddRenderer),
	void (*)(const FNiagaraExt_StackItemReference&, const TSubclassOf<UNiagaraRendererProperties>, FNiagaraExt_RendererRef&, FNiagaraExternalEditContext&)>);
static_assert(std::is_same_v<decltype(&UNiagaraExternalEditUtilities::AddEmitter),
	void (*)(UNiagaraEmitter*, FName, FNiagaraExt_EmitterTopology&, FNiagaraExternalEditContext&)>);
static_assert(std::is_same_v<decltype(&UNiagaraExternalEditUtilities::SetStackInputData),
	void (*)(const FNiagaraExt_StackItemReference&, const FNiagaraExt_StackInputValue&, FNiagaraExternalEditContext&)>);
static_assert(std::is_same_v<decltype(&UNiagaraExternalEditUtilities::ApplyStackIssueFix),
	void (*)(UNiagaraSystem*, const FString&, const FString&, FNiagaraExt_ApplyStackIssueFixResult&, FNiagaraExternalEditContext&)>);

namespace HyperAIStudio::Niagara::ExternalEditGate
{
	namespace
	{
		constexpr int32 MaxNameChars = 128;
		constexpr int32 MaxPathChars = 512;
		constexpr int32 MaxIdChars = 256;
		constexpr int32 MaxSummaryChars = 512;
		constexpr int32 MaxDescriptionChars = 1024;

		const TSet<FString>& ScriptNames()
		{
			static const TSet<FString> Names = {
				TEXT("SystemSpawnScript"), TEXT("SystemUpdateScript"),
				TEXT("EmitterSpawnScript"), TEXT("EmitterUpdateScript"),
				TEXT("ParticleSpawnScript"), TEXT("ParticleUpdateScript")};
			return Names;
		}

		bool IsSystemScript(const FString& ScriptName)
		{
			return ScriptName.StartsWith(TEXT("System"), ESearchCase::CaseSensitive);
		}

		bool IsBoundedText(const FString& Value, int32 MaxChars)
		{
			if (Value.Len() > MaxChars)
			{
				return false;
			}
			for (const TCHAR Char : Value)
			{
				if (Char < TEXT(' ') || Char == 0x7F)
				{
					return false;
				}
			}
			return true;
		}

		FName ToName(const FString& Value)
		{
			return Value.IsEmpty() ? NAME_None : FName(*Value);
		}

		FString JoinErrors(const FNiagaraExternalEditContext& Context, int32 FromIndex)
		{
			FString Joined;
			for (int32 Index = FromIndex; Index < Context.Errors.Num(); ++Index)
			{
				if (!Joined.IsEmpty())
				{
					Joined += TEXT(" | ");
				}
				Joined += Context.Errors[Index].ToString();
			}
			return Joined.Left(MaxDescriptionChars);
		}

		bool ParseFloats(const FString& Text, int32 Count, float* Out)
		{
			TArray<FString> Parts;
			Text.ParseIntoArray(Parts, TEXT(","), false);
			if (Parts.Num() != Count)
			{
				return false;
			}
			for (int32 Index = 0; Index < Count; ++Index)
			{
				const FString Part = Parts[Index].TrimStartAndEnd();
				if (Part.IsEmpty() || !Part.IsNumeric())
				{
					return false;
				}
				Out[Index] = FCString::Atof(*Part);
			}
			return true;
		}

		/** Local values ride in the instanced struct as the plain Niagara or core type the input already uses. */
		bool BuildInputValue(const FString& ValueType, const FString& Text, FNiagaraExt_StackInputValue& OutValue)
		{
			const FString Trimmed = Text.TrimStartAndEnd();
			if (ValueType == TEXT("float"))
			{
				FNiagaraFloat Value;
				if (!ParseFloats(Trimmed, 1, &Value.Value))
				{
					return false;
				}
				const UScriptStruct* Struct = FindObject<UScriptStruct>(nullptr, TEXT("/Script/Niagara.NiagaraFloat"));
				OutValue.InitializeAs(Struct, reinterpret_cast<const uint8*>(&Value));
				return Struct != nullptr;
			}
			if (ValueType == TEXT("int32"))
			{
				if (Trimmed.IsEmpty() || !Trimmed.IsNumeric() || Trimmed.Contains(TEXT(".")))
				{
					return false;
				}
				FNiagaraInt32 Value;
				Value.Value = FCString::Atoi(*Trimmed);
				const UScriptStruct* Struct = FindObject<UScriptStruct>(nullptr, TEXT("/Script/Niagara.NiagaraInt32"));
				OutValue.InitializeAs(Struct, reinterpret_cast<const uint8*>(&Value));
				return Struct != nullptr;
			}
			if (ValueType == TEXT("bool"))
			{
				if (Trimmed != TEXT("true") && Trimmed != TEXT("false"))
				{
					return false;
				}
				FNiagaraBool Value;
				Value.SetValue(Trimmed == TEXT("true"));
				const UScriptStruct* Struct = FindObject<UScriptStruct>(nullptr, TEXT("/Script/Niagara.NiagaraBool"));
				OutValue.InitializeAs(Struct, reinterpret_cast<const uint8*>(&Value));
				return Struct != nullptr;
			}
			if (ValueType == TEXT("vector"))
			{
				FVector3f Value;
				if (!ParseFloats(Trimmed, 3, &Value.X))
				{
					return false;
				}
				OutValue.InitializeAs(TVariantStructure<FVector3f>::Get(), reinterpret_cast<const uint8*>(&Value));
				return true;
			}
			if (ValueType == TEXT("color"))
			{
				FLinearColor Value;
				if (!ParseFloats(Trimmed, 4, &Value.R))
				{
					return false;
				}
				OutValue.InitializeAs(TBaseStructure<FLinearColor>::Get(), reinterpret_cast<const uint8*>(&Value));
				return true;
			}
			return false;
		}

		FNiagaraExt_StackItemReference MakeReference(UNiagaraSystem& System, const FHyperAINiagaraEditOp& Op)
		{
			FNiagaraExt_StackItemReference Reference(
				&System, ToName(Op.EmitterName), ToName(Op.ScriptName), ToName(Op.ModuleName));
			for (const FString& InputName : Op.InputNameStack)
			{
				Reference.InputNameStack.Add(ToName(InputName));
			}
			return Reference;
		}

		FString StackSeverity(ENiagaraExt_StackIssueSeverity Severity)
		{
			switch (Severity)
			{
			case ENiagaraExt_StackIssueSeverity::Error: return TEXT("error");
			case ENiagaraExt_StackIssueSeverity::Warning: return TEXT("warning");
			default: return TEXT("info");
			}
		}

		void CopyStack(const FNiagaraExt_ScriptStackTopology& Source, FHyperAINiagaraScriptStackTopology& Out, int32& ModuleBudget, bool& bTruncated)
		{
			Out.ScriptName = Source.ScriptName.ToString();
			for (const FNiagaraExt_ModuleTopology& Module : Source.Modules)
			{
				if (ModuleBudget <= 0)
				{
					bTruncated = true;
					return;
				}
				--ModuleBudget;
				FHyperAINiagaraModuleTopology& Entry = Out.Modules.AddDefaulted_GetRef();
				Entry.ModuleName = Module.ModuleName.ToString();
				Entry.bEnabled = Module.Enabled;
				Entry.bIsSetParametersModule = Module.bIsSetParametersModule;
				Entry.ScriptAssetPath = Module.ModuleScript ? Module.ModuleScript->GetPathName() : FString();
				for (const FNiagaraExt_StackInputTopology& Input : Module.Inputs)
				{
					if (Entry.InputNames.Num() >= MaxInputNamesPerModule)
					{
						bTruncated = true;
						break;
					}
					Entry.InputNames.Add(Input.Name.ToString());
				}
			}
		}
	}

	bool IsApiAvailable()
	{
		return UNiagaraExternalEditUtilities::StaticClass() != nullptr;
	}

	bool ReadTopology(
		UNiagaraSystem& System,
		TArray<FHyperAINiagaraEmitterTopology>& OutEmitters,
		bool& bOutTruncated,
		FString& OutError)
	{
		OutEmitters.Reset();
		bOutTruncated = false;
		OutError.Reset();
		check(IsInGameThread());

		FNiagaraExternalEditContext Context(&System);
		FNiagaraExt_SystemSummary Summary;
		UNiagaraExternalEditUtilities::GetSystemSummary(&System, Summary, Context);
		if (Context.HasErrors())
		{
			OutError = JoinErrors(Context, 0);
			return false;
		}

		int32 ModuleBudget = MaxTopologyModules;
		for (const FNiagaraExt_EmitterSummary& Emitter : Summary.Emitters)
		{
			if (OutEmitters.Num() >= FHyperAIStudioNiagaraContracts::MaxEmitters)
			{
				bOutTruncated = true;
				break;
			}
			FNiagaraExt_EmitterTopology Topology;
			UNiagaraExternalEditUtilities::GetEmitterTopology(
				FNiagaraExt_StackItemReference(&System, Emitter.EmitterName), Topology, Context);
			if (Context.HasErrors())
			{
				OutError = JoinErrors(Context, 0);
				return false;
			}

			FHyperAINiagaraEmitterTopology& Out = OutEmitters.AddDefaulted_GetRef();
			Out.EmitterName = Topology.EmitterName.ToString();
			Out.bEnabled = Topology.bEnabled;
			Out.SimTarget = Topology.SimTarget == ENiagaraSimTarget::GPUComputeSim ? TEXT("GPUComputeSim") : TEXT("CPUSim");
			for (const FNiagaraExt_ScriptStackTopology* Stack : {
				&Topology.EmitterSpawnScript, &Topology.EmitterUpdateScript,
				&Topology.ParticleSpawnScript, &Topology.ParticleUpdateScript})
			{
				CopyStack(*Stack, Out.ScriptStacks.AddDefaulted_GetRef(), ModuleBudget, bOutTruncated);
			}
			for (const FNiagaraExt_RendererRef& Renderer : Topology.Renderers)
			{
				FHyperAINiagaraRendererTopology& Entry = Out.Renderers.AddDefaulted_GetRef();
				Entry.RendererIndex = Renderer.RendererIndex;
				Entry.RendererClassPath = Renderer.RendererClass ? Renderer.RendererClass->GetPathName() : FString();
			}
		}
		return true;
	}

	bool ReadDiagnostics(UNiagaraSystem& System, int32 MaxIssues, FDiagnostics& OutDiagnostics, FString& OutError)
	{
		OutDiagnostics = FDiagnostics();
		OutError.Reset();
		check(IsInGameThread());

		FNiagaraExternalEditContext Context(&System);
		FNiagaraExt_SystemCompileState Compile;
		UNiagaraExternalEditUtilities::GetSystemCompileState(&System, Compile, Context);
		FNiagaraExt_StackIssues Stack;
		UNiagaraExternalEditUtilities::GetStackIssues(&System, Stack, Context);
		if (Context.HasErrors())
		{
			OutError = JoinErrors(Context, 0);
			return false;
		}

		OutDiagnostics.bCompileStateKnown = Compile.AggregateStatus != ENiagaraExt_ScriptCompileStatus::Unknown;
		OutDiagnostics.bCompiling = Compile.bIsCompiling || Compile.bIsStale;
		OutDiagnostics.bCompileHasErrors = Compile.bHasErrors;
		OutDiagnostics.bCompileHasWarnings = Compile.bHasWarnings;

		auto AddIssue = [&](FHyperAINiagaraIssue&& Issue)
		{
			int32& Counter = Issue.Severity == TEXT("error") ? OutDiagnostics.ErrorCount
				: Issue.Severity == TEXT("warning") ? OutDiagnostics.WarningCount : OutDiagnostics.InfoCount;
			++Counter;
			if (OutDiagnostics.Issues.Num() >= MaxIssues)
			{
				OutDiagnostics.bTruncated = true;
				return;
			}
			Issue.Summary.LeftInline(MaxSummaryChars);
			Issue.Description.LeftInline(MaxDescriptionChars);
			OutDiagnostics.Issues.Add(MoveTemp(Issue));
		};

		for (const FNiagaraExt_StackIssue& StackIssue : Stack.Issues)
		{
			if (StackIssue.bIsDismissed)
			{
				continue;
			}
			if (StackIssue.Severity == ENiagaraExt_StackIssueSeverity::Error)
			{
				++OutDiagnostics.StackErrorCount;
			}
			FHyperAINiagaraIssue Issue;
			Issue.Code = TEXT("stack_issue");
			Issue.Severity = StackSeverity(StackIssue.Severity);
			Issue.StableId = StackIssue.IssueId.Left(MaxIdChars);
			Issue.SourcePath = StackIssue.StackDisplayPath.Left(MaxPathChars);
			Issue.Summary = StackIssue.ShortDescription;
			Issue.Description = StackIssue.LongDescription;
			for (const FNiagaraExt_StackIssueFix& Fix : StackIssue.Fixes)
			{
				// Link-style fixes are navigation hints for a person; ApplyStackIssueFix refuses them.
				if (Fix.Style == ENiagaraExt_StackIssueFixStyle::Fix)
				{
					Issue.FixIds.Add(Fix.FixId.Left(MaxIdChars));
				}
			}
			AddIssue(MoveTemp(Issue));
		}

		for (const FNiagaraExt_ScriptCompileInfo& Script : Compile.Scripts)
		{
			for (const FNiagaraExt_CompileEvent& Event : Script.CompileEvents)
			{
				if (Event.Severity != ENiagaraExt_CompileEventSeverity::Error
					&& Event.Severity != ENiagaraExt_CompileEventSeverity::Warning)
				{
					continue;
				}
				FHyperAINiagaraIssue Issue;
				Issue.Code = TEXT("compile_event");
				Issue.Severity = Event.Severity == ENiagaraExt_CompileEventSeverity::Error ? TEXT("error") : TEXT("warning");
				Issue.SourcePath = FString::Printf(TEXT("%s/%s"), *Script.EmitterName.ToString(), *Script.ScriptName.ToString()).Left(MaxPathChars);
				Issue.Summary = Event.ShortDescription.IsEmpty() ? Event.Message : Event.ShortDescription;
				Issue.Description = Event.Message;
				AddIssue(MoveTemp(Issue));
			}
		}
		return true;
	}

	bool ValidateOp(const FHyperAINiagaraEditOp& Op, bool bResolveAssets, FString& OutStatus, FString& OutDiagnostic)
	{
		OutStatus.Reset();
		OutDiagnostic.Reset();
		auto Fail = [&](const TCHAR* Status, const FString& Diagnostic)
		{
			OutStatus = Status;
			OutDiagnostic = Diagnostic;
			return false;
		};

		if (!IsBoundedText(Op.EmitterName, MaxNameChars) || !IsBoundedText(Op.ScriptName, MaxNameChars)
			|| !IsBoundedText(Op.ModuleName, MaxNameChars) || !IsBoundedText(Op.Name, MaxNameChars)
			|| !IsBoundedText(Op.AssetPath, MaxPathChars) || !IsBoundedText(Op.IssueId, MaxIdChars)
			|| !IsBoundedText(Op.FixId, MaxIdChars) || !IsBoundedText(Op.ValueType, MaxNameChars)
			|| !IsBoundedText(Op.Value, MaxNameChars) || Op.InputNameStack.Num() > MaxInputNameDepth)
		{
			return Fail(TEXT("invalid_op_text"), TEXT("An op field is too long, contains control characters, or nests inputs too deeply."));
		}
		for (const FString& InputName : Op.InputNameStack)
		{
			if (InputName.IsEmpty() || !IsBoundedText(InputName, MaxNameChars))
			{
				return Fail(TEXT("invalid_op_text"), TEXT("input_name_stack entries must be non-empty, bounded names."));
			}
		}

		const bool bHasScript = !Op.ScriptName.IsEmpty();
		if (bHasScript && !ScriptNames().Contains(Op.ScriptName))
		{
			return Fail(TEXT("invalid_script_name"), TEXT("script_name must be one of SystemSpawnScript, SystemUpdateScript, EmitterSpawnScript, EmitterUpdateScript, ParticleSpawnScript, ParticleUpdateScript."));
		}
		// Emitter scripts need their emitter; system scripts must not name one.
		const bool bEmitterScopeOk = !bHasScript || (IsSystemScript(Op.ScriptName) == Op.EmitterName.IsEmpty());

		if (Op.Kind == TEXT("set_module_enabled"))
		{
			if (!bHasScript || !bEmitterScopeOk || Op.ModuleName.IsEmpty())
			{
				return Fail(TEXT("invalid_op_address"), TEXT("set_module_enabled needs script_name, module_name, and emitter_name for emitter scripts."));
			}
			return true;
		}
		if (Op.Kind == TEXT("add_module"))
		{
			if (!bHasScript || !bEmitterScopeOk || Op.AssetPath.IsEmpty())
			{
				return Fail(TEXT("invalid_op_address"), TEXT("add_module needs script_name, asset_path of a Niagara module script, and emitter_name for emitter scripts."));
			}
			if (bResolveAssets && !Cast<UNiagaraScript>(FSoftObjectPath(Op.AssetPath).TryLoad()))
			{
				return Fail(TEXT("module_script_not_found"), FString::Printf(TEXT("No Niagara module script at %s."), *Op.AssetPath));
			}
			return true;
		}
		if (Op.Kind == TEXT("add_renderer"))
		{
			if (Op.EmitterName.IsEmpty() || bHasScript || Op.AssetPath.IsEmpty())
			{
				return Fail(TEXT("invalid_op_address"), TEXT("add_renderer needs emitter_name and asset_path of a renderer class, and no script_name."));
			}
			if (bResolveAssets)
			{
				const UClass* RendererClass = FSoftClassPath(Op.AssetPath).TryLoadClass<UNiagaraRendererProperties>();
				if (!RendererClass || RendererClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated))
				{
					return Fail(TEXT("renderer_class_not_found"), FString::Printf(TEXT("%s is not a concrete Niagara renderer class."), *Op.AssetPath));
				}
			}
			return true;
		}
		if (Op.Kind == TEXT("add_emitter"))
		{
			if (Op.AssetPath.IsEmpty() || Op.Name.IsEmpty() || bHasScript || !Op.EmitterName.IsEmpty())
			{
				return Fail(TEXT("invalid_op_address"), TEXT("add_emitter needs asset_path of an emitter template and a new name, and no emitter or script address."));
			}
			if (bResolveAssets && !Cast<UNiagaraEmitter>(FSoftObjectPath(Op.AssetPath).TryLoad()))
			{
				return Fail(TEXT("emitter_template_not_found"), FString::Printf(TEXT("No Niagara emitter at %s."), *Op.AssetPath));
			}
			return true;
		}
		if (Op.Kind == TEXT("set_input_value"))
		{
			if (!bHasScript || !bEmitterScopeOk || Op.ModuleName.IsEmpty() || Op.InputNameStack.IsEmpty())
			{
				return Fail(TEXT("invalid_op_address"), TEXT("set_input_value needs script_name, module_name, input_name_stack, and emitter_name for emitter scripts."));
			}
			FNiagaraExt_StackInputValue Parsed;
			if (!BuildInputValue(Op.ValueType, Op.Value, Parsed))
			{
				return Fail(TEXT("invalid_input_value"), TEXT("value_type must be float, int32, bool, vector (x,y,z) or color (r,g,b,a), and value must parse as that type."));
			}
			return true;
		}
		if (Op.Kind == TEXT("apply_stack_issue_fix"))
		{
			if (Op.IssueId.IsEmpty() || Op.FixId.IsEmpty())
			{
				return Fail(TEXT("invalid_op_address"), TEXT("apply_stack_issue_fix needs issue_id and fix_id from hyper_niagara_validate."));
			}
			return true;
		}
		return Fail(TEXT("unsupported_op_kind"), TEXT("kind must be set_module_enabled, add_module, add_renderer, add_emitter, set_input_value, or apply_stack_issue_fix."));
	}

	bool ApplyOps(
		UNiagaraSystem& System,
		const TArray<FHyperAINiagaraEditOp>& Ops,
		int32& OutAppliedCount,
		TArray<FString>& OutPerOpStatus,
		FString& OutError)
	{
		OutAppliedCount = 0;
		OutPerOpStatus.Reset();
		OutError.Reset();
		check(IsInGameThread());

		// One transaction for the whole batch: a single undo step reverts every op together.
		FScopedTransaction Transaction(LOCTEXT("ApplyOpsTransaction", "HyperAI Niagara Edit"));
		System.Modify();
		FNiagaraExternalEditContext Context(&System);

		for (const FHyperAINiagaraEditOp& Op : Ops)
		{
			const int32 ErrorsBefore = Context.Errors.Num();
			FString Status = TEXT("applied");

			if (Op.Kind == TEXT("set_module_enabled"))
			{
				UNiagaraExternalEditUtilities::SetModuleEnabled(MakeReference(System, Op), Op.bEnabled, Context);
			}
			else if (Op.Kind == TEXT("add_module"))
			{
				FNiagaraExt_ModuleTopology Added;
				UNiagaraExternalEditUtilities::AddModule(MakeReference(System, Op),
					Cast<UNiagaraScript>(FSoftObjectPath(Op.AssetPath).ResolveObject()), Added, Context);
				Status = TEXT("added_module:") + Added.ModuleName.ToString();
			}
			else if (Op.Kind == TEXT("add_renderer"))
			{
				FNiagaraExt_RendererRef Added;
				UNiagaraExternalEditUtilities::AddRenderer(MakeReference(System, Op),
					FSoftClassPath(Op.AssetPath).ResolveClass(), Added, Context);
				Status = FString::Printf(TEXT("added_renderer:%d"), Added.RendererIndex);
			}
			else if (Op.Kind == TEXT("add_emitter"))
			{
				FNiagaraExt_EmitterTopology Added;
				UNiagaraExternalEditUtilities::AddEmitter(
					Cast<UNiagaraEmitter>(FSoftObjectPath(Op.AssetPath).ResolveObject()), ToName(Op.Name), Added, Context);
				Status = TEXT("added_emitter:") + Added.EmitterName.ToString();
			}
			else if (Op.Kind == TEXT("set_input_value"))
			{
				FNiagaraExt_StackInputValue Value;
				if (BuildInputValue(Op.ValueType, Op.Value, Value))
				{
					UNiagaraExternalEditUtilities::SetStackInputData(MakeReference(System, Op), Value, Context);
				}
				else
				{
					Context.Error(LOCTEXT("InvalidInputValue", "The input value no longer parses."));
				}
			}
			else if (HyperAIStudio::Niagara::AssetsGate::IsAssetOp(Op.Kind))
			{
				FString AssetError;
				if (!HyperAIStudio::Niagara::AssetsGate::ApplyAssetOp(System, Op, Status, AssetError))
				{
					Context.Error(FText::FromString(AssetError));
				}
			}
			else if (Op.Kind == TEXT("apply_stack_issue_fix"))
			{
				FNiagaraExt_ApplyStackIssueFixResult Result;
				UNiagaraExternalEditUtilities::ApplyStackIssueFix(&System, Op.IssueId, Op.FixId, Result, Context);
				if (!Result.bApplied && Context.Errors.Num() == ErrorsBefore)
				{
					Context.Error(LOCTEXT("FixNotApplied", "The stack issue fix was not applied."));
				}
			}
			else
			{
				Context.Error(LOCTEXT("UnsupportedOp", "Unsupported op kind."));
			}

			if (Context.Errors.Num() > ErrorsBefore)
			{
				OutPerOpStatus.Add(TEXT("failed"));
				OutError = FString::Printf(TEXT("Op %d (%s) failed: %s"), OutPerOpStatus.Num() - 1, *Op.Kind, *JoinErrors(Context, ErrorsBefore));
				return false;
			}
			OutPerOpStatus.Add(Status);
			++OutAppliedCount;
		}
		return true;
	}
}

#undef LOCTEXT_NAMESPACE
