// Games by Hyper 2026.

#include "HyperAIStudioBlueprintWorkflowToolset.h"

#include "CoreGlobals.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "Engine/Engine.h"
#include "HAL/PlatformTime.h"
#include "HyperAIStudioCapabilityPackRegistry.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioOperationJournal.h"
#include "HyperAIStudioPlanExecuteToolset.h"
#include "HyperAIStudioService.h"
#include "HyperAIStudioTypedPlan.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreDelegates.h"
#include "Misc/DateTime.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectGlobals.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(HyperAIStudioBlueprintWorkflowToolset)

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioBlueprintWorkflow, Log, All);

namespace HyperAIStudio::BlueprintWorkflow::Private
{
	constexpr const TCHAR* PackId = TEXT("blueprint");
	constexpr int32 MaxLinksPerOutputPin = 64;
	constexpr int64 ArtifactLifetimeMs = 4ll * 60ll * 1000ll;

	int64 NowUtcMs()
	{
		const FDateTime Now = FDateTime::UtcNow();
		return Now.ToUnixTimestamp() * 1000 + Now.GetMillisecond();
	}

	bool IsSha256(const FString& Value)
	{
		if (Value.Len() != 71 || !Value.StartsWith(TEXT("sha256:"), ESearchCase::CaseSensitive))
		{
			return false;
		}
		for (int32 Index = 7; Index < Value.Len(); ++Index)
		{
			const TCHAR C = Value[Index];
			if (!((C >= TEXT('0') && C <= TEXT('9')) || (C >= TEXT('a') && C <= TEXT('f'))))
			{
				return false;
			}
		}
		return true;
	}

	FString Clip(const FString& Value, const int32 MaxCharacters)
	{
		return Value.Len() <= MaxCharacters ? Value : Value.Left(MaxCharacters);
	}

	bool IsEmpty(const FHyperAIBlueprintGraphRef& Value)
	{
		return Value.GraphGuid.IsEmpty() && Value.GraphPath.IsEmpty();
	}

	bool IsEmpty(const FHyperAIBlueprintNodeRef& Value)
	{
		return IsEmpty(Value.Graph) && Value.NodeGuid.IsEmpty();
	}

	bool IsEmpty(const FHyperAIBlueprintPinRef& Value)
	{
		return IsEmpty(Value.Node) && Value.PersistentGuid.IsEmpty() && Value.PinName.IsEmpty()
			&& Value.Direction.IsEmpty() && Value.PinIndex == -1;
	}

	bool ParseGuid(const FString& Value, FGuid& OutGuid)
	{
		OutGuid.Invalidate();
		return Value.Len() == 36
			&& FGuid::ParseExact(Value, EGuidFormats::DigitsWithHyphens, OutGuid)
			&& OutGuid.IsValid();
	}

	UBlueprint* ResolveLoadedBlueprint(const FString& AssetPath, FString& OutStatus, FString& OutDiagnostic)
	{
		OutStatus.Reset();
		OutDiagnostic.Reset();
		if (!IsInGameThread())
		{
			OutStatus = TEXT("game_thread_required");
			OutDiagnostic = TEXT("Loaded Blueprint workflows require Unreal's serialized game thread.");
			return nullptr;
		}
		if (AssetPath.IsEmpty() || AssetPath.Len() > FHyperAIStudioBlueprintPatch::MaxPathCharacters)
		{
			OutStatus = TEXT("invalid_asset_path");
			OutDiagnostic = TEXT("asset_path must be one bounded Unreal object path.");
			return nullptr;
		}
		const FSoftObjectPath Reference(AssetPath);
		const FString PackageName = Reference.GetLongPackageName();
		if (!Reference.IsValid() || PackageName.IsEmpty()
			|| !FPackageName::IsValidLongPackageName(PackageName) || PackageName.Contains(TEXT("..")))
		{
			OutStatus = TEXT("invalid_asset_path");
			OutDiagnostic = TEXT("asset_path must be a valid Unreal object path, never a filesystem path.");
			return nullptr;
		}
		UObject* Object = Reference.ResolveObject();
		if (!Object)
		{
			OutStatus = TEXT("asset_not_loaded");
			OutDiagnostic = TEXT("Open the Blueprint with Epic's editor tools first; HyperAI never synchronously loads it on the MCP game thread.");
			return nullptr;
		}
		UBlueprint* Blueprint = Cast<UBlueprint>(Object);
		if (!Blueprint || Blueprint->GetPathName() != AssetPath)
		{
			OutStatus = TEXT("not_exact_loaded_blueprint");
			OutDiagnostic = TEXT("The exact already-loaded object is not a UBlueprint.");
			return nullptr;
		}
		return Blueprint;
	}

	FHyperAIBlueprintEffectReport ToEffectReport(const FHyperAIBlueprintPatchPlan& Plan)
	{
		FHyperAIBlueprintEffectReport Out;
		Out.OperationCount = Plan.Effects.OperationCount;
		Out.NoOpCount = Plan.Effects.NoOpCount;
		Out.NodesCreated = Plan.Effects.NodesCreated;
		Out.NodesMoved = Plan.Effects.NodesMoved;
		Out.LinksAdded = Plan.Effects.LinksAdded;
		Out.LinksBroken = Plan.Effects.LinksBroken;
		Out.DefaultsChanged = Plan.Effects.DefaultsChanged;
		Out.LayoutNodesMoved = Plan.Effects.LayoutNodesMoved;
		Out.bCompileOnce = Plan.Effects.bRequiresCompile;
		Out.bSaveOnce = Plan.Effects.bRequiresCallerSave;
		Out.bFreshVerifyOnce = Plan.Effects.bRequiresCallerSave;
		return Out;
	}

	int32 EffectCount(const FHyperAIBlueprintPatchPlan& Plan)
	{
		return Plan.Effects.NodesCreated + Plan.Effects.CompileRequests + Plan.Effects.NodesMoved
			+ Plan.Effects.LinksAdded + Plan.Effects.LinksBroken + Plan.Effects.DefaultsChanged
			+ Plan.Effects.NodesDeleted + Plan.Effects.LayoutNodesMoved;
	}

	void CopyIssues(const TArray<FHyperAIBlueprintPatchIssue>& Source, FHyperAIBlueprintPatchReport& Out)
	{
		for (const FHyperAIBlueprintPatchIssue& Item : Source)
		{
			if (Out.Issues.Num() >= FHyperAIStudioBlueprintPatch::MaxIssues)
			{
				break;
			}
			FHyperAIBlueprintWorkflowIssue& Issue = Out.Issues.AddDefaulted_GetRef();
			Issue.Code = Clip(Item.Code, 96);
			Issue.Severity = Clip(Item.Severity, 16);
			Issue.OperationIndex = Item.OperationIndex;
			Issue.Target = Clip(Item.Target, FHyperAIStudioBlueprintPatch::MaxPathCharacters);
			Issue.Message = Clip(Item.Message, 512);
		}
	}

	TSharedRef<FJsonObject> MakeBudget(const int32 NativeOperations, const int32 GameThreadMs, const int32 OutputBytes)
	{
		TSharedRef<FJsonObject> Budget = MakeShared<FJsonObject>();
		Budget->SetNumberField(TEXT("max_native_operations"), NativeOperations);
		Budget->SetNumberField(TEXT("max_game_thread_ms"), GameThreadMs);
		Budget->SetNumberField(TEXT("max_output_bytes"), OutputBytes);
		return Budget;
	}

	bool BuildBlueprintPlanJson(
		const FHyperAIBlueprintPatch& Patch,
		const FHyperAIBlueprintPatchPlan& PatchPlan,
		const FString& OperationId,
		const bool bDryRun,
		const FString& AuthorizationToken,
		FString& OutJson,
		FHyperAIStudioValidatedPlan& OutPlan,
		FString& OutError)
	{
		OutJson.Reset();
		OutPlan = {};
		OutError.Reset();
		if (!PatchPlan.bValid || EffectCount(PatchPlan) <= 0 || !IsSha256(PatchPlan.PlanHash)
			|| !FHyperAIStudioOperationJournal::IsValidOperationId(OperationId))
		{
			OutError = TEXT("Only a valid classified patch and bounded operation_id can form a Blueprint execution plan.");
			return false;
		}
		if ((!bDryRun && PatchPlan.Safety == EHyperAIBlueprintPatchSafety::Destructive
				&& AuthorizationToken.IsEmpty())
			|| AuthorizationToken.Len() > FHyperAIStudioBlueprintWorkflowContracts::MaxAuthorizationTokenCharacters)
		{
			OutError = TEXT("Destructive execution requires one bounded opaque server-issued authorization token.");
			return false;
		}
		const FHyperAIStudioTypedOperationRegistry Registry =
			FHyperAIStudioTypedOperationRegistry::CreateFoundationRegistry();
		const FString CapabilityHash = Registry.ComputeCapabilityHash();
		if (!IsSha256(CapabilityHash))
		{
			OutError = TEXT("The frozen native operation registry did not produce a canonical capability hash.");
			return false;
		}
		const FString PatchId = TEXT("bp-") + PatchPlan.PlanHash.Mid(7, 48);
		const FString PatchEffect = FHyperAIStudioBlueprintPatch::ComputeEffectFingerprint(PatchPlan);
		if (!IsSha256(PatchEffect))
		{
			OutError = TEXT("The Blueprint patch effect fingerprint is unavailable.");
			return false;
		}

		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("schema"), FHyperAIStudioValidatedPlan::SchemaVersion);
		Root->SetBoolField(TEXT("dry_run"), bDryRun);
		Root->SetStringField(TEXT("capability_hash"), CapabilityHash);
		Root->SetStringField(TEXT("operation_id"), OperationId);
		if (!AuthorizationToken.IsEmpty())
		{
			Root->SetStringField(TEXT("authorization_token"), AuthorizationToken);
		}
		TSharedRef<FJsonObject> RootBudget = MakeShared<FJsonObject>();
		RootBudget->SetNumberField(TEXT("deadline_ms"), 30000);
		RootBudget->SetNumberField(TEXT("max_steps"), 1);
		RootBudget->SetNumberField(TEXT("max_mutations"), 1);
		RootBudget->SetNumberField(TEXT("max_native_operations"), Patch.Operations.Num() + 16);
		RootBudget->SetNumberField(TEXT("max_game_thread_ms"), 11000);
		RootBudget->SetNumberField(TEXT("max_output_bytes"), 262144);
		Root->SetObjectField(TEXT("budget"), RootBudget);

		TSharedRef<FJsonObject> Step = MakeShared<FJsonObject>();
		const bool bDestructive = PatchPlan.Safety == EHyperAIBlueprintPatchSafety::Destructive;
		Step->SetStringField(TEXT("id"), bDestructive ? TEXT("blueprint_delete_patch") : TEXT("blueprint_patch"));
		Step->SetStringField(TEXT("operation"), bDestructive
			? FHyperAIStudioBlueprintPatch::DeleteTypedOperationType
			: FHyperAIStudioBlueprintPatch::EditTypedOperationType);
		Step->SetArrayField(TEXT("depends_on"), TArray<TSharedPtr<FJsonValue>>());
		TSharedRef<FJsonObject> Arguments = MakeShared<FJsonObject>();
		Arguments->SetStringField(TEXT("asset_path"), Patch.TargetAssetPath);
		Arguments->SetStringField(TEXT("patch_id"), PatchId);
		Step->SetObjectField(TEXT("arguments"), Arguments);
		TSharedRef<FJsonObject> Exists = MakeShared<FJsonObject>();
		Exists->SetStringField(TEXT("kind"), TEXT("object_exists"));
		Exists->SetStringField(TEXT("target"), Patch.TargetAssetPath);
		TSharedRef<FJsonObject> Revision = MakeShared<FJsonObject>();
		Revision->SetStringField(TEXT("kind"), TEXT("revision_equals"));
		Revision->SetStringField(TEXT("target"), Patch.TargetAssetPath);
		Revision->SetStringField(TEXT("expected"), Patch.ExpectedRevision);
		TArray<TSharedPtr<FJsonValue>> Preconditions;
		Preconditions.Add(MakeShared<FJsonValueObject>(Exists));
		Preconditions.Add(MakeShared<FJsonValueObject>(Revision));
		Step->SetArrayField(TEXT("preconditions"), Preconditions);
		TSharedRef<FJsonObject> Effect = MakeShared<FJsonObject>();
		Effect->SetStringField(TEXT("kind"), TEXT("object_updated"));
		Effect->SetStringField(TEXT("target"), Patch.TargetAssetPath);
		Effect->SetStringField(TEXT("validator_id"), bDestructive
			? TEXT("blueprint.delete_compile_validate") : TEXT("blueprint.compile_validate"));
		Effect->SetStringField(TEXT("expected"), PatchEffect);
		TArray<TSharedPtr<FJsonValue>> Effects;
		Effects.Add(MakeShared<FJsonValueObject>(Effect));
		if (bDestructive)
		{
			TSharedRef<FJsonObject> DeletedEffect = MakeShared<FJsonObject>();
			DeletedEffect->SetStringField(TEXT("kind"), TEXT("object_deleted"));
			DeletedEffect->SetStringField(TEXT("target"), Patch.TargetAssetPath);
			DeletedEffect->SetStringField(TEXT("validator_id"), TEXT("blueprint.delete_compile_validate"));
			DeletedEffect->SetStringField(TEXT("expected"), PatchEffect);
			Effects.Add(MakeShared<FJsonValueObject>(DeletedEffect));
		}
		Step->SetArrayField(TEXT("effects"), Effects);
		Step->SetObjectField(TEXT("budget"), MakeBudget(Patch.Operations.Num() + 2, 10000, 8192));
		TArray<TSharedPtr<FJsonValue>> Steps;
		Steps.Add(MakeShared<FJsonValueObject>(Step));
		Root->SetArrayField(TEXT("steps"), Steps);

		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutJson);
		if (!FJsonSerializer::Serialize(Root, Writer))
		{
			OutError = TEXT("The closed Blueprint execution plan could not be serialized.");
			return false;
		}
		FHyperAIStudioPlanDryRunResult DryRunResult;
		if (!FHyperAIStudioTypedPlanValidator::ValidateJson(OutJson, Registry, OutPlan, DryRunResult))
		{
			OutError = DryRunResult.Diagnostics.IsEmpty()
				? TEXT("The closed Blueprint execution plan failed strict validation.")
				: DryRunResult.Diagnostics[0].Code + TEXT(": ") + DryRunResult.Diagnostics[0].Message;
			return false;
		}
		return true;
	}

	FHyperAIBlueprintPatchReport StagePreparedAndSubmit(
		const FString& ToolName,
		const FHyperAIBlueprintPatch& BackendPatch,
		const FHyperAIBlueprintPatchPlan& PatchPlan,
		const FString& OperationId,
		const FString& ExpectedPlanHash,
		const FString& AuthorizationToken,
		const bool bRequireCreate)
	{
		FHyperAIBlueprintPatchReport Report;
		Report.bDryRun = false;
		Report.OperationId = OperationId;
		Report.RevisionBefore = PatchPlan.BaseRevision;
		Report.PatchPlanHash = PatchPlan.PlanHash;
		Report.PatchEffectFingerprint = FHyperAIStudioBlueprintPatch::ComputeEffectFingerprint(PatchPlan);
		Report.Effects = ToEffectReport(PatchPlan);
		CopyIssues(PatchPlan.Issues, Report);
		if (!PatchPlan.bValid)
		{
			Report.Status = PatchPlan.Status.IsEmpty() ? TEXT("patch_invalid") : PatchPlan.Status;
			Report.Diagnostic = TEXT("The prepared Blueprint patch failed closed validation.");
			return Report;
		}
		if (EffectCount(PatchPlan) <= 0)
		{
			Report.bOk = true;
			Report.Status = TEXT("no_changes");
			Report.Diagnostic = TEXT("The exact revision already satisfies every requested Blueprint operation; no execution was staged.");
			return Report;
		}
		const bool bDestructive = PatchPlan.Safety == EHyperAIBlueprintPatchSafety::Destructive;
		if (bDestructive && ToolName != TEXT("hyper_blueprint_apply_patch")
			&& ToolName != TEXT("hyper_blueprint_repair"))
		{
			Report.Status = TEXT("destructive_route_not_exposed");
			Report.Diagnostic = TEXT("This Blueprint workflow cannot execute a classified node-removal patch.");
			return Report;
		}
		if (bDestructive != (PatchPlan.Effects.NodesDeleted > 0))
		{
			Report.Status = TEXT("destructive_classification_mismatch");
			Report.Diagnostic = TEXT("Patch safety and deletion effects do not form one exact classified route.");
			return Report;
		}
		FString ResolveStatus;
		FString ResolveDiagnostic;
		UBlueprint* Blueprint = ResolveLoadedBlueprint(
			BackendPatch.TargetAssetPath, ResolveStatus, ResolveDiagnostic);
		if (!Blueprint)
		{
			Report.Status = ResolveStatus;
			Report.Diagnostic = ResolveDiagnostic;
			return Report;
		}
		const FHyperAIBlueprintPatchResult LiveDryRun = FHyperAIStudioBlueprintPatch::DryRunLoaded(
			Blueprint, BackendPatch, bDestructive
				? FHyperAIStudioBlueprintPatch::DeleteTypedOperationType
				: FHyperAIStudioBlueprintPatch::EditTypedOperationType);
		if (!LiveDryRun.bOk || LiveDryRun.PlanHash != PatchPlan.PlanHash)
		{
			Report.Status = LiveDryRun.Status.IsEmpty() ? TEXT("live_patch_rejected") : LiveDryRun.Status;
			Report.Diagnostic = LiveDryRun.Diagnostic.IsEmpty()
				? TEXT("Live schema/revision validation no longer matches the prepared patch.")
				: Clip(LiveDryRun.Diagnostic, 512);
			return Report;
		}
		if (bRequireCreate && PatchPlan.Effects.NodesCreated <= 0)
		{
			Report.bOk = false;
			Report.Status = TEXT("build_batch_requires_create");
			Report.Diagnostic = TEXT("hyper_blueprint_build_batch requires at least one allowlisted create_node operation.");
			return Report;
		}
		if (!bDestructive && !AuthorizationToken.IsEmpty())
		{
			Report.bOk = false;
			Report.Status = TEXT("authorization_not_applicable");
			Report.Diagnostic = TEXT("Edit plans do not accept a bearer token; destructive authorization cannot be smuggled through an edit route.");
			return Report;
		}
		if (bDestructive && AuthorizationToken.IsEmpty())
		{
			Report.Status = TEXT("destructive_authorization_required");
			Report.Diagnostic = TEXT("Node removal requires one opaque server-issued token bound to this exact operation/plan/effect/project.");
			return Report;
		}
		if (!FHyperAIStudioOperationJournal::IsValidOperationId(OperationId))
		{
			Report.bOk = false;
			Report.Status = TEXT("invalid_operation_id");
			Report.Diagnostic = TEXT("Every managed Blueprint mutation requires one bounded client operation_id.");
			return Report;
		}

		FString PlanJson;
		FHyperAIStudioValidatedPlan ExecutionPlan;
		FString PlanError;
		if (!BuildBlueprintPlanJson(BackendPatch, PatchPlan, OperationId, false, AuthorizationToken,
			PlanJson, ExecutionPlan, PlanError))
		{
			Report.bOk = false;
			Report.Status = TEXT("execution_plan_invalid");
			Report.Diagnostic = Clip(PlanError, 512);
			return Report;
		}
		Report.PlanHash = ExecutionPlan.PlanHash;
		Report.EffectFingerprint = ExecutionPlan.EffectFingerprint;
		if (!IsSha256(ExpectedPlanHash) || ExpectedPlanHash != ExecutionPlan.PlanHash)
		{
			Report.bOk = false;
			Report.Status = TEXT("expected_plan_hash_mismatch");
			Report.Diagnostic = TEXT("The mutation must echo the exact plan hash returned by hyper_blueprint_validate_patch.");
			return Report;
		}

		const FString CanonicalProjectId = FHyperAIStudioOperationJournal::MakeCanonicalProjectId(
			FHyperAIStudioService::GetProjectRoot());
		FHyperAIStudioStagedBlueprintPatchArtifact Artifact;
		Artifact.CanonicalProjectId = CanonicalProjectId;
		Artifact.OperationId = OperationId;
		Artifact.PlanHash = ExecutionPlan.PlanHash;
		Artifact.CapabilityHash = ExecutionPlan.CapabilityHash;
		Artifact.EffectFingerprint = ExecutionPlan.EffectFingerprint;
		Artifact.PatchId = TEXT("bp-") + PatchPlan.PlanHash.Mid(7, 48);
		Artifact.AssetPath = BackendPatch.TargetAssetPath;
		Artifact.ExpiresUtcMs = NowUtcMs() + ArtifactLifetimeMs;
		Artifact.Patch = BackendPatch;
		FString StageError;
		if (!FHyperAIStudioPlanExecutionService::StageBlueprintPatch(Artifact, StageError))
		{
			Report.bOk = false;
			Report.Status = TEXT("patch_stage_rejected");
			Report.Diagnostic = Clip(StageError, 512);
			return Report;
		}
		Report.bStaged = true;
		const FHyperAIPlanExecuteResponse Response = FHyperAIStudioPlanExecutionService::Submit(PlanJson);
		Report.bOk = Response.bOk;
		Report.bExecutionSubmitted = Response.bAccepted;
		Report.Status = Response.Status;
		Report.Diagnostic = Clip(Response.Diagnostic.IsEmpty()
			? ToolName + TEXT(" submitted one journal-bound serial workflow.") : Response.Diagnostic, 512);
		Report.bMutationStarted = false;
		return Report;
	}

	FHyperAIBlueprintPatchReport StageAndSubmit(
		const FString& ToolName,
		const FHyperAIBlueprintMutationRequest& Request,
		const bool bRequireCreate)
	{
		FHyperAIBlueprintPatch BackendPatch;
		FHyperAIBlueprintPatchPlan PatchPlan;
		FHyperAIBlueprintPatchReport Validation =
			FHyperAIStudioBlueprintWorkflowContracts::ValidatePatchLoaded(
				Request.Patch, &BackendPatch, &PatchPlan);
		if (!Validation.bOk)
		{
			Validation.bDryRun = false;
			Validation.OperationId = Request.OperationId;
			return Validation;
		}
		return StagePreparedAndSubmit(ToolName, BackendPatch, PatchPlan, Request.OperationId,
			Request.ExpectedPlanHash, Request.AuthorizationToken, bRequireCreate);
	}

	struct FSnapshotCacheEntry
	{
		FString Token;
		FString AssetPath;
		FString GraphPath;
		bool bIncludePins = true;
		bool bIncludeLinks = true;
		int64 CreatedUtcMs = 0;
		FHyperAIBlueprintPatchSnapshot Snapshot;
	};

	FCriticalSection SnapshotCacheMutex;
	TArray<FSnapshotCacheEntry> SnapshotCache;

	void PruneSnapshotCacheLocked()
	{
		const int64 Now = NowUtcMs();
		SnapshotCache.RemoveAll([Now](const FSnapshotCacheEntry& Entry)
		{
			return Entry.CreatedUtcMs <= 0 || Now - Entry.CreatedUtcMs >
				FHyperAIStudioBlueprintWorkflowContracts::SnapshotCacheLifetimeMs;
		});
		while (SnapshotCache.Num() > FHyperAIStudioBlueprintWorkflowContracts::MaxSnapshotCacheEntries)
		{
			SnapshotCache.RemoveAt(0);
		}
	}

	FString StoreSnapshot(
		const FHyperAIBlueprintPatchSnapshot& Snapshot,
		const FHyperAIBlueprintSnapshotRequest& Request)
	{
		FScopeLock Lock(&SnapshotCacheMutex);
		PruneSnapshotCacheLocked();
		if (SnapshotCache.Num() >= FHyperAIStudioBlueprintWorkflowContracts::MaxSnapshotCacheEntries)
		{
			SnapshotCache.RemoveAt(0);
		}
		FSnapshotCacheEntry& Entry = SnapshotCache.AddDefaulted_GetRef();
		Entry.Token = TEXT("bps-") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
		Entry.AssetPath = Request.AssetPath;
		Entry.GraphPath = Request.GraphPath;
		Entry.bIncludePins = Request.bIncludePins;
		Entry.bIncludeLinks = Request.bIncludeLinks;
		Entry.CreatedUtcMs = NowUtcMs();
		Entry.Snapshot = Snapshot;
		return Entry.Token;
	}

	bool FindSnapshot(const FString& Token, FSnapshotCacheEntry& Out)
	{
		FScopeLock Lock(&SnapshotCacheMutex);
		PruneSnapshotCacheLocked();
		const FSnapshotCacheEntry* Entry = SnapshotCache.FindByPredicate(
			[&Token](const FSnapshotCacheEntry& Candidate) { return Candidate.Token == Token; });
		if (!Entry)
		{
			return false;
		}
		Out = *Entry;
		return true;
	}

	bool ParseSnapshotCursor(const FString& Cursor, FString& OutToken, int32& OutOffset)
	{
		OutToken.Reset();
		OutOffset = 0;
		if (Cursor.IsEmpty())
		{
			return true;
		}
		if (Cursor.Len() > FHyperAIStudioBlueprintWorkflowContracts::MaxCursorCharacters
			|| !Cursor.Split(TEXT("|"), &OutToken, nullptr, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
		{
			return false;
		}
		FString OffsetText;
		Cursor.Split(TEXT("|"), nullptr, &OffsetText, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		return OutToken.StartsWith(TEXT("bps-"), ESearchCase::CaseSensitive)
			&& LexTryParseString(OutOffset, *OffsetText) && OutOffset >= 0;
	}

	FString DirectionToken(const EHyperAIBlueprintPinDirection Direction)
	{
		return Direction == EHyperAIBlueprintPinDirection::Input ? TEXT("input")
			: Direction == EHyperAIBlueprintPinDirection::Output ? TEXT("output") : TEXT("invalid");
	}

	TArray<FHyperAIBlueprintSnapshotItem> FlattenSnapshot(
		const FHyperAIBlueprintPatchSnapshot& Snapshot,
		const FString& GraphFilter,
		const bool bIncludePins,
		const bool bIncludeLinks,
		bool& bOutSourceTruncated)
	{
		bOutSourceTruncated = false;
		TArray<FHyperAIBlueprintSnapshotItem> Items;
		Items.Reserve(FMath::Min(
			Snapshot.GraphCount + Snapshot.NodeCount + (bIncludePins ? Snapshot.PinCount : 0),
			FHyperAIStudioBlueprintPatch::MaxGraphs + FHyperAIStudioBlueprintPatch::MaxNodes
				+ FHyperAIStudioBlueprintPatch::MaxPins));
		for (const FHyperAIBlueprintPatchGraphSnapshot& Graph : Snapshot.Graphs)
		{
			if (!GraphFilter.IsEmpty() && Graph.Id.GraphPath != GraphFilter)
			{
				continue;
			}
			FHyperAIBlueprintSnapshotItem& GraphItem = Items.AddDefaulted_GetRef();
			GraphItem.Kind = TEXT("graph");
			GraphItem.StableId = Graph.Id.StableKey();
			GraphItem.GraphPath = Graph.Id.GraphPath;
			GraphItem.Name = FPaths::GetBaseFilename(Graph.Id.GraphPath);
			GraphItem.ClassPath = Graph.SchemaClassPath;
			for (const FHyperAIBlueprintPatchNodeSnapshot& Node : Graph.Nodes)
			{
				FHyperAIBlueprintSnapshotItem& NodeItem = Items.AddDefaulted_GetRef();
				NodeItem.Kind = TEXT("node");
				NodeItem.StableId = Node.Id.StableKey();
				NodeItem.ParentId = Graph.Id.StableKey();
				NodeItem.GraphPath = Graph.Id.GraphPath;
				NodeItem.Name = Node.Id.NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
				NodeItem.ClassPath = Node.NodeClassPath;
				NodeItem.X = Node.X;
				NodeItem.Y = Node.Y;
				NodeItem.bCanUserDelete = Node.bCanUserDelete;
				if (!bIncludePins)
				{
					continue;
				}
				for (const FHyperAIBlueprintPatchPinSnapshot& Pin : Node.Pins)
				{
					FHyperAIBlueprintSnapshotItem& PinItem = Items.AddDefaulted_GetRef();
					PinItem.Kind = TEXT("pin");
					PinItem.StableId = Pin.Id.StableKey();
					PinItem.ParentId = Node.Id.StableKey();
					PinItem.GraphPath = Graph.Id.GraphPath;
					PinItem.Name = Pin.Id.PinName.ToString();
					PinItem.PinType = Pin.PinType;
					PinItem.Direction = DirectionToken(Pin.Id.Direction);
					PinItem.DefaultValue = Pin.DefaultValue;
					PinItem.bOrphaned = Pin.bOrphaned;
					if (bIncludeLinks)
					{
						const int32 Count = FMath::Min(Pin.LinkedPinKeys.Num(), MaxLinksPerOutputPin);
						for (int32 LinkIndex = 0; LinkIndex < Count; ++LinkIndex)
						{
							PinItem.LinkedPinIds.Add(Pin.LinkedPinKeys[LinkIndex]);
						}
						bOutSourceTruncated |= Pin.LinkedPinKeys.Num() > Count;
					}
				}
			}
		}
		return Items;
	}

	int32 EstimateItemBytes(const FHyperAIBlueprintSnapshotItem& Item)
	{
		int64 Total = 160 + 2ll * (Item.Kind.Len() + Item.StableId.Len() + Item.ParentId.Len()
			+ Item.GraphPath.Len() + Item.Name.Len() + Item.ClassPath.Len()
			+ Item.PinType.Len() + Item.Direction.Len() + Item.DefaultValue.Len());
		for (const FString& Link : Item.LinkedPinIds)
		{
			Total += 16 + 2ll * Link.Len();
		}
		return static_cast<int32>(FMath::Min<int64>(Total, MAX_int32));
	}
}

FString FHyperAIStudioBlueprintWorkflowContracts::GetQualifiedToolsetName()
{
	return TEXT("HyperAIStudio.HyperAIStudioBlueprintWorkflowToolset");
}

const TArray<FHyperAIStudioBlueprintWorkflowManifestEntry>&
FHyperAIStudioBlueprintWorkflowContracts::GetManifest()
{
	static const TArray<FHyperAIStudioBlueprintWorkflowManifestEntry> Manifest = {
		{ TEXT("hyper_blueprint_snapshot"), GetQualifiedToolsetName(), FHyperAIStudioBlueprintWorkflowManifestEntry::ECompiledCeiling::SourceCandidate },
		{ TEXT("hyper_blueprint_discover_actions"), GetQualifiedToolsetName(), FHyperAIStudioBlueprintWorkflowManifestEntry::ECompiledCeiling::SourceCandidate },
		{ TEXT("hyper_blueprint_validate_patch"), GetQualifiedToolsetName(), FHyperAIStudioBlueprintWorkflowManifestEntry::ECompiledCeiling::SourceCandidate },
		{ TEXT("hyper_blueprint_apply_patch"), GetQualifiedToolsetName(), FHyperAIStudioBlueprintWorkflowManifestEntry::ECompiledCeiling::SourceCandidate },
		{ TEXT("hyper_blueprint_build_batch"), GetQualifiedToolsetName(), FHyperAIStudioBlueprintWorkflowManifestEntry::ECompiledCeiling::SourceCandidate },
		{ TEXT("hyper_blueprint_compile_validate"), GetQualifiedToolsetName(), FHyperAIStudioBlueprintWorkflowManifestEntry::ECompiledCeiling::SourceCandidate },
		{ TEXT("hyper_blueprint_repair"), GetQualifiedToolsetName(), FHyperAIStudioBlueprintWorkflowManifestEntry::ECompiledCeiling::SourceCandidate },
		{ TEXT("hyper_blueprint_diff"), GetQualifiedToolsetName(), FHyperAIStudioBlueprintWorkflowManifestEntry::ECompiledCeiling::SourceCandidate },
		{ TEXT("hyper_blueprint_layout"), GetQualifiedToolsetName(), FHyperAIStudioBlueprintWorkflowManifestEntry::ECompiledCeiling::SourceCandidate }
	};
	return Manifest;
}

bool FHyperAIStudioBlueprintWorkflowContracts::IsPendingTestRegistrationEnabled()
{
	return FHyperAIStudioExtensionRuntime::AreSourceCandidateToolsEnabled();
}

bool FHyperAIStudioBlueprintWorkflowContracts::IsRegistrationAllowed(
	const bool bAllowSourceCandidateForDev)
{
	using namespace HyperAIStudio::BlueprintWorkflow::Private;
	TArray<FString> Errors;
	if (!FHyperAIStudioCapabilityPackRegistry::ValidateBuiltInCatalog(Errors))
	{
		return false;
	}
	const TArray<FHyperAIStudioBlueprintWorkflowManifestEntry>& Manifest = GetManifest();
	if (Manifest.Num() != 9)
	{
		return false;
	}
	TSet<FString> Names;
	TOptional<EHyperAIStudioCapabilityAdmissionState> CohortState;
	FString CohortId;
	const FHyperAIStudioCapabilityCatalog& Catalog = FHyperAIStudioCapabilityPackRegistry::GetCatalog();
	for (const FHyperAIStudioBlueprintWorkflowManifestEntry& Entry : Manifest)
	{
		if (Entry.Name.IsEmpty() || Entry.QualifiedToolset != GetQualifiedToolsetName()
			|| Entry.CompiledCeiling != FHyperAIStudioBlueprintWorkflowManifestEntry::ECompiledCeiling::SourceCandidate
			|| Names.Contains(Entry.Name))
		{
			return false;
		}
		Names.Add(Entry.Name);
		const FHyperAIStudioCapabilityToolDefinition* Match = nullptr;
		int32 MatchCount = 0;
		for (const FHyperAIStudioCapabilityToolDefinition& Tool : Catalog.Tools)
		{
			if (Tool.Name == Entry.Name)
			{
				Match = &Tool;
				++MatchCount;
			}
		}
		if (!Match || MatchCount != 1 || Match->PackId != PackId || Match->AtomicCohortId.IsEmpty())
		{
			return false;
		}
		if (CohortId.IsEmpty())
		{
			CohortId = Match->AtomicCohortId;
			CohortState = Match->AdmissionState;
		}
		else if (CohortId != Match->AtomicCohortId || CohortState.GetValue() != Match->AdmissionState)
		{
			return false;
		}
	}
	if (!CohortState.IsSet())
	{
		return false;
	}
	if (CohortState.GetValue() == EHyperAIStudioCapabilityAdmissionState::Admitted)
	{
		return true;
	}
	return bAllowSourceCandidateForDev
		&& CohortState.GetValue() == EHyperAIStudioCapabilityAdmissionState::SourceCandidate;
}

bool FHyperAIStudioBlueprintWorkflowContracts::ConvertGraphRef(
	const FHyperAIBlueprintGraphRef& Input,
	FHyperAIBlueprintGraphId& Out,
	FString& OutError)
{
	using namespace HyperAIStudio::BlueprintWorkflow::Private;
	Out = {};
	OutError.Reset();
	if (!ParseGuid(Input.GraphGuid, Out.GraphGuid) || Input.GraphPath.IsEmpty()
		|| Input.GraphPath.Len() > FHyperAIStudioBlueprintPatch::MaxPathCharacters
		|| Input.GraphPath.Contains(TEXT("..")))
	{
		OutError = TEXT("graph requires an exact stable GUID and bounded graph path.");
		return false;
	}
	Out.GraphPath = Input.GraphPath;
	if (!Out.IsValid())
	{
		OutError = TEXT("graph identity is invalid.");
		return false;
	}
	return true;
}

bool FHyperAIStudioBlueprintWorkflowContracts::ConvertNodeRef(
	const FHyperAIBlueprintNodeRef& Input,
	FHyperAIBlueprintNodeId& Out,
	FString& OutError)
{
	using namespace HyperAIStudio::BlueprintWorkflow::Private;
	Out = {};
	if (!ConvertGraphRef(Input.Graph, Out.Graph, OutError) || !ParseGuid(Input.NodeGuid, Out.NodeGuid))
	{
		if (OutError.IsEmpty())
		{
			OutError = TEXT("node requires an exact stable GUID.");
		}
		return false;
	}
	return Out.IsValid();
}

bool FHyperAIStudioBlueprintWorkflowContracts::ConvertPinRef(
	const FHyperAIBlueprintPinRef& Input,
	FHyperAIBlueprintPinId& Out,
	FString& OutError)
{
	using namespace HyperAIStudio::BlueprintWorkflow::Private;
	Out = {};
	if (!ConvertNodeRef(Input.Node, Out.Node, OutError))
	{
		return false;
	}
	if (Input.Direction == TEXT("input"))
	{
		Out.Direction = EHyperAIBlueprintPinDirection::Input;
	}
	else if (Input.Direction == TEXT("output"))
	{
		Out.Direction = EHyperAIBlueprintPinDirection::Output;
	}
	else
	{
		OutError = TEXT("pin direction must be exactly input or output.");
		return false;
	}
	if (Input.PinIndex < 0 || Input.PinIndex >= FHyperAIStudioBlueprintPatch::MaxPins
		|| Input.PinName.Len() > 128)
	{
		OutError = TEXT("pin name/index identity is outside the closed bounds.");
		return false;
	}
	Out.PinIndex = Input.PinIndex;
	Out.PinName = FName(*Input.PinName);
	if (!Input.PersistentGuid.IsEmpty() && !ParseGuid(Input.PersistentGuid, Out.PersistentGuid))
	{
		OutError = TEXT("persistent pin GUID is malformed.");
		return false;
	}
	if (!Out.IsValid())
	{
		OutError = TEXT("pin requires persistent GUID or a nonempty revision-bound pin name.");
		return false;
	}
	return true;
}

bool FHyperAIStudioBlueprintWorkflowContracts::ConvertPatch(
	const FHyperAIBlueprintPatchInput& Input,
	FHyperAIBlueprintPatch& Out,
	FString& OutError)
{
	using namespace HyperAIStudio::BlueprintWorkflow::Private;
	Out = {};
	OutError.Reset();
	if (Input.AssetPath.IsEmpty() || Input.AssetPath.Len() > FHyperAIStudioBlueprintPatch::MaxPathCharacters
		|| !IsSha256(Input.ExpectedRevision) || Input.Operations.IsEmpty()
		|| Input.Operations.Num() > FHyperAIStudioBlueprintPatch::MaxOperations)
	{
		OutError = TEXT("patch requires one bounded asset path, exact revision, and 1..256 typed operations.");
		return false;
	}
	Out.TargetAssetPath = Input.AssetPath;
	Out.ExpectedRevision = Input.ExpectedRevision;
	auto CommonUnused = [](const FHyperAIBlueprintPatchOperationInput& Op)
	{
		return Op.NewNodeGuid.IsEmpty() && Op.NodeKind.IsEmpty() && Op.Value.IsEmpty()
			&& Op.DeleteMode.IsEmpty() && Op.LayoutNodes.IsEmpty() && Op.Columns == 0
			&& Op.HorizontalSpacing == 0 && Op.VerticalSpacing == 0;
	};
	for (int32 Index = 0; Index < Input.Operations.Num(); ++Index)
	{
		const FHyperAIBlueprintPatchOperationInput& Op = Input.Operations[Index];
		FHyperAIBlueprintPatchOperation Converted;
		if (Op.Type == TEXT("create_node"))
		{
			if (!IsEmpty(Op.Node) || !IsEmpty(Op.PinA) || !IsEmpty(Op.PinB) || !Op.Value.IsEmpty()
				|| !Op.DeleteMode.IsEmpty() || !Op.LayoutNodes.IsEmpty() || Op.Columns != 0
				|| Op.HorizontalSpacing != 0 || Op.VerticalSpacing != 0)
			{
				OutError = FString::Printf(TEXT("operation %d contains fields outside create_node."), Index);
				return false;
			}
			FHyperAIBlueprintCreateNode Create;
			if (!ConvertGraphRef(Op.Graph, Create.Graph, OutError)
				|| !ParseGuid(Op.NewNodeGuid, Create.NewNodeGuid))
			{
				return false;
			}
			if (Op.NodeKind == TEXT("branch")) Create.Kind = EHyperAIBlueprintCreateNodeKind::Branch;
			else if (Op.NodeKind == TEXT("sequence")) Create.Kind = EHyperAIBlueprintCreateNodeKind::Sequence;
			else if (Op.NodeKind == TEXT("reroute")) Create.Kind = EHyperAIBlueprintCreateNodeKind::Reroute;
			else
			{
				OutError = TEXT("create_node kind must be branch, sequence, or reroute.");
				return false;
			}
			Create.X = Op.X;
			Create.Y = Op.Y;
			Converted.Value.Set<FHyperAIBlueprintCreateNode>(Create);
		}
		else if (Op.Type == TEXT("move_node"))
		{
			if (!IsEmpty(Op.Graph) || !IsEmpty(Op.PinA) || !IsEmpty(Op.PinB) || !CommonUnused(Op))
			{
				OutError = FString::Printf(TEXT("operation %d contains fields outside move_node."), Index);
				return false;
			}
			FHyperAIBlueprintMoveNode Move;
			if (!ConvertNodeRef(Op.Node, Move.Node, OutError)) return false;
			Move.X = Op.X;
			Move.Y = Op.Y;
			Converted.Value.Set<FHyperAIBlueprintMoveNode>(Move);
		}
		else if (Op.Type == TEXT("connect_pins") || Op.Type == TEXT("break_pin_link"))
		{
			if (!IsEmpty(Op.Graph) || !IsEmpty(Op.Node) || !CommonUnused(Op) || Op.X != 0 || Op.Y != 0)
			{
				OutError = FString::Printf(TEXT("operation %d contains fields outside the pin-link operation."), Index);
				return false;
			}
			FHyperAIBlueprintPinId A;
			FHyperAIBlueprintPinId B;
			if (!ConvertPinRef(Op.PinA, A, OutError) || !ConvertPinRef(Op.PinB, B, OutError)) return false;
			if (Op.Type == TEXT("connect_pins"))
			{
				Converted.Value.Set<FHyperAIBlueprintConnectPins>({ A, B });
			}
			else
			{
				Converted.Value.Set<FHyperAIBlueprintBreakPinLink>({ A, B });
			}
		}
		else if (Op.Type == TEXT("set_literal_default"))
		{
			if (!IsEmpty(Op.Graph) || !IsEmpty(Op.Node) || !IsEmpty(Op.PinB) || !Op.NewNodeGuid.IsEmpty()
				|| !Op.NodeKind.IsEmpty() || !Op.DeleteMode.IsEmpty() || !Op.LayoutNodes.IsEmpty()
				|| Op.X != 0 || Op.Y != 0 || Op.Columns != 0 || Op.HorizontalSpacing != 0 || Op.VerticalSpacing != 0
				|| Op.Value.Len() > FHyperAIStudioBlueprintPatch::MaxLiteralCharacters)
			{
				OutError = FString::Printf(TEXT("operation %d contains invalid fields outside set_literal_default."), Index);
				return false;
			}
			FHyperAIBlueprintSetLiteralDefault SetDefault;
			if (!ConvertPinRef(Op.PinA, SetDefault.Pin, OutError)) return false;
			SetDefault.Value = Op.Value;
			Converted.Value.Set<FHyperAIBlueprintSetLiteralDefault>(SetDefault);
		}
		else if (Op.Type == TEXT("delete_node"))
		{
			if (!IsEmpty(Op.Graph) || !IsEmpty(Op.PinA) || !IsEmpty(Op.PinB) || !Op.NewNodeGuid.IsEmpty()
				|| !Op.NodeKind.IsEmpty() || !Op.Value.IsEmpty() || !Op.LayoutNodes.IsEmpty()
				|| Op.X != 0 || Op.Y != 0 || Op.Columns != 0 || Op.HorizontalSpacing != 0 || Op.VerticalSpacing != 0
				|| (Op.DeleteMode != TEXT("orphan_only") && Op.DeleteMode != TEXT("authorized")))
			{
				OutError = FString::Printf(TEXT("operation %d is not a closed orphan_only/authorized delete_node."), Index);
				return false;
			}
			FHyperAIBlueprintDeleteNode Delete;
			if (!ConvertNodeRef(Op.Node, Delete.Node, OutError)) return false;
			Delete.Mode = Op.DeleteMode == TEXT("authorized")
				? EHyperAIBlueprintDeleteMode::ExplicitConfirmed
				: EHyperAIBlueprintDeleteMode::OrphanOnly;
			Converted.Value.Set<FHyperAIBlueprintDeleteNode>(Delete);
		}
		else if (Op.Type == TEXT("layout_nodes"))
		{
			if (!IsEmpty(Op.Node) || !IsEmpty(Op.PinA) || !IsEmpty(Op.PinB) || !Op.NewNodeGuid.IsEmpty()
				|| !Op.NodeKind.IsEmpty() || !Op.Value.IsEmpty() || !Op.DeleteMode.IsEmpty()
				|| Op.LayoutNodes.IsEmpty() || Op.LayoutNodes.Num() > FHyperAIStudioBlueprintPatch::MaxLayoutNodes)
			{
				OutError = FString::Printf(TEXT("operation %d contains invalid fields outside layout_nodes."), Index);
				return false;
			}
			FHyperAIBlueprintLayoutNodes Layout;
			if (!ConvertGraphRef(Op.Graph, Layout.Graph, OutError)) return false;
			Layout.OriginX = Op.X;
			Layout.OriginY = Op.Y;
			Layout.Columns = Op.Columns;
			Layout.HorizontalSpacing = Op.HorizontalSpacing;
			Layout.VerticalSpacing = Op.VerticalSpacing;
			for (const FHyperAIBlueprintNodeRef& NodeInput : Op.LayoutNodes)
			{
				FHyperAIBlueprintNodeId Node;
				if (!ConvertNodeRef(NodeInput, Node, OutError) || Node.Graph.StableKey() != Layout.Graph.StableKey())
				{
					if (OutError.IsEmpty()) OutError = TEXT("every layout node must belong to the exact graph.");
					return false;
				}
				Layout.Nodes.Add(Node);
			}
			Converted.Value.Set<FHyperAIBlueprintLayoutNodes>(Layout);
		}
		else
		{
			OutError = FString::Printf(TEXT("operation %d has an unknown closed type; raw execution is unavailable."), Index);
			return false;
		}
		Out.Operations.Add(MoveTemp(Converted));
	}
	return true;
}

FHyperAIBlueprintPatchReport FHyperAIStudioBlueprintWorkflowContracts::ValidatePatchLoaded(
	const FHyperAIBlueprintPatchInput& Input,
	FHyperAIBlueprintPatch* OutPatch,
	FHyperAIBlueprintPatchPlan* OutPlan)
{
	using namespace HyperAIStudio::BlueprintWorkflow::Private;
	FHyperAIBlueprintPatchReport Report;
	Report.bDryRun = true;
	FHyperAIBlueprintPatch Patch;
	FString Error;
	if (!ConvertPatch(Input, Patch, Error))
	{
		Report.Status = TEXT("invalid_patch_contract");
		Report.Diagnostic = Clip(Error, 512);
		return Report;
	}
	FString ResolveStatus;
	FString ResolveDiagnostic;
	UBlueprint* Blueprint = ResolveLoadedBlueprint(Patch.TargetAssetPath, ResolveStatus, ResolveDiagnostic);
	if (!Blueprint)
	{
		Report.Status = ResolveStatus;
		Report.Diagnostic = ResolveDiagnostic;
		return Report;
	}
	const FHyperAIBlueprintPatchSnapshot Snapshot = FHyperAIStudioBlueprintPatch::CaptureLoaded(Blueprint);
	if (!Snapshot.bComplete)
	{
		Report.Status = Snapshot.Status;
		Report.Diagnostic = Clip(Snapshot.Diagnostic, 512);
		return Report;
	}
	const FHyperAIBlueprintPatchPlan PatchPlan = FHyperAIStudioBlueprintPatch::PlanSnapshot(Snapshot, Patch);
	Report.RevisionBefore = Snapshot.Revision;
	Report.PatchPlanHash = PatchPlan.PlanHash;
	Report.PatchEffectFingerprint = FHyperAIStudioBlueprintPatch::ComputeEffectFingerprint(PatchPlan);
	Report.Effects = ToEffectReport(PatchPlan);
	CopyIssues(PatchPlan.Issues, Report);
	if (!PatchPlan.bValid)
	{
		Report.Status = PatchPlan.Status.IsEmpty() ? TEXT("patch_invalid") : PatchPlan.Status;
		Report.Diagnostic = TEXT("The revision-bound Blueprint patch did not pass pure planning.");
		return Report;
	}
	const FString TypedOperation = PatchPlan.Safety == EHyperAIBlueprintPatchSafety::Edit
		? FHyperAIStudioBlueprintPatch::EditTypedOperationType
		: FHyperAIStudioBlueprintPatch::DeleteTypedOperationType;
	const FHyperAIBlueprintPatchResult DryRun =
		FHyperAIStudioBlueprintPatch::DryRunLoaded(Blueprint, Patch, TypedOperation);
	Report.bOk = DryRun.bOk;
	Report.Status = DryRun.Status;
	Report.Diagnostic = Clip(DryRun.Diagnostic, 512);
	Report.RevisionBefore = DryRun.RevisionBefore;
	Report.PatchPlanHash = DryRun.PlanHash;
	Report.Effects = ToEffectReport(PatchPlan);
	CopyIssues(DryRun.Issues, Report);
	if (!DryRun.bOk)
	{
		return Report;
	}
	if (EffectCount(PatchPlan) <= 0)
	{
		Report.Status = TEXT("no_changes");
		Report.Diagnostic = TEXT("The exact loaded revision already satisfies the requested patch; no mutation plan is needed.");
		if (OutPatch) *OutPatch = Patch;
		if (OutPlan) *OutPlan = PatchPlan;
		return Report;
	}
	{
		const bool bDestructive = PatchPlan.Safety == EHyperAIBlueprintPatchSafety::Destructive;
		const FString ValidationOperationId = TEXT("blueprint-validate-") + PatchPlan.PlanHash.Mid(7, 24);
		FString PlanJson;
		FHyperAIStudioValidatedPlan TypedPlan;
		if (!BuildBlueprintPlanJson(Patch, PatchPlan, ValidationOperationId, true, FString(),
			PlanJson, TypedPlan, Error))
		{
			Report.bOk = false;
			Report.Status = TEXT("execution_plan_invalid");
			Report.Diagnostic = Clip(Error, 512);
			return Report;
		}
		Report.PlanHash = TypedPlan.PlanHash;
		Report.EffectFingerprint = TypedPlan.EffectFingerprint;
		Report.Status = bDestructive ? TEXT("valid_requires_destructive_authorization") : TEXT("valid_dry_run");
		Report.Diagnostic = bDestructive
			? TEXT("Deletion validated without mutation; execution requires an opaque server-issued token bound to this exact plan.")
			: TEXT("Exact revision, live K2 schema, closed operations, effects, and execution binding validated without mutation.");
	}
	if (OutPatch) *OutPatch = Patch;
	if (OutPlan) *OutPlan = PatchPlan;
	return Report;
}

FHyperAIBlueprintDiffReport FHyperAIStudioBlueprintWorkflowContracts::DiffSnapshots(
	const FHyperAIBlueprintPatchSnapshot& Base,
	const FHyperAIBlueprintPatchSnapshot& Current,
	const int32 MaxChanges)
{
	FHyperAIBlueprintDiffReport Report;
	Report.BaseRevision = Base.Revision;
	Report.CurrentRevision = Current.Revision;
	if (!Base.bComplete || !Current.bComplete || Base.BlueprintAssetPath != Current.BlueprintAssetPath
		|| MaxChanges < 1 || MaxChanges > MaxDiffChanges)
	{
		Report.Status = TEXT("invalid_diff_input");
		Report.Diagnostic = TEXT("Diff requires two complete snapshots of one exact Blueprint and a bounded max_changes.");
		return Report;
	}
	TMap<FString, TMap<FString, FString>> Before;
	TMap<FString, TMap<FString, FString>> After;
	auto Flatten = [](const FHyperAIBlueprintPatchSnapshot& Snapshot,
		TMap<FString, TMap<FString, FString>>& Out)
	{
		TMap<FString, FString>& Asset = Out.Add(TEXT("asset:") + Snapshot.BlueprintAssetPath);
		Asset.Add(TEXT("kind"), TEXT("asset"));
		Asset.Add(TEXT("compile_status"), Snapshot.CompileStatus);
		Asset.Add(TEXT("generated_class"), Snapshot.GeneratedClassPath);
		for (const FHyperAIBlueprintPatchGraphSnapshot& Graph : Snapshot.Graphs)
		{
			TMap<FString, FString>& GraphFields = Out.Add(Graph.Id.StableKey());
			GraphFields.Add(TEXT("kind"), TEXT("graph"));
			GraphFields.Add(TEXT("schema"), Graph.SchemaClassPath);
			for (const FHyperAIBlueprintPatchNodeSnapshot& Node : Graph.Nodes)
			{
				TMap<FString, FString>& NodeFields = Out.Add(Node.Id.StableKey());
				NodeFields.Add(TEXT("kind"), TEXT("node"));
				NodeFields.Add(TEXT("class"), Node.NodeClassPath);
				NodeFields.Add(TEXT("x"), FString::FromInt(Node.X));
				NodeFields.Add(TEXT("y"), FString::FromInt(Node.Y));
				NodeFields.Add(TEXT("can_delete"), Node.bCanUserDelete ? TEXT("true") : TEXT("false"));
				for (const FHyperAIBlueprintPatchPinSnapshot& Pin : Node.Pins)
				{
					TMap<FString, FString>& PinFields = Out.Add(Pin.Id.StableKey());
					PinFields.Add(TEXT("kind"), TEXT("pin"));
					PinFields.Add(TEXT("type"), Pin.PinType);
					PinFields.Add(TEXT("default"), Pin.DefaultValue);
					PinFields.Add(TEXT("orphaned"), Pin.bOrphaned ? TEXT("true") : TEXT("false"));
					PinFields.Add(TEXT("default_read_only"), Pin.bDefaultReadOnly ? TEXT("true") : TEXT("false"));
					PinFields.Add(TEXT("default_ignored"), Pin.bDefaultIgnored ? TEXT("true") : TEXT("false"));
					TArray<FString> Links = Pin.LinkedPinKeys;
					Links.Sort();
					PinFields.Add(TEXT("links"), FString::Join(Links, TEXT(",")));
				}
			}
		}
	};
	Flatten(Base, Before);
	Flatten(Current, After);
	TSet<FString> KeySet;
	for (const TPair<FString, TMap<FString, FString>>& Pair : Before) KeySet.Add(Pair.Key);
	for (const TPair<FString, TMap<FString, FString>>& Pair : After) KeySet.Add(Pair.Key);
	TArray<FString> Keys = KeySet.Array();
	Keys.Sort();
	auto AddChange = [&Report, MaxChanges](const FString& Kind, const FString& StableId,
		const FString& Field, const FString& BeforeValue, const FString& AfterValue)
	{
		if (Report.Changes.Num() >= MaxChanges)
		{
			Report.bTruncated = true;
			return;
		}
		FHyperAIBlueprintDiffItem& Item = Report.Changes.AddDefaulted_GetRef();
		Item.Kind = Kind;
		Item.StableId = StableId;
		Item.Field = Field;
		Item.Before = BeforeValue;
		Item.After = AfterValue;
	};
	for (const FString& Key : Keys)
	{
		const TMap<FString, FString>* Left = Before.Find(Key);
		const TMap<FString, FString>* Right = After.Find(Key);
		const FString Kind = Left ? Left->FindRef(TEXT("kind")) : Right->FindRef(TEXT("kind"));
		if (!Left)
		{
			AddChange(Kind, Key, TEXT("presence"), TEXT("missing"), TEXT("present"));
			continue;
		}
		if (!Right)
		{
			AddChange(Kind, Key, TEXT("presence"), TEXT("present"), TEXT("missing"));
			continue;
		}
		TSet<FString> FieldSet;
		for (const TPair<FString, FString>& Pair : *Left) if (Pair.Key != TEXT("kind")) FieldSet.Add(Pair.Key);
		for (const TPair<FString, FString>& Pair : *Right) if (Pair.Key != TEXT("kind")) FieldSet.Add(Pair.Key);
		TArray<FString> Fields = FieldSet.Array();
		Fields.Sort();
		for (const FString& Field : Fields)
		{
			const FString LeftValue = Left->FindRef(Field);
			const FString RightValue = Right->FindRef(Field);
			if (LeftValue != RightValue)
			{
				AddChange(Kind, Key, Field, LeftValue, RightValue);
			}
		}
	}
	Report.bOk = true;
	Report.Status = Report.Changes.IsEmpty() ? TEXT("identical")
		: (Report.bTruncated ? TEXT("changes_truncated") : TEXT("changes_found"));
	Report.Diagnostic = TEXT("Deterministic stable-identity diff completed from bounded immutable value snapshots.");
	return Report;
}

FHyperAIBlueprintPatch FHyperAIStudioBlueprintWorkflowContracts::BuildDeterministicRepairPatch(
	const FHyperAIBlueprintPatchSnapshot& Snapshot,
	TArray<FHyperAIBlueprintWorkflowIssue>& OutFindings)
{
	FHyperAIBlueprintPatch Patch;
	Patch.TargetAssetPath = Snapshot.BlueprintAssetPath;
	Patch.ExpectedRevision = Snapshot.Revision;
	OutFindings.Reset();
	auto AddFinding = [&OutFindings](const FString& Code, const FString& Severity,
		const FString& Target, const FString& Message, const double Confidence, const FString& Evidence)
	{
		if (OutFindings.Num() >= FHyperAIStudioBlueprintWorkflowContracts::MaxRepairFindings) return;
		FHyperAIBlueprintWorkflowIssue& Finding = OutFindings.AddDefaulted_GetRef();
		Finding.Code = Code;
		Finding.Severity = Severity;
		Finding.Target = Target;
		Finding.Message = Message;
		Finding.Confidence = Confidence;
		Finding.Evidence = Evidence;
	};
	if (Snapshot.CompileStatus == TEXT("error"))
	{
		AddFinding(TEXT("compile_error_requires_human_choice"), TEXT("error"), Snapshot.BlueprintAssetPath,
			TEXT("Compilation is in error; automatic semantic rewrites are intentionally unavailable."), 0.0,
			TEXT("snapshot.compile_status=error"));
	}
	TMap<FString, const FHyperAIBlueprintPatchPinSnapshot*> PinsByKey;
	for (const FHyperAIBlueprintPatchGraphSnapshot& Graph : Snapshot.Graphs)
	{
		for (const FHyperAIBlueprintPatchNodeSnapshot& Node : Graph.Nodes)
		{
			for (const FHyperAIBlueprintPatchPinSnapshot& Pin : Node.Pins)
			{
				PinsByKey.Add(Pin.Id.StableKey(), &Pin);
			}
		}
	}
	for (const FHyperAIBlueprintPatchGraphSnapshot& Graph : Snapshot.Graphs)
	{
		TMap<FIntPoint, int32> PositionCounts;
		for (const FHyperAIBlueprintPatchNodeSnapshot& Node : Graph.Nodes)
		{
			PositionCounts.FindOrAdd(FIntPoint(Node.X, Node.Y))++;
			if (Node.NodeClassPath.IsEmpty()
				|| Node.NodeClassPath.Contains(TEXT("REINST_"), ESearchCase::CaseSensitive)
				|| Node.NodeClassPath.Contains(TEXT("TRASHCLASS_"), ESearchCase::CaseSensitive))
			{
				AddFinding(TEXT("broken_node_reference"), TEXT("error"), Node.Id.StableKey(),
					TEXT("Node class identity is missing or is a transient broken-reference class; replacement needs semantic intent."),
					0.0, TEXT("node_class_path=") + Node.NodeClassPath);
			}
			else if (Node.NodeClassPath.Contains(TEXT("Deprecated"), ESearchCase::IgnoreCase))
			{
				AddFinding(TEXT("deprecated_node_reference"), TEXT("warning"), Node.Id.StableKey(),
					TEXT("Deprecated node class detected; automatic replacement is unavailable without an exact migration mapping."),
					0.25, TEXT("node_class_path=") + Node.NodeClassPath);
			}

			TOptional<EHyperAIBlueprintCreateNodeKind> KnownKind;
			if (Node.NodeClassPath == TEXT("/Script/BlueprintGraph.K2Node_IfThenElse"))
			{
				KnownKind = EHyperAIBlueprintCreateNodeKind::Branch;
			}
			else if (Node.NodeClassPath == TEXT("/Script/BlueprintGraph.K2Node_ExecutionSequence"))
			{
				KnownKind = EHyperAIBlueprintCreateNodeKind::Sequence;
			}
			else if (Node.NodeClassPath == TEXT("/Script/BlueprintGraph.K2Node_Knot"))
			{
				KnownKind = EHyperAIBlueprintCreateNodeKind::Reroute;
			}
			if (KnownKind.IsSet())
			{
				FHyperAIBlueprintCreateNode Create;
				Create.Graph = Node.Id.Graph;
				Create.NewNodeGuid = Node.Id.NodeGuid;
				Create.Kind = KnownKind.GetValue();
				Create.X = Node.X;
				Create.Y = Node.Y;
				FHyperAIBlueprintPatchNodeSnapshot Expected;
				FString TemplateError;
				if (FHyperAIStudioBlueprintPatch::BuildAllowedNodeTemplate(Create, Expected, TemplateError))
				{
					for (const FHyperAIBlueprintPatchPinSnapshot& ExpectedPin : Expected.Pins)
					{
						if (!Node.Pins.ContainsByPredicate([&ExpectedPin](const FHyperAIBlueprintPatchPinSnapshot& Actual)
						{
							return Actual.Id.PinName == ExpectedPin.Id.PinName
								&& Actual.Id.Direction == ExpectedPin.Id.Direction
								&& Actual.Id.PinIndex == ExpectedPin.Id.PinIndex;
						}))
						{
							AddFinding(TEXT("missing_expected_pin"), TEXT("error"), Node.Id.StableKey(),
								TEXT("A known native K2 node is missing an expected pin; reconstructing it could alter links and is suggestion-only."),
								0.5, TEXT("expected_pin=") + ExpectedPin.Id.PinName.ToString()
									+ TEXT(";direction=")
										+ HyperAIStudio::BlueprintWorkflow::Private::DirectionToken(ExpectedPin.Id.Direction));
						}
					}
				}
			}
			for (const FHyperAIBlueprintPatchPinSnapshot& Pin : Node.Pins)
			{
				if (!Pin.Id.IsValid())
				{
					AddFinding(TEXT("invalid_pin_identity"), TEXT("error"), Node.Id.StableKey(),
						TEXT("Pin lacks a valid stable/revision-bound identity; automated rewiring is unsafe."),
						0.0, TEXT("pin_name=") + Pin.Id.PinName.ToString());
				}
				if (Pin.bOrphaned)
				{
					AddFinding(TEXT("orphaned_pin_suggestion"), TEXT("warning"), Pin.Id.StableKey(),
						TEXT("Orphaned pin detected; deleting or remapping it needs semantic intent."), 0.5,
						TEXT("snapshot.pin.orphaned=true"));
				}
				if (Pin.Id.Direction == EHyperAIBlueprintPinDirection::Input
					&& Pin.LinkedPinKeys.IsEmpty() && Pin.DefaultValue.IsEmpty()
					&& !Pin.bDefaultIgnored && !Pin.bDefaultReadOnly
					&& !Pin.PinType.StartsWith(TEXT("exec/"))
					&& !Pin.PinType.StartsWith(TEXT("wildcard/")))
				{
					AddFinding(TEXT("unconnected_required_pin_candidate"), TEXT("warning"), Pin.Id.StableKey(),
						TEXT("Unconnected input has no literal default; node-specific requiredness cannot be proven from a value snapshot, so this is suggestion-only."),
						0.25, TEXT("links=0;default=empty;type=") + Pin.PinType);
				}
				for (const FString& LinkedKey : Pin.LinkedPinKeys)
				{
					const FHyperAIBlueprintPatchPinSnapshot* const* Other = PinsByKey.Find(LinkedKey);
					if (!Other)
					{
						AddFinding(TEXT("missing_link_target"), TEXT("error"), Pin.Id.StableKey(),
							TEXT("Pin link targets an identity absent from the bounded snapshot; automatic removal is not attempted."),
							0.5, TEXT("missing_link_target=") + LinkedKey);
					}
					else if (!(*Other)->LinkedPinKeys.Contains(Pin.Id.StableKey()))
					{
						AddFinding(TEXT("nonreciprocal_pin_link"), TEXT("error"), Pin.Id.StableKey(),
							TEXT("Pin link is not reciprocal; choosing break versus restore requires graph-schema intent."),
							0.5, TEXT("other_pin=") + LinkedKey + TEXT(";reciprocal=false"));
					}
				}
			}
		}
		int32 DuplicateNodeCount = 0;
		for (const TPair<FIntPoint, int32>& Pair : PositionCounts)
		{
			if (Pair.Value > 1) DuplicateNodeCount += Pair.Value;
		}
		if (DuplicateNodeCount > 0 && Graph.Nodes.Num() > 1
			&& Graph.Nodes.Num() <= FHyperAIStudioBlueprintPatch::MaxLayoutNodes
			&& Patch.Operations.Num() < FHyperAIStudioBlueprintPatch::MaxOperations)
		{
			FHyperAIBlueprintLayoutNodes Layout;
			Layout.Graph = Graph.Id;
			Layout.Columns = FMath::Clamp(FMath::CeilToInt(FMath::Sqrt(static_cast<float>(Graph.Nodes.Num()))), 1, 16);
			Layout.HorizontalSpacing = 400;
			Layout.VerticalSpacing = 240;
			Layout.OriginX = MAX_int32;
			Layout.OriginY = MAX_int32;
			for (const FHyperAIBlueprintPatchNodeSnapshot& Node : Graph.Nodes)
			{
				Layout.Nodes.Add(Node.Id);
				Layout.OriginX = FMath::Min(Layout.OriginX, Node.X);
				Layout.OriginY = FMath::Min(Layout.OriginY, Node.Y);
			}
			FHyperAIBlueprintPatchOperation Operation;
			Operation.Value.Set<FHyperAIBlueprintLayoutNodes>(MoveTemp(Layout));
			Patch.Operations.Add(MoveTemp(Operation));
			AddFinding(TEXT("duplicate_position_safe_layout"), TEXT("warning"), Graph.Id.StableKey(),
				TEXT("Nodes share exact coordinates; a semantics-preserving deterministic full-graph layout is available."),
				1.0, FString::Printf(TEXT("duplicate_node_count=%d;layout=stable_row_major"), DuplicateNodeCount));
		}
	}
	return Patch;
}

FHyperAIBlueprintSnapshotReport UHyperAIStudioBlueprintWorkflowToolset::hyper_blueprint_snapshot(
	const FHyperAIBlueprintSnapshotRequest& Request)
{
	using namespace HyperAIStudio::BlueprintWorkflow::Private;
	FHyperAIBlueprintSnapshotReport Report;
	if (Request.PageSize < 1 || Request.PageSize > FHyperAIStudioBlueprintWorkflowContracts::MaxPageSize
		|| Request.MaxOutputBytes < 1024
		|| Request.MaxOutputBytes > FHyperAIStudioBlueprintWorkflowContracts::MaxOutputBytes
		|| Request.GraphPath.Len() > FHyperAIStudioBlueprintPatch::MaxPathCharacters)
	{
		Report.Status = TEXT("invalid_bounds");
		Report.Diagnostic = TEXT("page_size, graph_path, and max_output_bytes are outside the closed snapshot bounds.");
		return Report;
	}
	FString Token;
	int32 Offset = 0;
	if (!ParseSnapshotCursor(Request.Cursor, Token, Offset))
	{
		Report.Status = TEXT("invalid_cursor");
		Report.Diagnostic = TEXT("cursor is malformed or outside the bounded contract.");
		return Report;
	}
	FSnapshotCacheEntry Entry;
	if (!Request.Cursor.IsEmpty())
	{
		if (!FindSnapshot(Token, Entry))
		{
			Report.Status = TEXT("cursor_expired");
			Report.Diagnostic = TEXT("The immutable snapshot cursor is unknown or expired; request a new first page.");
			return Report;
		}
		if (Entry.AssetPath != Request.AssetPath || Entry.GraphPath != Request.GraphPath
			|| Entry.bIncludePins != Request.bIncludePins || Entry.bIncludeLinks != Request.bIncludeLinks)
		{
			Report.Status = TEXT("cursor_binding_mismatch");
			Report.Diagnostic = TEXT("cursor cannot be replayed with altered asset, graph, pin, or link selectors.");
			return Report;
		}
	}
	else
	{
		FString ResolveStatus;
		FString ResolveDiagnostic;
		UBlueprint* Blueprint = ResolveLoadedBlueprint(Request.AssetPath, ResolveStatus, ResolveDiagnostic);
		if (!Blueprint)
		{
			Report.Status = ResolveStatus;
			Report.Diagnostic = ResolveDiagnostic;
			return Report;
		}
		Entry.Snapshot = FHyperAIStudioBlueprintPatch::CaptureLoaded(Blueprint);
		if (!Entry.Snapshot.bComplete)
		{
			Report.Status = Entry.Snapshot.Status;
			Report.Diagnostic = Clip(Entry.Snapshot.Diagnostic, 512);
			return Report;
		}
		Entry.AssetPath = Request.AssetPath;
		Entry.GraphPath = Request.GraphPath;
		Entry.bIncludePins = Request.bIncludePins;
		Entry.bIncludeLinks = Request.bIncludeLinks;
		Entry.Token = StoreSnapshot(Entry.Snapshot, Request);
		if (!Request.GraphPath.IsEmpty())
		{
			const bool bGraphExists = Entry.Snapshot.Graphs.ContainsByPredicate(
				[&Request](const FHyperAIBlueprintPatchGraphSnapshot& Graph)
				{
					return Graph.Id.GraphPath == Request.GraphPath;
				});
			if (!bGraphExists)
			{
				Report.Status = TEXT("graph_not_found");
				Report.Diagnostic = TEXT("The exact graph_path is absent from the revision-bound snapshot.");
				return Report;
			}
		}
	}
	bool bSourceTruncated = false;
	const TArray<FHyperAIBlueprintSnapshotItem> Items = FlattenSnapshot(
		Entry.Snapshot, Request.GraphPath, Request.bIncludePins, Request.bIncludeLinks, bSourceTruncated);
	if (Offset > Items.Num())
	{
		Report.Status = TEXT("cursor_offset_invalid");
		Report.Diagnostic = TEXT("cursor offset is outside its immutable snapshot.");
		return Report;
	}
	int32 EstimatedBytes = 768;
	int32 Index = Offset;
	for (; Index < Items.Num() && Report.Items.Num() < Request.PageSize; ++Index)
	{
		const int32 ItemBytes = EstimateItemBytes(Items[Index]);
		if (EstimatedBytes + ItemBytes > Request.MaxOutputBytes)
		{
			break;
		}
		Report.Items.Add(Items[Index]);
		EstimatedBytes += ItemBytes;
	}
	if (Index == Offset && Offset < Items.Num())
	{
		Report.Status = TEXT("output_budget_too_small");
		Report.Diagnostic = TEXT("max_output_bytes cannot hold even one bounded snapshot item.");
		return Report;
	}
	Report.bOk = true;
	Report.Status = TEXT("snapshot_ready");
	Report.Diagnostic = TEXT("Returned an immutable loaded-only Blueprint value snapshot page.");
	Report.AssetPath = Entry.Snapshot.BlueprintAssetPath;
	Report.Revision = Entry.Snapshot.Revision;
	Report.CompileStatus = Entry.Snapshot.CompileStatus;
	Report.SnapshotToken = Entry.Token;
	Report.GraphCount = Entry.Snapshot.GraphCount;
	Report.NodeCount = Entry.Snapshot.NodeCount;
	Report.PinCount = Entry.Snapshot.PinCount;
	Report.LinkCount = Entry.Snapshot.LinkCount;
	Report.TotalItems = Items.Num();
	Report.ReturnedItems = Report.Items.Num();
	Report.bTruncated = bSourceTruncated || Index < Items.Num();
	if (Index < Items.Num())
	{
		Report.NextCursor = Entry.Token + TEXT("|") + FString::FromInt(Index);
	}
	return Report;
}

FHyperAIBlueprintActionReport UHyperAIStudioBlueprintWorkflowToolset::hyper_blueprint_discover_actions(
	const FHyperAIBlueprintDiscoverActionsRequest& Request)
{
	using namespace HyperAIStudio::BlueprintWorkflow::Private;
	FHyperAIBlueprintActionReport Report;
	if (Request.Query.Len() > FHyperAIStudioBlueprintWorkflowContracts::MaxQueryCharacters
		|| Request.MaxActions < 1 || Request.MaxActions > 16)
	{
		Report.Status = TEXT("invalid_bounds");
		Report.Diagnostic = TEXT("query or max_actions exceeds the closed allowlist discovery bounds.");
		return Report;
	}
	FHyperAIBlueprintGraphId GraphId;
	FString Error;
	if (!FHyperAIStudioBlueprintWorkflowContracts::ConvertGraphRef(Request.Graph, GraphId, Error))
	{
		Report.Status = TEXT("invalid_graph_identity");
		Report.Diagnostic = Clip(Error, 512);
		return Report;
	}
	FString ResolveStatus;
	FString ResolveDiagnostic;
	UBlueprint* Blueprint = ResolveLoadedBlueprint(Request.AssetPath, ResolveStatus, ResolveDiagnostic);
	if (!Blueprint)
	{
		Report.Status = ResolveStatus;
		Report.Diagnostic = ResolveDiagnostic;
		return Report;
	}
	const FHyperAIBlueprintPatchSnapshot Snapshot = FHyperAIStudioBlueprintPatch::CaptureLoaded(Blueprint);
	const FHyperAIBlueprintPatchGraphSnapshot* Graph = Snapshot.Graphs.FindByPredicate(
		[&GraphId](const FHyperAIBlueprintPatchGraphSnapshot& Candidate)
		{
			return Candidate.Id.StableKey() == GraphId.StableKey();
		});
	if (!Snapshot.bComplete || !Graph || Graph->SchemaClassPath != TEXT("/Script/BlueprintGraph.EdGraphSchema_K2"))
	{
		Report.Status = !Snapshot.bComplete ? Snapshot.Status : TEXT("k2_graph_not_found");
		Report.Diagnostic = !Snapshot.bComplete ? Clip(Snapshot.Diagnostic, 512)
			: TEXT("Action discovery requires one exact already-loaded K2 graph identity.");
		return Report;
	}
	struct FCandidate
	{
		EHyperAIBlueprintCreateNodeKind Kind;
		const TCHAR* Id;
		const TCHAR* Token;
		const TCHAR* Title;
		FGuid Guid;
	};
	const FCandidate Candidates[] = {
		{ EHyperAIBlueprintCreateNodeKind::Branch, TEXT("k2.branch"), TEXT("branch"), TEXT("Branch"), FGuid(0x10000001, 1, 1, 1) },
		{ EHyperAIBlueprintCreateNodeKind::Sequence, TEXT("k2.sequence"), TEXT("sequence"), TEXT("Sequence"), FGuid(0x10000002, 2, 2, 2) },
		{ EHyperAIBlueprintCreateNodeKind::Reroute, TEXT("k2.reroute"), TEXT("reroute"), TEXT("Reroute"), FGuid(0x10000003, 3, 3, 3) }
	};
	for (const FCandidate& Candidate : Candidates)
	{
		const FString Search = FString(Candidate.Token) + TEXT(" ") + Candidate.Title;
		if (!Request.Query.IsEmpty() && !Search.Contains(Request.Query, ESearchCase::IgnoreCase)) continue;
		FHyperAIBlueprintCreateNode Create;
		Create.Graph = GraphId;
		Create.NewNodeGuid = Candidate.Guid;
		Create.Kind = Candidate.Kind;
		FHyperAIBlueprintPatchNodeSnapshot Template;
		if (!FHyperAIStudioBlueprintPatch::BuildAllowedNodeTemplate(Create, Template, Error)) continue;
		FHyperAIBlueprintActionRecord& Action = Report.Actions.AddDefaulted_GetRef();
		Action.ActionId = Candidate.Id;
		Action.NodeKind = Candidate.Token;
		Action.Title = Candidate.Title;
		Action.NodeClassPath = Template.NodeClassPath;
		for (const FHyperAIBlueprintPatchPinSnapshot& Pin : Template.Pins)
		{
			FHyperAIBlueprintSnapshotItem& Item = Action.Pins.AddDefaulted_GetRef();
			Item.Kind = TEXT("pin_template");
			Item.StableId = Pin.Id.StableKey();
			Item.Name = Pin.Id.PinName.ToString();
			Item.PinType = Pin.PinType;
			Item.Direction = DirectionToken(Pin.Id.Direction);
			Item.DefaultValue = Pin.DefaultValue;
		}
		if (Report.Actions.Num() >= Request.MaxActions) break;
	}
	Report.bOk = true;
	Report.Status = TEXT("actions_ready");
	Report.Diagnostic = TEXT("Returned only the bounded native K2 node-creation allowlist; no reflected/raw action execution is exposed.");
	Report.AssetPath = Snapshot.BlueprintAssetPath;
	Report.Revision = Snapshot.Revision;
	return Report;
}

FHyperAIBlueprintPatchReport UHyperAIStudioBlueprintWorkflowToolset::hyper_blueprint_validate_patch(
	const FHyperAIBlueprintPatchInput& Patch)
{
	return FHyperAIStudioBlueprintWorkflowContracts::ValidatePatchLoaded(Patch);
}

FHyperAIBlueprintPatchReport UHyperAIStudioBlueprintWorkflowToolset::hyper_blueprint_apply_patch(
	const FHyperAIBlueprintMutationRequest& Request)
{
	return HyperAIStudio::BlueprintWorkflow::Private::StageAndSubmit(
		TEXT("hyper_blueprint_apply_patch"), Request, false);
}

FHyperAIBlueprintPatchReport UHyperAIStudioBlueprintWorkflowToolset::hyper_blueprint_build_batch(
	const FHyperAIBlueprintMutationRequest& Request)
{
	return HyperAIStudio::BlueprintWorkflow::Private::StageAndSubmit(
		TEXT("hyper_blueprint_build_batch"), Request, true);
}

namespace HyperAIStudio::BlueprintWorkflow::Private
{
	FHyperAIBlueprintPatchReport PreviewPreparedPatch(
		UBlueprint* Blueprint,
		const FHyperAIBlueprintPatch& Patch,
		const FHyperAIBlueprintPatchPlan& Plan)
	{
		FHyperAIBlueprintPatchReport Report;
		Report.bDryRun = true;
		Report.RevisionBefore = Plan.BaseRevision;
		Report.PatchPlanHash = Plan.PlanHash;
		Report.PatchEffectFingerprint = FHyperAIStudioBlueprintPatch::ComputeEffectFingerprint(Plan);
		Report.Effects = ToEffectReport(Plan);
		CopyIssues(Plan.Issues, Report);
		if (!Plan.bValid || Plan.Safety != EHyperAIBlueprintPatchSafety::Edit)
		{
			Report.Status = Plan.Status.IsEmpty() ? TEXT("patch_invalid") : Plan.Status;
			Report.Diagnostic = TEXT("Prepared edit patch did not pass revision-bound planning.");
			return Report;
		}
		if (EffectCount(Plan) <= 0)
		{
			Report.bOk = true;
			Report.Status = TEXT("no_changes");
			Report.Diagnostic = TEXT("The exact loaded revision already satisfies the prepared operation.");
			return Report;
		}
		const FHyperAIBlueprintPatchResult DryRun = FHyperAIStudioBlueprintPatch::DryRunLoaded(
			Blueprint, Patch, FHyperAIStudioBlueprintPatch::EditTypedOperationType);
		if (!DryRun.bOk || DryRun.PlanHash != Plan.PlanHash)
		{
			Report.Status = DryRun.Status.IsEmpty() ? TEXT("live_patch_rejected") : DryRun.Status;
			Report.Diagnostic = DryRun.Diagnostic.IsEmpty()
				? TEXT("Prepared patch no longer matches the live revision/schema.") : Clip(DryRun.Diagnostic, 512);
			return Report;
		}
		const FString PreviewOperationId = TEXT("blueprint-preview-") + Plan.PlanHash.Mid(7, 24);
		FString PlanJson;
		FHyperAIStudioValidatedPlan TypedPlan;
		FString Error;
		if (!BuildBlueprintPlanJson(Patch, Plan, PreviewOperationId, true, FString(), PlanJson, TypedPlan, Error))
		{
			Report.Status = TEXT("execution_plan_invalid");
			Report.Diagnostic = Clip(Error, 512);
			return Report;
		}
		Report.bOk = true;
		Report.Status = TEXT("valid_dry_run");
		Report.Diagnostic = TEXT("Closed native operations and exact execution/effect fingerprints validated without mutation.");
		Report.PlanHash = TypedPlan.PlanHash;
		Report.EffectFingerprint = TypedPlan.EffectFingerprint;
		return Report;
	}
}

FHyperAIBlueprintPatchReport UHyperAIStudioBlueprintWorkflowToolset::hyper_blueprint_compile_validate(
	const FHyperAIBlueprintCompileValidateRequest& Request)
{
	using namespace HyperAIStudio::BlueprintWorkflow::Private;
	FString Status;
	FString Diagnostic;
	UBlueprint* Blueprint = ResolveLoadedBlueprint(Request.AssetPath, Status, Diagnostic);
	if (!Blueprint)
	{
		FHyperAIBlueprintPatchReport Report;
		Report.Status = Status;
		Report.Diagnostic = Diagnostic;
		return Report;
	}
	const FHyperAIBlueprintPatchSnapshot Snapshot = FHyperAIStudioBlueprintPatch::CaptureLoaded(Blueprint);
	if (!Snapshot.bComplete || !IsSha256(Request.ExpectedRevision)
		|| Snapshot.Revision != Request.ExpectedRevision)
	{
		FHyperAIBlueprintPatchReport Report;
		Report.Status = !Snapshot.bComplete ? Snapshot.Status : TEXT("revision_mismatch");
		Report.Diagnostic = !Snapshot.bComplete ? Clip(Snapshot.Diagnostic, 512)
			: TEXT("compile_validate requires the exact current Blueprint revision.");
		Report.RevisionBefore = Snapshot.Revision;
		return Report;
	}
	FHyperAIBlueprintPatch Patch;
	Patch.TargetAssetPath = Snapshot.BlueprintAssetPath;
	Patch.ExpectedRevision = Snapshot.Revision;
	FHyperAIBlueprintPatchOperation Compile;
	Compile.Value.Set<FHyperAIBlueprintCompileOnly>(FHyperAIBlueprintCompileOnly());
	Patch.Operations.Add(MoveTemp(Compile));
	const FHyperAIBlueprintPatchPlan Plan = FHyperAIStudioBlueprintPatch::PlanSnapshot(Snapshot, Patch);
	FHyperAIBlueprintPatchReport Preview = PreviewPreparedPatch(Blueprint, Patch, Plan);
	if (!Preview.bOk || Request.ExpectedPlanHash.IsEmpty())
	{
		return Preview;
	}
	return StagePreparedAndSubmit(TEXT("hyper_blueprint_compile_validate"), Patch, Plan,
		Request.OperationId, Request.ExpectedPlanHash, Request.AuthorizationToken, false);
}

FHyperAIBlueprintPatchReport UHyperAIStudioBlueprintWorkflowToolset::hyper_blueprint_repair(
	const FHyperAIBlueprintRepairRequest& Request)
{
	using namespace HyperAIStudio::BlueprintWorkflow::Private;
	FHyperAIBlueprintPatchReport Report;
	if (Request.Mode != TEXT("suggest") && Request.Mode != TEXT("stage_safe"))
	{
		Report.Status = TEXT("invalid_repair_mode");
		Report.Diagnostic = TEXT("mode must be suggest or stage_safe.");
		return Report;
	}
	FString ResolveStatus;
	FString ResolveDiagnostic;
	UBlueprint* Blueprint = ResolveLoadedBlueprint(Request.AssetPath, ResolveStatus, ResolveDiagnostic);
	if (!Blueprint)
	{
		Report.Status = ResolveStatus;
		Report.Diagnostic = ResolveDiagnostic;
		return Report;
	}
	const FHyperAIBlueprintPatchSnapshot Snapshot = FHyperAIStudioBlueprintPatch::CaptureLoaded(Blueprint);
	if (!Snapshot.bComplete || !IsSha256(Request.ExpectedRevision)
		|| Request.ExpectedRevision != Snapshot.Revision)
	{
		Report.Status = !Snapshot.bComplete ? Snapshot.Status : TEXT("revision_mismatch");
		Report.Diagnostic = !Snapshot.bComplete ? Clip(Snapshot.Diagnostic, 512)
			: TEXT("repair analysis is bound to the exact current revision.");
		Report.RevisionBefore = Snapshot.Revision;
		return Report;
	}
	TArray<FHyperAIBlueprintWorkflowIssue> Findings;
	const FHyperAIBlueprintPatch Patch =
		FHyperAIStudioBlueprintWorkflowContracts::BuildDeterministicRepairPatch(Snapshot, Findings);
	if (Patch.Operations.IsEmpty())
	{
		Report.bOk = true;
		Report.bDryRun = true;
		Report.Status = Findings.IsEmpty() ? TEXT("no_repairs_needed") : TEXT("suggestions_only");
		Report.Diagnostic = Findings.IsEmpty()
			? TEXT("No deterministic repair finding was present.")
			: TEXT("Findings need semantic intent; no mutation was staged.");
		Report.RevisionBefore = Snapshot.Revision;
		Report.Issues = MoveTemp(Findings);
		return Report;
	}
	const FHyperAIBlueprintPatchPlan Plan = FHyperAIStudioBlueprintPatch::PlanSnapshot(Snapshot, Patch);
	Report = PreviewPreparedPatch(Blueprint, Patch, Plan);
	for (const FHyperAIBlueprintWorkflowIssue& Finding : Findings)
	{
		if (Report.Issues.Num() < FHyperAIStudioBlueprintWorkflowContracts::MaxRepairFindings)
		{
			Report.Issues.Add(Finding);
		}
	}
	if (!Report.bOk || Request.Mode == TEXT("suggest"))
	{
		return Report;
	}
	FHyperAIBlueprintPatchReport Submitted = StagePreparedAndSubmit(TEXT("hyper_blueprint_repair"),
		Patch, Plan, Request.OperationId, Request.ExpectedPlanHash, Request.AuthorizationToken, false);
	for (const FHyperAIBlueprintWorkflowIssue& Finding : Findings)
	{
		if (Submitted.Issues.Num() < FHyperAIStudioBlueprintWorkflowContracts::MaxRepairFindings)
		{
			Submitted.Issues.Add(Finding);
		}
	}
	return Submitted;
}

FHyperAIBlueprintDiffReport UHyperAIStudioBlueprintWorkflowToolset::hyper_blueprint_diff(
	const FHyperAIBlueprintDiffRequest& Request)
{
	using namespace HyperAIStudio::BlueprintWorkflow::Private;
	FHyperAIBlueprintDiffReport Report;
	if (Request.MaxChanges < 1 || Request.MaxChanges > FHyperAIStudioBlueprintWorkflowContracts::MaxDiffChanges
		|| Request.BaseSnapshotToken.Len() > FHyperAIStudioBlueprintWorkflowContracts::MaxCursorCharacters)
	{
		Report.Status = TEXT("invalid_bounds");
		Report.Diagnostic = TEXT("base_snapshot_token or max_changes exceeds the diff contract bounds.");
		return Report;
	}
	FSnapshotCacheEntry Base;
	if (!FindSnapshot(Request.BaseSnapshotToken, Base))
	{
		Report.Status = TEXT("snapshot_token_expired");
		Report.Diagnostic = TEXT("Base snapshot is unknown or expired; acquire a new snapshot token.");
		return Report;
	}
	if (Base.AssetPath != Request.AssetPath)
	{
		Report.Status = TEXT("snapshot_binding_mismatch");
		Report.Diagnostic = TEXT("Base snapshot token is bound to a different exact Blueprint asset.");
		return Report;
	}
	FString ResolveStatus;
	FString ResolveDiagnostic;
	UBlueprint* Blueprint = ResolveLoadedBlueprint(Request.AssetPath, ResolveStatus, ResolveDiagnostic);
	if (!Blueprint)
	{
		Report.Status = ResolveStatus;
		Report.Diagnostic = ResolveDiagnostic;
		return Report;
	}
	const FHyperAIBlueprintPatchSnapshot Current = FHyperAIStudioBlueprintPatch::CaptureLoaded(Blueprint);
	return FHyperAIStudioBlueprintWorkflowContracts::DiffSnapshots(
		Base.Snapshot, Current, Request.MaxChanges);
}

FHyperAIBlueprintPatchReport UHyperAIStudioBlueprintWorkflowToolset::hyper_blueprint_layout(
	const FHyperAIBlueprintLayoutRequest& Request)
{
	using namespace HyperAIStudio::BlueprintWorkflow::Private;
	FHyperAIBlueprintPatchReport Report;
	FHyperAIBlueprintGraphId Graph;
	FString Error;
	if (!FHyperAIStudioBlueprintWorkflowContracts::ConvertGraphRef(Request.Graph, Graph, Error)
		|| Request.Nodes.IsEmpty() || Request.Nodes.Num() > FHyperAIStudioBlueprintPatch::MaxLayoutNodes)
	{
		Report.Status = TEXT("invalid_layout_contract");
		Report.Diagnostic = Error.IsEmpty() ? TEXT("layout needs 1..1024 exact stable node identities.") : Clip(Error, 512);
		return Report;
	}
	FString ResolveStatus;
	FString ResolveDiagnostic;
	UBlueprint* Blueprint = ResolveLoadedBlueprint(Request.AssetPath, ResolveStatus, ResolveDiagnostic);
	if (!Blueprint)
	{
		Report.Status = ResolveStatus;
		Report.Diagnostic = ResolveDiagnostic;
		return Report;
	}
	const FHyperAIBlueprintPatchSnapshot Snapshot = FHyperAIStudioBlueprintPatch::CaptureLoaded(Blueprint);
	if (!Snapshot.bComplete || !IsSha256(Request.ExpectedRevision)
		|| Request.ExpectedRevision != Snapshot.Revision)
	{
		Report.Status = !Snapshot.bComplete ? Snapshot.Status : TEXT("revision_mismatch");
		Report.Diagnostic = !Snapshot.bComplete ? Clip(Snapshot.Diagnostic, 512)
			: TEXT("layout requires the exact current Blueprint revision.");
		Report.RevisionBefore = Snapshot.Revision;
		return Report;
	}
	FHyperAIBlueprintLayoutNodes Layout;
	Layout.Graph = Graph;
	Layout.OriginX = Request.OriginX;
	Layout.OriginY = Request.OriginY;
	Layout.Columns = Request.Columns;
	Layout.HorizontalSpacing = Request.HorizontalSpacing;
	Layout.VerticalSpacing = Request.VerticalSpacing;
	TSet<FString> Seen;
	for (const FHyperAIBlueprintNodeRef& NodeInput : Request.Nodes)
	{
		FHyperAIBlueprintNodeId Node;
		if (!FHyperAIStudioBlueprintWorkflowContracts::ConvertNodeRef(NodeInput, Node, Error)
			|| Node.Graph.StableKey() != Graph.StableKey() || Seen.Contains(Node.StableKey()))
		{
			Report.Status = TEXT("invalid_layout_node");
			Report.Diagnostic = Error.IsEmpty()
				? TEXT("layout nodes must be unique and belong to the exact graph.") : Clip(Error, 512);
			return Report;
		}
		Seen.Add(Node.StableKey());
		Layout.Nodes.Add(Node);
	}
	FHyperAIBlueprintPatch Patch;
	Patch.TargetAssetPath = Snapshot.BlueprintAssetPath;
	Patch.ExpectedRevision = Snapshot.Revision;
	FHyperAIBlueprintPatchOperation Operation;
	Operation.Value.Set<FHyperAIBlueprintLayoutNodes>(MoveTemp(Layout));
	Patch.Operations.Add(MoveTemp(Operation));
	const FHyperAIBlueprintPatchPlan Plan = FHyperAIStudioBlueprintPatch::PlanSnapshot(Snapshot, Patch);
	Report = PreviewPreparedPatch(Blueprint, Patch, Plan);
	if (!Report.bOk || Request.ExpectedPlanHash.IsEmpty())
	{
		return Report;
	}
	return StagePreparedAndSubmit(TEXT("hyper_blueprint_layout"), Patch, Plan,
		Request.OperationId, Request.ExpectedPlanHash, Request.AuthorizationToken, false);
}

void FHyperAIStudioBlueprintWorkflowRegistration::Startup()
{
	if (bStarted)
	{
		return;
	}
	bStarted = true;
	PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(
		this, &FHyperAIStudioBlueprintWorkflowRegistration::RegisterAfterEngineInit);
	if (GIsRunning && GEngine && UObjectInitialized())
	{
		RegisterAfterEngineInit();
	}
}

void FHyperAIStudioBlueprintWorkflowRegistration::Shutdown()
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
			UHyperAIStudioBlueprintWorkflowToolset::StaticClass(),
			FHyperAIStudioBlueprintWorkflowContracts::GetQualifiedToolsetName(),
			Error))
		{
			UE_LOG(LogHyperAIStudioBlueprintWorkflow, Warning,
				TEXT("Could not unregister the owned Blueprint workflow toolset: %s"), *Error);
		}
	}
	bOwnsRegistration = false;
	bStarted = false;
}

bool FHyperAIStudioBlueprintWorkflowRegistration::IsRegistered() const
{
	return UObjectInitialized() && UToolsetRegistry::IsAvailable()
		&& FHyperAIStudioBlueprintWorkflowContracts::IsRegistrationAllowed(
			FHyperAIStudioBlueprintWorkflowContracts::IsPendingTestRegistrationEnabled())
		&& FHyperAIStudioCapabilityRuntimeIndex::IsOwned(
			UHyperAIStudioBlueprintWorkflowToolset::StaticClass(),
			FHyperAIStudioBlueprintWorkflowContracts::GetQualifiedToolsetName());
}

void FHyperAIStudioBlueprintWorkflowRegistration::RegisterAfterEngineInit()
{
	if (!bStarted || !IsInGameThread() || IsEngineExitRequested() || !UObjectInitialized()
		|| !UToolsetRegistry::IsAvailable()
		|| !FHyperAIStudioBlueprintWorkflowContracts::IsRegistrationAllowed(
			FHyperAIStudioBlueprintWorkflowContracts::IsPendingTestRegistrationEnabled()))
	{
		return;
	}
	if (!FHyperAIStudioCapabilityRuntimeIndex::IsOwned(
		UHyperAIStudioBlueprintWorkflowToolset::StaticClass(),
		FHyperAIStudioBlueprintWorkflowContracts::GetQualifiedToolsetName()))
	{
		FString Error;
		bOwnsRegistration = FHyperAIStudioCapabilityRuntimeIndex::RegisterOwnedToolsetClass(
			UHyperAIStudioBlueprintWorkflowToolset::StaticClass(),
			FHyperAIStudioBlueprintWorkflowContracts::GetQualifiedToolsetName(),
			Error);
		if (!bOwnsRegistration)
		{
			UE_LOG(LogHyperAIStudioBlueprintWorkflow, Error,
				TEXT("ToolsetRegistry rejected the owned nine-tool Blueprint admission cohort: %s"),
				*Error);
		}
	}
}
