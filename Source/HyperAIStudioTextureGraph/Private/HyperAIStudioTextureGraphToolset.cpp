// Games by Hyper 2026.

#include "HyperAIStudioTextureGraphToolset.h"

#include "Engine/Engine.h"
#include "FileHelpers.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioTextureGraphGate.h"
#include "Misc/CoreDelegates.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "TextureGraph.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioTextureGraph, Log, All);

namespace HyperAIStudio::TextureGraph::Private
{
	namespace Gate = HyperAIStudio::TextureGraph::Gate;

	void AppendToken(FString& Canonical, const FString& Value)
	{
		Canonical += FString::FromInt(Value.Len());
		Canonical += TEXT(":");
		Canonical += Value;
		Canonical += TEXT("\n");
	}

	int64 EstimateBytes(const FString& Value)
	{
		return 16 + 2ll * Value.Len();
	}

	int64 EstimateIssueBytes(const FHyperAITextureGraphIssue& Issue)
	{
		return 48 + EstimateBytes(Issue.Code) + EstimateBytes(Issue.Severity) + EstimateBytes(Issue.Pin) + EstimateBytes(Issue.Message);
	}

	int64 EstimateOutputBytes(const FHyperAITextureGraphOutput& Output)
	{
		return 64 + EstimateBytes(Output.OutputName) + EstimateBytes(Output.BaseName) + EstimateBytes(Output.FolderPath)
			+ EstimateBytes(Output.TextureFormat) + EstimateBytes(Output.TexturePath) + EstimateBytes(Output.TextureState);
	}

	int64 EstimateNodeBytes(const FHyperAITextureGraphNode& Node)
	{
		int64 Size = 64 + EstimateBytes(Node.ExpressionClass) + EstimateBytes(Node.Title) + EstimateBytes(Node.Comment);
		for (const FHyperAITextureGraphPin& Pin : Node.Pins)
		{
			Size += 32 + EstimateBytes(Pin.Name) + EstimateBytes(Pin.Alias) + EstimateBytes(Pin.Direction)
				+ EstimateBytes(Pin.CppType) + EstimateBytes(Pin.Value);
			for (const FString& Value : Pin.EnumValues)
			{
				Size += EstimateBytes(Value);
			}
		}
		return Size;
	}

	int32 ClampBytes(const int64 Size)
	{
		return static_cast<int32>(FMath::Clamp<int64>(Size, 1, MAX_int32));
	}

	FHyperAIStudioDomainAdapterResult ExecuteEditPhase(
		const EHyperAIStudioDomainExecutionActionKind Phase,
		const FHyperAIStudioTextureGraphEditOpsPayload& Payload)
	{
		FHyperAIStudioDomainAdapterResult Result;
		const double Started = FPlatformTime::Seconds();
		auto Finish = [&](const EHyperAIStudioDomainDispatchOutcome Outcome, const FString& Status,
			const FString& Diagnostic, const TCHAR* ResultPhase = nullptr, const FString& ContentKey = FString())
		{
			UE_LOG(LogHyperAIStudioTextureGraph, Log, TEXT("Edit phase %d finished %s in %.1f ms. %s"),
				static_cast<int32>(Phase), *Status, (FPlatformTime::Seconds() - Started) * 1000.0, *Diagnostic);
			Result.Outcome = Outcome;
			Result.StatusCode = Status.Left(FHyperAIStudioDomainLimits::MaxStatusCodeChars);
			Result.Diagnostic = Diagnostic.Left(FHyperAIStudioDomainLimits::MaxDiagnosticChars);
			if (Outcome == EHyperAIStudioDomainDispatchOutcome::Succeeded)
			{
				const TSharedRef<FHyperAIStudioTextureGraphMutationResultPayload, ESPMode::ThreadSafe> Output =
					MakeShared<FHyperAIStudioTextureGraphMutationResultPayload, ESPMode::ThreadSafe>();
				Output->Phase = ResultPhase;
				Output->ContentKey = ContentKey;
				Result.Payload = Output;
			}
			return Result;
		};
		const bool bApply = Phase == EHyperAIStudioDomainExecutionActionKind::Apply;
		const EHyperAIStudioDomainDispatchOutcome FailedOutcome = bApply
			? EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect
			: EHyperAIStudioDomainDispatchOutcome::FailedAfterKnownEffect;
		if (!IsInGameThread())
		{
			return Finish(FailedOutcome, TEXT("game_thread_required"), TEXT("Texture Graph edits run on the game thread."));
		}

		UTextureGraph* Graph = Gate::ResolveGraph(Payload.TargetPath, /*bLoad=*/false);
		if (bApply)
		{
			if (Payload.bCreate)
			{
				if (Graph || FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(Payload.TargetPath)))
				{
					return Finish(EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect, TEXT("target_exists"),
						TEXT("An asset appeared at target_path after planning; nothing was created."));
				}
				FString Error;
				Graph = Gate::CreateGraphAsset(Payload.TargetPath, Error);
				if (!Graph)
				{
					return Finish(EHyperAIStudioDomainDispatchOutcome::FailedBeforeEffect, TEXT("create_failed"), Error);
				}
			}
			else if (!Graph || Gate::ComputeRevision(*Graph) != Payload.BaseRevision)
			{
				return Finish(EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect, TEXT("stale_revision"),
					TEXT("The graph changed or unloaded after planning; nothing was applied."));
			}
			else if (Gate::IsOpenInEditor(*Graph))
			{
				return Finish(EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect, TEXT("target_open_in_editor"),
					TEXT("The graph was opened in the Texture Graph editor after planning; nothing was applied."));
			}
			TMap<FString, int32> Keys;
			int32 Applied = 0;
			FString Error;
			if (!Gate::ApplyOps(*Graph, Payload.Ops, Payload.bAutoLayout, /*bTransact=*/true, Keys, Applied, Error))
			{
				return Finish(Applied > 0 || Payload.bCreate
					? EHyperAIStudioDomainDispatchOutcome::FailedAfterKnownEffect
					: EHyperAIStudioDomainDispatchOutcome::FailedBeforeEffect,
					TEXT("edit_op_failed"), Error);
			}
			return Finish(EHyperAIStudioDomainDispatchOutcome::Succeeded, TEXT("applied"),
				FString::Printf(TEXT("%d ops applied."), Applied), TEXT("applied"));
		}
		if (!Graph)
		{
			return Finish(FailedOutcome, TEXT("target_not_loaded"), TEXT("The graph is no longer loaded."));
		}

		switch (Phase)
		{
		case EHyperAIStudioDomainExecutionActionKind::Compile:
		{
			// The executor's order is fixed (apply, compile, validate, save, verify), so export takes the compile slot.
			// Epic's exporter duplicates the graph now and renders over later ticks; hyper_texture_graph_validate with
			// policy exported confirms the textures.
			FString Error;
			if (!Gate::StartExport(*Graph, Error))
			{
				return Finish(EHyperAIStudioDomainDispatchOutcome::FailedAfterKnownEffect, TEXT("export_not_started"), Error);
			}
			return Finish(EHyperAIStudioDomainDispatchOutcome::Succeeded, TEXT("export_started"),
				TEXT("Export started; it finishes asynchronously."), TEXT("export_started"));
		}
		case EHyperAIStudioDomainExecutionActionKind::Validate:
		{
			int32 Errors = 0;
			FString First;
			for (const FHyperAITextureGraphIssue& Issue : Gate::Validate(*Graph, /*bRequireExported=*/false))
			{
				if (Issue.Severity == TEXT("error") && Errors++ == 0)
				{
					First = Issue.Code + TEXT(": ") + Issue.Message;
				}
			}
			if (Errors > 0)
			{
				return Finish(EHyperAIStudioDomainDispatchOutcome::FailedAfterKnownEffect, TEXT("graph_invalid"),
					FString::Printf(TEXT("%d errors, first %s The edit is in memory and was not saved; undo reverts it."), Errors, *First));
			}
			return Finish(EHyperAIStudioDomainDispatchOutcome::Succeeded, TEXT("validated"), TEXT("No structural errors."), TEXT("validated"));
		}
		case EHyperAIStudioDomainExecutionActionKind::Save:
			if (!UEditorLoadingAndSavingUtils::SavePackages({Graph->GetOutermost()}, /*bOnlyDirty=*/false))
			{
				return Finish(EHyperAIStudioDomainDispatchOutcome::FailedAfterKnownEffect, TEXT("save_failed"),
					TEXT("The edit applied but the package did not save; it remains in memory."));
			}
			return Finish(EHyperAIStudioDomainDispatchOutcome::Succeeded, TEXT("saved"), TEXT("Package saved."), TEXT("saved"));
		case EHyperAIStudioDomainExecutionActionKind::VerifyFresh:
			return Finish(EHyperAIStudioDomainDispatchOutcome::Succeeded, TEXT("fresh_captured"),
				TEXT("Fresh content captured for verification."), TEXT("completed"), Gate::ComputeContentKey(*Graph));
		default:
			return Finish(FailedOutcome, TEXT("unsupported_phase"), TEXT("Unknown execution phase."));
		}
	}
}

FString FHyperAIStudioTextureGraphContracts::GetQualifiedToolsetName()
{
	return TEXT("HyperAIStudioTextureGraph.HyperAIStudioTextureGraphToolset");
}

TArray<FString> FHyperAIStudioTextureGraphContracts::GetToolNames()
{
	return {InspectToolName, MutationToolName, ValidateToolName};
}

bool FHyperAIStudioTextureGraphContracts::IsRegistrationAllowed(const bool bAllowSourceCandidateForDev)
{
	return FHyperAIStudioExtensionRuntime::IsExactGeneratedCohortRegistrationAllowed(
		PackId, AtomicCohortId, GetToolNames(), bAllowSourceCandidateForDev);
}

bool FHyperAIStudioTextureGraphContracts::IsCanonicalProjectObjectPath(const FString& Path)
{
	if (Path.IsEmpty() || Path.Len() > MaxPathCharacters || !Path.StartsWith(TEXT("/Game/"))
		|| Path.Contains(TEXT("*")) || Path.Contains(TEXT("?")) || Path.Contains(TEXT(":")))
	{
		return false;
	}
	for (const TCHAR Character : Path)
	{
		if (Character < 0x20 || Character == 0x7F)
		{
			return false;
		}
	}
	FText Reason;
	if (!FPackageName::IsValidObjectPath(Path, &Reason))
	{
		return false;
	}
	const FString PackageName = FPackageName::ObjectPathToPackageName(Path);
	const FString ObjectName = FPackageName::ObjectPathToObjectName(Path);
	return !PackageName.IsEmpty() && !ObjectName.IsEmpty()
		&& FPackageName::GetLongPackageAssetName(PackageName) == ObjectName;
}

bool FHyperAIStudioTextureGraphContracts::IsCanonicalSha256(const FString& Value)
{
	if (Value.Len() != 71 || !Value.StartsWith(TEXT("sha256:")))
	{
		return false;
	}
	for (int32 Index = 7; Index < Value.Len(); ++Index)
	{
		const TCHAR Character = Value[Index];
		if (!((Character >= TEXT('0') && Character <= TEXT('9')) || (Character >= TEXT('a') && Character <= TEXT('f'))))
		{
			return false;
		}
	}
	return true;
}

FString FHyperAIStudioTextureGraphContracts::InspectPayloadSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("texture_graph.inspect.payload.v1|exact_target|optional_load|pins|expression_catalog|output_bytes"));
	return Value;
}

FString FHyperAIStudioTextureGraphContracts::EditOpsPayloadSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("texture_graph.edit_ops.payload.v1|one_target|base_revision_or_create|ordered_closed_ops|auto_layout|export|save|deep_clone|bounded"));
	return Value;
}

FString FHyperAIStudioTextureGraphContracts::ValidatePayloadSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("texture_graph.validate.payload.v1|exact_target|optional_revision|closed_policy|output_bytes"));
	return Value;
}

FString FHyperAIStudioTextureGraphContracts::InspectResultSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("texture_graph.inspect.result.v1|revision|package_state|nodes_pins_values|edges|outputs|export_state|catalog|bounded"));
	return Value;
}

FString FHyperAIStudioTextureGraphContracts::MutationResultSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("texture_graph.edit_ops.result.v1|phase|content_key|bounded"));
	return Value;
}

FString FHyperAIStudioTextureGraphContracts::ValidateResultSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("texture_graph.validate.result.v1|valid|policy|revision|counts|outputs|issues|bounded"));
	return Value;
}

FString FHyperAIStudioTextureGraphInspectPayload::GetTypeId() const
{
	return FHyperAIStudioTextureGraphContracts::InspectPayloadTypeId;
}

FString FHyperAIStudioTextureGraphInspectPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioTextureGraphContracts::InspectPayloadSchemaFingerprint();
}

int32 FHyperAIStudioTextureGraphInspectPayload::GetBoundedByteSize() const
{
	return HyperAIStudio::TextureGraph::Private::ClampBytes(96 + 2ll * Request.TargetPath.Len());
}

FString FHyperAIStudioTextureGraphValidatePayload::GetTypeId() const
{
	return FHyperAIStudioTextureGraphContracts::ValidatePayloadTypeId;
}

FString FHyperAIStudioTextureGraphValidatePayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioTextureGraphContracts::ValidatePayloadSchemaFingerprint();
}

int32 FHyperAIStudioTextureGraphValidatePayload::GetBoundedByteSize() const
{
	return HyperAIStudio::TextureGraph::Private::ClampBytes(
		96 + 2ll * (Request.TargetPath.Len() + Request.ExpectedRevision.Len() + Request.Policy.Len()));
}

FString FHyperAIStudioTextureGraphEditOpsPayload::GetTypeId() const
{
	return FHyperAIStudioTextureGraphContracts::EditOpsPayloadTypeId;
}

FString FHyperAIStudioTextureGraphEditOpsPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioTextureGraphContracts::EditOpsPayloadSchemaFingerprint();
}

int32 FHyperAIStudioTextureGraphEditOpsPayload::GetBoundedByteSize() const
{
	using HyperAIStudio::TextureGraph::Private::EstimateBytes;
	int64 Size = 128 + EstimateBytes(TargetPath) + EstimateBytes(BaseRevision) + EstimateBytes(SemanticFingerprint);
	for (const FHyperAITextureGraphEditOp& Op : Ops)
	{
		Size += 64 + EstimateBytes(Op.Kind) + EstimateBytes(Op.NodeKey) + EstimateBytes(Op.ExpressionClass)
			+ EstimateBytes(Op.Node) + EstimateBytes(Op.Pin) + EstimateBytes(Op.ToNode) + EstimateBytes(Op.ToPin)
			+ EstimateBytes(Op.Value) + EstimateBytes(Op.BaseName) + EstimateBytes(Op.FolderPath)
			+ EstimateBytes(Op.TextureFormat);
	}
	return HyperAIStudio::TextureGraph::Private::ClampBytes(Size);
}

FString FHyperAIStudioTextureGraphEditOpsPayload::GetSemanticFingerprint() const
{
	return SemanticFingerprint;
}

TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe>
FHyperAIStudioTextureGraphEditOpsPayload::CloneImmutable() const
{
	const TSharedRef<FHyperAIStudioTextureGraphEditOpsPayload, ESPMode::ThreadSafe> Clone =
		MakeShared<FHyperAIStudioTextureGraphEditOpsPayload, ESPMode::ThreadSafe>();
	Clone->TargetPath = TargetPath;
	Clone->BaseRevision = BaseRevision;
	Clone->bCreate = bCreate;
	Clone->Ops = Ops;
	Clone->bAutoLayout = bAutoLayout;
	Clone->bExport = bExport;
	Clone->bSave = bSave;
	Clone->SemanticFingerprint = SemanticFingerprint;
	return Clone;
}

FString FHyperAIStudioTextureGraphInspectResultPayload::GetTypeId() const
{
	return FHyperAIStudioTextureGraphContracts::InspectResultTypeId;
}

FString FHyperAIStudioTextureGraphInspectResultPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioTextureGraphContracts::InspectResultSchemaFingerprint();
}

int32 FHyperAIStudioTextureGraphInspectResultPayload::GetBoundedByteSize() const
{
	using namespace HyperAIStudio::TextureGraph::Private;
	int64 Size = 512 + EstimateBytes(Report.Status) + EstimateBytes(Report.Diagnostic) + EstimateBytes(Report.Revision);
	for (const FHyperAITextureGraphNode& Node : Report.Nodes) Size += EstimateNodeBytes(Node);
	for (const FHyperAITextureGraphEdge& Edge : Report.Edges) Size += 32 + EstimateBytes(Edge.FromPin) + EstimateBytes(Edge.ToPin);
	for (const FHyperAITextureGraphOutput& Output : Report.Outputs) Size += EstimateOutputBytes(Output);
	for (const FString& Name : Report.ExpressionClasses) Size += EstimateBytes(Name);
	return ClampBytes(Size);
}

FString FHyperAIStudioTextureGraphValidateResultPayload::GetTypeId() const
{
	return FHyperAIStudioTextureGraphContracts::ValidateResultTypeId;
}

FString FHyperAIStudioTextureGraphValidateResultPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioTextureGraphContracts::ValidateResultSchemaFingerprint();
}

int32 FHyperAIStudioTextureGraphValidateResultPayload::GetBoundedByteSize() const
{
	using namespace HyperAIStudio::TextureGraph::Private;
	int64 Size = 512 + EstimateBytes(Report.Status) + EstimateBytes(Report.Diagnostic) + EstimateBytes(Report.Revision);
	for (const FHyperAITextureGraphOutput& Output : Report.Outputs) Size += EstimateOutputBytes(Output);
	for (const FHyperAITextureGraphIssue& Issue : Report.Issues) Size += EstimateIssueBytes(Issue);
	return ClampBytes(Size);
}

FString FHyperAIStudioTextureGraphMutationResultPayload::GetTypeId() const
{
	return FHyperAIStudioTextureGraphContracts::MutationResultTypeId;
}

FString FHyperAIStudioTextureGraphMutationResultPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioTextureGraphContracts::MutationResultSchemaFingerprint();
}

int32 FHyperAIStudioTextureGraphMutationResultPayload::GetBoundedByteSize() const
{
	return HyperAIStudio::TextureGraph::Private::ClampBytes(96 + 2ll * (Phase.Len() + ContentKey.Len()));
}

TArray<FHyperAITextureGraphCapabilityStatus> FHyperAIStudioTextureGraphContracts::GetCapabilityMatrix()
{
	FHyperAITextureGraphCapabilityStatus Status;
	Status.SupportedCases = {
		TEXT("create_texture_graph_asset"),
		TEXT("nodes_pins_values_edges_outputs_capture"),
		TEXT("add_remove_connect_disconnect_nodes"),
		TEXT("scalar_int_bool_string_color_vector_enum_name_and_asset_pin_values"),
		TEXT("pin_and_output_aliases_node_comments_and_positions"),
		TEXT("output_texture_name_folder_resolution_format_srgb"),
		TEXT("auto_layout_of_added_nodes"),
		TEXT("structural_validation_and_export_state"),
		TEXT("journaled_apply_export_validate_save_fresh_verify")};
	Status.UnsupportedCases = {
		TEXT("target_open_in_texture_graph_editor"),
		TEXT("texture_graph_instances"),
		TEXT("texture_and_array_pin_values_use_connections"),
		TEXT("output_settings_through_set_pin_value"),
		TEXT("viewport_preview_material_and_mesh_settings"),
		TEXT("synchronous_render_to_render_targets"),
		TEXT("export_outside_game_folder")};
	Status.Remediation = TEXT("Inspect (bLoad if needed), dry-run apply_plan, resubmit with operation_id and plan_hash, poll hyper_operation_status, then validate. With bExport, validate with policy exported until it passes. Close the graph's editor tab first.");
	return {Status};
}

const FHyperAIStudioDomainAdapterDescriptor& FHyperAIStudioTextureGraphContracts::GetAdapterDescriptor()
{
	static const FHyperAIStudioDomainAdapterDescriptor Descriptor = []
	{
		FHyperAIStudioDomainAdapterDescriptor Value;
		Value.PackId = PackId;
		Value.AdapterId = TEXT("adapter.texture_graph.graph_edit.ue58");
		Value.SemanticVersion = TEXT("1.0.0");
		Value.AdapterVersion = 1;
		Value.Variants.Add({InspectToolName, InspectVariantId, InspectPayloadTypeId, InspectPayloadSchemaFingerprint(),
			InspectResultTypeId, InspectResultSchemaFingerprint(), EHyperAIStudioDomainSafety::Read});
		Value.Variants.Add({MutationToolName, MutationVariantId, EditOpsPayloadTypeId, EditOpsPayloadSchemaFingerprint(),
			MutationResultTypeId, MutationResultSchemaFingerprint(), EHyperAIStudioDomainSafety::Edit});
		Value.Variants.Add({ValidateToolName, ValidateVariantId, ValidatePayloadTypeId, ValidatePayloadSchemaFingerprint(),
			ValidateResultTypeId, ValidateResultSchemaFingerprint(), EHyperAIStudioDomainSafety::Read});
		Value.ContractFingerprint = FHyperAIStudioDomainAdapterRegistry::ComputeContractFingerprint(Value);
		Value.AdapterFingerprint = FHyperAIStudioDomainAdapterRegistry::ComputeAdapterFingerprint(Value);
		return Value;
	}();
	return Descriptor;
}

FHyperAITextureGraphInspectReport FHyperAIStudioTextureGraphContracts::Inspect(const FHyperAITextureGraphInspectRequest& Request)
{
	using namespace HyperAIStudio::TextureGraph::Private;
	FHyperAITextureGraphInspectReport Report;
	Report.Capabilities = GetCapabilityMatrix();
	auto Reject = [&](const TCHAR* Status, const FString& Diagnostic)
	{
		Report.bOk = false;
		Report.Status = Status;
		Report.Diagnostic = Diagnostic;
		return Report;
	};
	if (!IsInGameThread())
	{
		return Reject(TEXT("game_thread_required"), TEXT("Texture Graph inspection runs on the game thread."));
	}
	if (!IsCanonicalProjectObjectPath(Request.TargetPath))
	{
		return Reject(TEXT("invalid_target_path"), TEXT("Target must be one canonical /Game object path, e.g. /Game/T/TG_Rock.TG_Rock."));
	}
	if (Request.MaxOutputBytes < MinOutputBytes || Request.MaxOutputBytes > MaxOutputBytes)
	{
		return Reject(TEXT("invalid_bounds"), TEXT("max_output_bytes is outside the closed output bounds."));
	}
	Report.bEngineAvailable = Gate::IsEngineAvailable();
	Report.ExportsInFlight = Gate::CountExportsInFlight();
	Report.bExistsOnDisk = FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(Request.TargetPath));
	if (Request.bIncludeExpressionCatalog)
	{
		Report.ExpressionClasses = Gate::GetExpressionCatalog();
	}
	const UTextureGraph* Graph = Gate::ResolveGraph(Request.TargetPath, Request.bLoad);
	if (!Graph)
	{
		return Reject(Report.bExistsOnDisk ? TEXT("target_not_loaded") : TEXT("target_not_found"),
			Report.bExistsOnDisk
				? FString(TEXT("The asset exists but is not a loaded Texture Graph; inspect with bLoad."))
				: FString(TEXT("No Texture Graph exists there; create one with apply_plan bCreate.")));
	}
	Report.bLoaded = true;
	Report.bPackageDirty = Graph->GetOutermost()->IsDirty();
	Report.bOpenInEditor = Gate::IsOpenInEditor(*Graph);
	Report.Revision = Gate::ComputeRevision(*Graph);
	TArray<FHyperAITextureGraphNode> Nodes;
	Gate::ReadGraph(*Graph, Request.bIncludePins, Nodes, Report.Edges);
	Report.Outputs = Gate::ReadOutputs(*Graph);

	int64 Budget = 1024;
	for (const FString& Name : Report.ExpressionClasses) Budget += EstimateBytes(Name);
	for (const FHyperAITextureGraphEdge& Edge : Report.Edges) Budget += 32 + EstimateBytes(Edge.FromPin) + EstimateBytes(Edge.ToPin);
	for (const FHyperAITextureGraphOutput& Output : Report.Outputs) Budget += EstimateOutputBytes(Output);
	for (FHyperAITextureGraphNode& Node : Nodes)
	{
		const int64 NodeBytes = EstimateNodeBytes(Node);
		if (Budget + NodeBytes > Request.MaxOutputBytes || Report.Nodes.Num() >= MaxNodes)
		{
			Report.bTruncated = true;
			break;
		}
		Budget += NodeBytes;
		Report.Nodes.Add(MoveTemp(Node));
	}
	Report.bOk = true;
	Report.Status = Report.bTruncated ? TEXT("truncated") : TEXT("captured");
	Report.Diagnostic = Report.bTruncated
		? TEXT("Nodes were cut at max_output_bytes; raise it or inspect with bIncludePins false.")
		: FString::Printf(TEXT("%d nodes, %d edges, %d outputs."), Report.Nodes.Num(), Report.Edges.Num(), Report.Outputs.Num());
	return Report;
}

FHyperAITextureGraphValidateReport FHyperAIStudioTextureGraphContracts::Validate(const FHyperAITextureGraphValidateRequest& Request)
{
	using namespace HyperAIStudio::TextureGraph::Private;
	FHyperAITextureGraphValidateReport Report;
	Report.Policy = Request.Policy;
	auto Reject = [&](const TCHAR* Status, const FString& Diagnostic)
	{
		Report.bOk = false;
		Report.bValid = false;
		Report.Status = Status;
		Report.Diagnostic = Diagnostic;
		return Report;
	};
	if (!IsInGameThread())
	{
		return Reject(TEXT("game_thread_required"), TEXT("Texture Graph validation runs on the game thread."));
	}
	if (!IsCanonicalProjectObjectPath(Request.TargetPath)
		|| (!Request.ExpectedRevision.IsEmpty() && !IsCanonicalSha256(Request.ExpectedRevision)))
	{
		return Reject(TEXT("invalid_target_or_revision"), TEXT("A canonical /Game object path and an optional revision from inspect are required."));
	}
	if (Request.Policy != TEXT("authoring") && Request.Policy != TEXT("exported"))
	{
		return Reject(TEXT("invalid_policy"), TEXT("policy must be authoring or exported."));
	}
	if (Request.MaxOutputBytes < MinOutputBytes || Request.MaxOutputBytes > MaxOutputBytes)
	{
		return Reject(TEXT("invalid_bounds"), TEXT("max_output_bytes is outside the closed output bounds."));
	}
	const UTextureGraph* Graph = Gate::ResolveGraph(Request.TargetPath, /*bLoad=*/false);
	if (!Graph)
	{
		return Reject(TEXT("target_not_loaded"), TEXT("The Texture Graph is not loaded; inspect it with bLoad first."));
	}
	Report.Revision = Gate::ComputeRevision(*Graph);
	if (!Request.ExpectedRevision.IsEmpty() && Request.ExpectedRevision != Report.Revision)
	{
		return Reject(TEXT("stale_revision"), TEXT("The graph changed since that revision; inspect it again."));
	}
	Report.ExportsInFlight = Gate::CountExportsInFlight();
	Report.Outputs = Gate::ReadOutputs(*Graph);
	int64 Budget = 1024;
	for (const FHyperAITextureGraphOutput& Output : Report.Outputs) Budget += EstimateOutputBytes(Output);
	for (const FHyperAITextureGraphIssue& Issue : Gate::Validate(*Graph, Request.Policy == TEXT("exported")))
	{
		Report.ErrorCount += Issue.Severity == TEXT("error") ? 1 : 0;
		Report.WarningCount += Issue.Severity == TEXT("warning") ? 1 : 0;
		const int64 IssueBytes = EstimateIssueBytes(Issue);
		if (Report.Issues.Num() >= MaxIssues || Budget + IssueBytes > Request.MaxOutputBytes)
		{
			Report.bTruncated = true;
			continue;
		}
		Budget += IssueBytes;
		Report.Issues.Add(Issue);
	}
	Report.bOk = true;
	Report.bValid = Report.ErrorCount == 0;
	Report.Status = Report.bValid ? TEXT("valid") : TEXT("invalid");
	Report.Diagnostic = FString::Printf(TEXT("%d errors, %d warnings, %d exports running."),
		Report.ErrorCount, Report.WarningCount, Report.ExportsInFlight);
	return Report;
}

FString FHyperAIStudioTextureGraphContracts::ComputeEditOpsSemanticFingerprint(const FHyperAIStudioTextureGraphEditOpsPayload& Payload)
{
	using HyperAIStudio::TextureGraph::Private::AppendToken;
	FString Canonical;
	AppendToken(Canonical, TEXT("hyperai.texture_graph.edit-ops-intent.v1"));
	AppendToken(Canonical, PackId);
	AppendToken(Canonical, MutationToolName);
	AppendToken(Canonical, MutationVariantId);
	AppendToken(Canonical, EditOpsPayloadSchemaFingerprint());
	AppendToken(Canonical, GetAdapterDescriptor().AdapterFingerprint);
	AppendToken(Canonical, Payload.TargetPath);
	AppendToken(Canonical, Payload.BaseRevision);
	AppendToken(Canonical, Payload.bCreate ? TEXT("create") : TEXT("edit"));
	AppendToken(Canonical, Payload.bAutoLayout ? TEXT("auto_layout") : TEXT("manual_layout"));
	AppendToken(Canonical, Payload.bExport ? TEXT("export") : TEXT("no_export"));
	AppendToken(Canonical, Payload.bSave ? TEXT("save") : TEXT("no_save"));
	AppendToken(Canonical, FString::FromInt(Payload.Ops.Num()));
	for (const FHyperAITextureGraphEditOp& Op : Payload.Ops)
	{
		AppendToken(Canonical, Op.Kind);
		AppendToken(Canonical, Op.NodeKey);
		AppendToken(Canonical, Op.ExpressionClass);
		AppendToken(Canonical, Op.Node);
		AppendToken(Canonical, Op.Pin);
		AppendToken(Canonical, Op.ToNode);
		AppendToken(Canonical, Op.ToPin);
		AppendToken(Canonical, Op.Value);
		AppendToken(Canonical, Op.BaseName);
		AppendToken(Canonical, Op.FolderPath);
		AppendToken(Canonical, FString::Printf(TEXT("%d|%d|%s|%d|%d|%d|%d"), Op.Width, Op.Height, *Op.TextureFormat,
			Op.bSRGB ? 1 : 0, Op.bShouldExport ? 1 : 0, Op.PosX, Op.PosY));
	}
	return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
}

FHyperAITextureGraphApplyPlanReport FHyperAIStudioTextureGraphContracts::BuildPlan(const FHyperAITextureGraphApplyPlanRequest& Request)
{
	using namespace HyperAIStudio::TextureGraph::Private;
	FHyperAITextureGraphApplyPlanReport Report;
	Report.bDryRun = Request.bDryRun;
	Report.OperationId = Request.OperationId.Left(FHyperAIStudioDomainLimits::MaxOperationIdChars);
	auto Reject = [&](const FString& Status, const FString& Diagnostic)
	{
		Report.bOk = false;
		Report.bStaged = false;
		Report.bExecutionSubmitted = false;
		Report.Status = Status;
		Report.Diagnostic = Diagnostic;
		return Report;
	};
	if (!IsInGameThread())
	{
		return Reject(TEXT("game_thread_required"), TEXT("Texture Graph edit planning runs on the game thread."));
	}
	if (Request.bDryRun && (!Request.OperationId.IsEmpty() || !Request.ExpectedPlanHash.IsEmpty()))
	{
		return Reject(TEXT("unexpected_submission_fields"), TEXT("A dry run takes no operation_id or expected_plan_hash."));
	}
	if (!Request.bDryRun && (!FHyperAIStudioExtensionRuntime::IsValidOperationId(Request.OperationId)
		|| !IsCanonicalSha256(Request.ExpectedPlanHash)))
	{
		return Reject(TEXT("invalid_submission_fields"), TEXT("A non-dry submission needs a valid operation_id and the plan_hash its dry run returned."));
	}
	if (!IsCanonicalProjectObjectPath(Request.TargetPath)
		|| (Request.bCreate ? !Request.ExpectedRevision.IsEmpty() : !IsCanonicalSha256(Request.ExpectedRevision)))
	{
		return Reject(TEXT("invalid_target_or_revision"),
			TEXT("Pass a canonical /Game object path, plus the revision from inspect when editing, or no revision when bCreate."));
	}
	if ((Request.Ops.IsEmpty() && !Request.bCreate && !Request.bExport) || Request.Ops.Num() > MaxOpsPerPlan)
	{
		return Reject(TEXT("invalid_op_count"), FString::Printf(TEXT("A plan needs up to %d ops, and at least one unless it creates or exports."), MaxOpsPerPlan));
	}
	if (Request.MaxOutputBytes < MinOutputBytes || Request.MaxOutputBytes > MaxOutputBytes)
	{
		return Reject(TEXT("invalid_bounds"), TEXT("max_output_bytes is outside the closed output bounds."));
	}
	if (Request.bExport && !Request.bSave)
	{
		return Reject(TEXT("export_requires_save"), TEXT("Exporting saves the graph first; set bSave true."));
	}
	if (!Request.bSave && Request.bCreate)
	{
		return Reject(TEXT("create_requires_save"), TEXT("A created graph must be saved; set bSave true."));
	}
	if (Request.bExport && !Gate::IsEngineAvailable())
	{
		return Reject(TEXT("texture_graph_engine_unavailable"), TEXT("Texture Graph's engine is not running, so nothing can export."));
	}

	const UTextureGraph* Graph = Gate::ResolveGraph(Request.TargetPath, /*bLoad=*/false);
	if (Request.bCreate)
	{
		if (Graph || FindPackage(nullptr, *FPackageName::ObjectPathToPackageName(Request.TargetPath))
			|| FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(Request.TargetPath)))
		{
			return Reject(TEXT("target_exists"), TEXT("An asset already exists at target_path; inspect and edit it instead."));
		}
	}
	else
	{
		if (!Graph)
		{
			return Reject(TEXT("target_not_loaded"), TEXT("The Texture Graph is not loaded; inspect it with bLoad first."));
		}
		Report.BaseRevision = Gate::ComputeRevision(*Graph);
		if (Report.BaseRevision != Request.ExpectedRevision)
		{
			return Reject(TEXT("stale_revision"), TEXT("The graph changed after inspection; inspect it again."));
		}
		if (Gate::IsOpenInEditor(*Graph))
		{
			return Reject(TEXT("target_open_in_editor"),
				TEXT("Close the graph's Texture Graph editor tab first. That editor works on a copy and writes it back over the asset when saved."));
		}
	}

	// Run the ops on a transient copy: every node, pin, value and connection is checked by Texture Graph itself.
	TArray<FHyperAITextureGraphOutput> PlannedOutputs;
	TArray<FHyperAITextureGraphIssue> PlannedIssues;
	FString SimulationError;
	if (!Gate::SimulatePlan(Graph, Request.TargetPath, Request.Ops, Request.bAutoLayout,
		Report.Effects.NodeKeyIds, PlannedOutputs, PlannedIssues, SimulationError))
	{
		return Reject(TEXT("invalid_op"), SimulationError);
	}
	int32 Errors = 0;
	int64 Budget = 1024;
	for (const FHyperAITextureGraphIssue& Issue : PlannedIssues)
	{
		Errors += Issue.Severity == TEXT("error") ? 1 : 0;
		const int64 IssueBytes = EstimateIssueBytes(Issue);
		if (Report.Issues.Num() < MaxIssues && Budget + IssueBytes <= Request.MaxOutputBytes)
		{
			Budget += IssueBytes;
			Report.Issues.Add(Issue);
		}
	}
	if (Errors > 0)
	{
		return Reject(TEXT("planned_graph_invalid"), FString::Printf(TEXT("The graph would have %d structural errors; see issues."), Errors));
	}
	Report.Effects.OpCount = Request.Ops.Num();
	Report.Effects.bCreatesAsset = Request.bCreate;
	Report.Effects.bWouldSave = Request.bSave;
	Report.Effects.bWouldExport = Request.bExport;
	if (Request.bExport)
	{
		for (const FHyperAITextureGraphOutput& Output : PlannedOutputs)
		{
			if (!Output.bShouldExport)
			{
				continue;
			}
			Report.Effects.ExportTexturePaths.Add(Output.TexturePath);
			if (Output.TextureState != TEXT("missing"))
			{
				Report.Effects.OverwrittenTexturePaths.Add(Output.TexturePath);
			}
		}
	}

	const TSharedRef<FHyperAIStudioTextureGraphEditOpsPayload, ESPMode::ThreadSafe> Payload =
		MakeShared<FHyperAIStudioTextureGraphEditOpsPayload, ESPMode::ThreadSafe>();
	Payload->TargetPath = Request.TargetPath;
	Payload->BaseRevision = Report.BaseRevision;
	Payload->bCreate = Request.bCreate;
	Payload->Ops = Request.Ops;
	Payload->bAutoLayout = Request.bAutoLayout;
	Payload->bExport = Request.bExport;
	Payload->bSave = Request.bSave;
	Payload->SemanticFingerprint = ComputeEditOpsSemanticFingerprint(*Payload);
	if (Payload->GetBoundedByteSize() > FHyperAIStudioDomainLimits::MaxRequestBytes)
	{
		return Reject(TEXT("payload_bound_exceeded"), TEXT("The ops exceed the bounded request size; split them into smaller plans."));
	}

	FHyperAIStudioTrustedArtifactRequest Trusted;
	Trusted.PackId = PackId;
	Trusted.ToolName = MutationToolName;
	Trusted.VariantId = MutationVariantId;
	Trusted.Safety = EHyperAIStudioDomainSafety::Edit;
	Trusted.ArtifactSemanticFingerprint = Payload->SemanticFingerprint;
	Trusted.EffectTarget = Request.TargetPath;
	Trusted.DeadlineMs = FHyperAIStudioTypedArtifactLimits::MaxArtifactDeadlineMs;
	Trusted.MaxNativeOperations = MaxOpsPerPlan + 8;
	Trusted.MaxGameThreadMs = FHyperAIStudioTypedArtifactLimits::MaxArtifactGameThreadMs;
	Trusted.MaxOutputBytes = Request.MaxOutputBytes;
	Trusted.MaxResultBytes = 256;
	Trusted.StageLifetimeMs = StageLifetimeMs;
	// A plan that neither saves nor exports still validates and verifies; the executor requires both.
	Trusted.bCompileOnce = Request.bExport;
	Trusted.bSaveOnce = Request.bSave;
	Trusted.bValidateOnce = true;
	Trusted.bVerifyFreshOnce = true;

	FHyperAIStudioTrustedPreparedArtifact Prepared;
	FHyperAIStudioTrustedPrepareReport Prepare;
	FString PrepareError;
	const TSharedRef<FHyperAIStudioTextureGraphFreshVerifier, ESPMode::ThreadSafe> Verifier =
		MakeShared<FHyperAIStudioTextureGraphFreshVerifier, ESPMode::ThreadSafe>();
	if (!FHyperAIStudioTrustedExecutionFacade::PrepareDryRun(Trusted, Payload, Verifier, Prepared, Prepare, PrepareError)
		|| !Prepare.bPrepared)
	{
		FString Diagnostic = PrepareError.IsEmpty() ? Prepare.Status.Diagnostic : PrepareError;
		if (!Prepare.Status.BlockingPrerequisiteIds.IsEmpty())
		{
			Diagnostic += TEXT(" Blocking: ") + FString::Join(Prepare.Status.BlockingPrerequisiteIds, TEXT(", "));
		}
		return Reject(Prepare.Status.StatusCode.IsEmpty() ? FString(TEXT("prepare_failed")) : Prepare.Status.StatusCode, Diagnostic);
	}
	Report.bTrustedPrepared = true;
	// Bound to the semantic intent, not the host plan hash, which differs between preparations of the same plan.
	Report.PlanHash = Payload->SemanticFingerprint;
	Report.AuthorizationPlanHash = Prepare.PlanHash;

	if (Request.bDryRun)
	{
		Report.bOk = true;
		Report.Status = TEXT("planned");
		Report.Diagnostic = TEXT("Nothing changed. To apply, resubmit with bDryRun false, a new operation_id, and expected_plan_hash set to plan_hash.");
		return Report;
	}
	if (Request.ExpectedPlanHash != Payload->SemanticFingerprint)
	{
		return Reject(TEXT("plan_hash_mismatch"), TEXT("The plan changed since its dry run (ops, graph state, or catalog). Dry-run again and review it."));
	}
	FHyperAIStudioTypedArtifactStageReceipt Receipt;
	FHyperAIStudioTrustedExecutionDiagnostic StageStatus;
	FString StageError;
	if (!FHyperAIStudioTrustedExecutionFacade::StageExact(Prepared, Request.OperationId, Receipt, StageStatus, StageError))
	{
		return Reject(StageStatus.StatusCode.IsEmpty() ? FString(TEXT("stage_failed")) : StageStatus.StatusCode,
			StageError.IsEmpty() ? StageStatus.Diagnostic : StageError);
	}
	Report.bStaged = true;
	FHyperAIStudioTypedArtifactSubmissionReceipt Submission;
	FHyperAIStudioTrustedExecutionDiagnostic SubmitStatus;
	FString SubmitError;
	// Edit is tokenless; only risky safety classes need a server-issued grant.
	if (!FHyperAIStudioTrustedExecutionFacade::SubmitExact(Receipt, FString(), Submission, SubmitStatus, SubmitError))
	{
		Report.Status = SubmitStatus.StatusCode.IsEmpty() ? FString(TEXT("submit_failed")) : SubmitStatus.StatusCode;
		Report.Diagnostic = SubmitError.IsEmpty() ? SubmitStatus.Diagnostic : SubmitError;
		return Report;
	}
	Report.bExecutionSubmitted = true;
	Report.bOk = true;
	Report.Status = TEXT("submitted");
	Report.Diagnostic = TEXT("The edit runs over the next editor ticks. Poll hyper_operation_status with operation_id until it is terminal.");
	return Report;
}

FHyperAITextureGraphInspectReport UHyperAIStudioTextureGraphToolset::hyper_texture_graph_inspect(
	const FHyperAITextureGraphInspectRequest& Request)
{
	return FHyperAIStudioTextureGraphContracts::Inspect(Request);
}

FHyperAITextureGraphApplyPlanReport UHyperAIStudioTextureGraphToolset::hyper_texture_graph_apply_plan(
	const FHyperAITextureGraphApplyPlanRequest& Request)
{
	return FHyperAIStudioTextureGraphContracts::BuildPlan(Request);
}

FHyperAITextureGraphValidateReport UHyperAIStudioTextureGraphToolset::hyper_texture_graph_validate(
	const FHyperAITextureGraphValidateRequest& Request)
{
	return FHyperAIStudioTextureGraphContracts::Validate(Request);
}

const FHyperAIStudioDomainAdapterDescriptor& FHyperAIStudioTextureGraphDomainAdapter::GetDescriptor() const
{
	return FHyperAIStudioTextureGraphContracts::GetAdapterDescriptor();
}

FHyperAIStudioDomainAdapterResult FHyperAIStudioTextureGraphDomainAdapter::Execute(
	const FHyperAIStudioDomainDispatchContext& Context,
	const IHyperAIStudioDomainRequestPayload& Payload)
{
	using Contracts = FHyperAIStudioTextureGraphContracts;
	const FHyperAIStudioDomainAdapterDescriptor& Descriptor = GetDescriptor();
	FHyperAIStudioDomainAdapterResult Result;
	Result.Outcome = EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect;
	auto Reject = [&](const TCHAR* Status, const TCHAR* Diagnostic)
	{
		Result.StatusCode = Status;
		Result.Diagnostic = Diagnostic;
		return Result;
	};
	auto Matches = [&](const TCHAR* Tool, const TCHAR* Variant, const EHyperAIStudioDomainSafety Safety,
		const TCHAR* TypeId, const FString& Schema)
	{
		return Context.Binding.ToolName == Tool && Context.Binding.VariantId == Variant && Context.Safety == Safety
			&& Payload.GetTypeId() == TypeId && Payload.GetSchemaFingerprint() == Schema;
	};
	auto Succeed = [&](const TSharedRef<IHyperAIStudioDomainResultPayload, ESPMode::ThreadSafe>& Output,
		const FString& Status, const FString& Diagnostic)
	{
		Result.Outcome = EHyperAIStudioDomainDispatchOutcome::Succeeded;
		Result.StatusCode = Status.Left(FHyperAIStudioDomainLimits::MaxStatusCodeChars);
		Result.Diagnostic = Diagnostic.Left(FHyperAIStudioDomainLimits::MaxDiagnosticChars);
		Result.Payload = Output;
		return Result;
	};
	if (Context.Binding.PackId != Descriptor.PackId
		|| (!Context.Binding.ExpectedAdapterFingerprint.IsEmpty()
			&& Context.Binding.ExpectedAdapterFingerprint != Descriptor.AdapterFingerprint)
		|| Payload.GetBoundedByteSize() <= 0
		|| Payload.GetBoundedByteSize() > FHyperAIStudioDomainLimits::MaxRequestBytes)
	{
		return Reject(TEXT("typed_binding_mismatch"), TEXT("Texture Graph adapter rejected pack, adapter, or bounded DTO identity."));
	}
	if (Matches(Contracts::InspectToolName, Contracts::InspectVariantId, EHyperAIStudioDomainSafety::Read,
		Contracts::InspectPayloadTypeId, Contracts::InspectPayloadSchemaFingerprint()))
	{
		const TSharedRef<FHyperAIStudioTextureGraphInspectResultPayload, ESPMode::ThreadSafe> Output =
			MakeShared<FHyperAIStudioTextureGraphInspectResultPayload, ESPMode::ThreadSafe>();
		Output->Report = Contracts::Inspect(static_cast<const FHyperAIStudioTextureGraphInspectPayload&>(Payload).Request);
		return Succeed(Output, Output->Report.Status, Output->Report.Diagnostic);
	}
	if (Matches(Contracts::ValidateToolName, Contracts::ValidateVariantId, EHyperAIStudioDomainSafety::Read,
		Contracts::ValidatePayloadTypeId, Contracts::ValidatePayloadSchemaFingerprint()))
	{
		const TSharedRef<FHyperAIStudioTextureGraphValidateResultPayload, ESPMode::ThreadSafe> Output =
			MakeShared<FHyperAIStudioTextureGraphValidateResultPayload, ESPMode::ThreadSafe>();
		Output->Report = Contracts::Validate(static_cast<const FHyperAIStudioTextureGraphValidatePayload&>(Payload).Request);
		return Succeed(Output, Output->Report.Status, Output->Report.Diagnostic);
	}
	if (Matches(Contracts::MutationToolName, Contracts::MutationVariantId, EHyperAIStudioDomainSafety::Edit,
		Contracts::EditOpsPayloadTypeId, Contracts::EditOpsPayloadSchemaFingerprint()))
	{
		const FHyperAIStudioTextureGraphEditOpsPayload& Typed = static_cast<const FHyperAIStudioTextureGraphEditOpsPayload&>(Payload);
		if (Contracts::ComputeEditOpsSemanticFingerprint(Typed) != Typed.SemanticFingerprint)
		{
			return Reject(TEXT("typed_payload_drift"), TEXT("The sealed edit-ops semantic fingerprint drifted."));
		}
		return HyperAIStudio::TextureGraph::Private::ExecuteEditPhase(Context.ActionKind, Typed);
	}
	return Reject(TEXT("typed_binding_mismatch"), TEXT("Texture Graph adapter rejected a non-exact tool, variant, safety, schema, or DTO binding."));
}

FString FHyperAIStudioTextureGraphFreshVerifier::GetOwnerAdapterFingerprint() const
{
	return FHyperAIStudioTextureGraphContracts::GetAdapterDescriptor().AdapterFingerprint;
}

bool FHyperAIStudioTextureGraphFreshVerifier::ResolveCanonicalEffectTarget(
	const IHyperAIStudioTypedArtifactPayload& Request,
	FString& OutCanonicalEffectTarget,
	FString& OutError)
{
	using Contracts = FHyperAIStudioTextureGraphContracts;
	OutCanonicalEffectTarget.Reset();
	OutError.Reset();
	if (Request.GetTypeId() != Contracts::EditOpsPayloadTypeId
		|| Request.GetSchemaFingerprint() != Contracts::EditOpsPayloadSchemaFingerprint())
	{
		OutError = TEXT("Fresh verifier received the wrong typed Texture Graph request schema.");
		return false;
	}
	const FHyperAIStudioTextureGraphEditOpsPayload& Typed = static_cast<const FHyperAIStudioTextureGraphEditOpsPayload&>(Request);
	if (!Contracts::IsCanonicalProjectObjectPath(Typed.TargetPath)
		|| Contracts::ComputeEditOpsSemanticFingerprint(Typed) != Typed.SemanticFingerprint)
	{
		OutError = TEXT("Fresh verifier rejected the canonical target or semantic seal.");
		return false;
	}
	OutCanonicalEffectTarget = Typed.TargetPath;
	return true;
}

bool FHyperAIStudioTextureGraphFreshVerifier::VerifyFreshExact(
	const IHyperAIStudioTypedArtifactPayload& Request,
	const IHyperAIStudioDomainResultPayload& Result,
	FString& OutPostconditionHash,
	FString& OutError)
{
	using Contracts = FHyperAIStudioTextureGraphContracts;
	OutPostconditionHash.Reset();
	OutError.Reset();
	if (!IsInGameThread())
	{
		OutError = TEXT("Fresh Texture Graph verification requires the game thread.");
		return false;
	}
	FString CanonicalTarget;
	if (!ResolveCanonicalEffectTarget(Request, CanonicalTarget, OutError)
		|| Result.GetTypeId() != Contracts::MutationResultTypeId
		|| Result.GetSchemaFingerprint() != Contracts::MutationResultSchemaFingerprint())
	{
		if (OutError.IsEmpty()) OutError = TEXT("Fresh verifier received the wrong result schema.");
		return false;
	}
	const FHyperAIStudioTextureGraphEditOpsPayload& Typed = static_cast<const FHyperAIStudioTextureGraphEditOpsPayload&>(Request);
	const FHyperAIStudioTextureGraphMutationResultPayload& TypedResult =
		static_cast<const FHyperAIStudioTextureGraphMutationResultPayload&>(Result);
	if (TypedResult.Phase != TEXT("completed") || !Contracts::IsCanonicalSha256(TypedResult.ContentKey))
	{
		OutError = TEXT("Only a completed fresh-capture result may be verified.");
		return false;
	}
	const UTextureGraph* Graph = HyperAIStudio::TextureGraph::Gate::ResolveGraph(CanonicalTarget, /*bLoad=*/false);
	if (!Graph)
	{
		OutError = TEXT("The edited graph is no longer loaded.");
		return false;
	}
	const FString ContentKey = HyperAIStudio::TextureGraph::Gate::ComputeContentKey(*Graph);
	if (ContentKey != TypedResult.ContentKey)
	{
		OutError = TEXT("The graph changed between fresh capture and verification.");
		return false;
	}
	if (Typed.bSave == Graph->GetOutermost()->IsDirty())
	{
		OutError = Typed.bSave
			? TEXT("The graph still has unsaved changes after the save phase.")
			: TEXT("The unsaved edit is no longer present in memory.");
		return false;
	}
	FString Canonical;
	HyperAIStudio::TextureGraph::Private::AppendToken(Canonical, TEXT("hyperai.texture_graph.edit-ops-postcondition.v1"));
	HyperAIStudio::TextureGraph::Private::AppendToken(Canonical, Typed.SemanticFingerprint);
	HyperAIStudio::TextureGraph::Private::AppendToken(Canonical, ContentKey);
	OutPostconditionHash = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
	if (!Contracts::IsCanonicalSha256(OutPostconditionHash))
	{
		OutError = TEXT("Bounded postcondition hashing failed.");
		OutPostconditionHash.Reset();
		return false;
	}
	return true;
}

void FHyperAIStudioTextureGraphRegistration::Startup()
{
	if (bStarted) return;
	bStarted = true;
	PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(
		this, &FHyperAIStudioTextureGraphRegistration::RegisterAfterEngineInit);
	if (GIsRunning && GEngine && UObjectInitialized()) RegisterAfterEngineInit();
}

void FHyperAIStudioTextureGraphRegistration::Shutdown()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}
	RollBackRegistration();
	bStarted = false;
}

bool FHyperAIStudioTextureGraphRegistration::IsRegistered() const
{
	return bOwnsToolset && AdapterHandle.IsValid() && ProbeHandle.IsValid()
		&& FHyperAIStudioCapabilityRuntimeIndex::IsOwned(
			UHyperAIStudioTextureGraphToolset::StaticClass(),
			FHyperAIStudioTextureGraphContracts::GetQualifiedToolsetName());
}

bool FHyperAIStudioTextureGraphRegistration::HasLiveOwnership() const
{
	return bOwnsToolset || AdapterHandle.IsValid() || ProbeHandle.IsValid();
}

void FHyperAIStudioTextureGraphRegistration::RegisterAfterEngineInit()
{
	if (!bStarted || IsRegistered() || !IsInGameThread() || IsEngineExitRequested()
		|| !UObjectInitialized() || !UToolsetRegistry::IsAvailable()
		|| !FHyperAIStudioTrustedExecutionFacade::IsAvailable())
	{
		return;
	}
	if (!FHyperAIStudioTextureGraphContracts::IsRegistrationAllowed(
		FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled()))
	{
		UE_LOG(LogHyperAIStudioTextureGraph, Verbose,
			TEXT("Texture Graph source cohort is not catalog-admitted; registration remains fail-closed."));
		return;
	}
	if (!FModuleManager::Get().IsModuleLoaded(TEXT("TextureGraph")))
	{
		return;
	}
	Adapter = MakeShared<FHyperAIStudioTextureGraphDomainAdapter, ESPMode::ThreadSafe>();
	FString Error;
	if (!FHyperAIStudioTrustedExecutionFacade::RegisterAdapter(Adapter.ToSharedRef(), AdapterHandle, Error))
	{
		UE_LOG(LogHyperAIStudioTextureGraph, Error, TEXT("Texture Graph adapter registration failed closed: %s"), *Error);
		Adapter.Reset();
		return;
	}
	if (!FHyperAIStudioTrustedExecutionFacade::RegisterLiveProbe(
		AdapterHandle, FHyperAIStudioTextureGraphContracts::LiveProbeId, ProbeHandle, Error))
	{
		UE_LOG(LogHyperAIStudioTextureGraph, Error, TEXT("Texture Graph live-probe registration failed closed: %s"), *Error);
		RollBackRegistration();
		return;
	}
	// Graph edits need only the TextureGraph module; the engine singleton is reported for exports, which check it again.
	const auto Observe = []()
	{
		FHyperAIStudioTrustedProbeResult Observation;
		Observation.bReady = FModuleManager::Get().IsModuleLoaded(TEXT("TextureGraph"))
			&& UTextureGraph::StaticClass() != nullptr;
		const bool bEngine = HyperAIStudio::TextureGraph::Gate::IsEngineAvailable();
		Observation.StatusCode = !Observation.bReady ? TEXT("required_module_not_loaded")
			: bEngine ? TEXT("ready_with_engine") : TEXT("ready_edit_only");
		Observation.Diagnostic = !Observation.bReady
			? TEXT("The TextureGraph module is not loaded.")
			: bEngine ? TEXT("Texture Graph and its engine are live; edits and exports are available.")
				: TEXT("Texture Graph is loaded but its engine is not running; edits work, exports are refused.");
		return Observation;
	};
	const FHyperAIStudioTrustedProbeResult First = Observe();
	if (!First.bReady || !ProbePublisher.Start(ProbeHandle, Observe, Error))
	{
		if (Error.IsEmpty()) Error = First.Diagnostic;
		UE_LOG(LogHyperAIStudioTextureGraph, Error, TEXT("Texture Graph live-probe publication failed closed: %s"), *Error);
		RollBackRegistration();
		return;
	}
	if (!FHyperAIStudioCapabilityRuntimeIndex::RegisterOwnedToolsetClass(
		UHyperAIStudioTextureGraphToolset::StaticClass(),
		FHyperAIStudioTextureGraphContracts::GetQualifiedToolsetName(), Error))
	{
		UE_LOG(LogHyperAIStudioTextureGraph, Error, TEXT("Texture Graph cohort registration failed closed: %s"), *Error);
		RollBackRegistration();
		return;
	}
	bOwnsToolset = true;
}

void FHyperAIStudioTextureGraphRegistration::RollBackRegistration()
{
	if (!IsInGameThread())
	{
		return;
	}
	if (bOwnsToolset && UObjectInitialized())
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::UnregisterOwned(
			UHyperAIStudioTextureGraphToolset::StaticClass(),
			FHyperAIStudioTextureGraphContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioTextureGraph, Error, TEXT("Texture Graph owned-toolset rollback failed closed: %s"), *Error);
			return;
		}
		bOwnsToolset = false;
	}
	ProbePublisher.Stop();
	if (ProbeHandle.IsValid())
	{
		FString Error;
		if (!FHyperAIStudioTrustedExecutionFacade::UnregisterLiveProbe(ProbeHandle, Error))
		{
			UE_LOG(LogHyperAIStudioTextureGraph, Error, TEXT("Texture Graph probe rollback failed closed: %s"), *Error);
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
			UE_LOG(LogHyperAIStudioTextureGraph, Error, TEXT("Texture Graph adapter rollback failed closed: %s"), *Error);
			return;
		}
		AdapterHandle = {};
		Adapter.Reset();
	}
}
