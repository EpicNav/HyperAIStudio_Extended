// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HyperAIStudioTextureGraphToolset.h"

#include "Async/TaskGraphInterfaces.h"
#include "Containers/Ticker.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioTextureGraphGate.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "RenderingThread.h"
#include "UObject/UnrealType.h"

namespace HyperAIStudio::TextureGraph::Tests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;
	const FString Folder = TEXT("/Game/__HyperAIStudioTests/TextureGraph");

	FHyperAIStudioDomainDispatchContext MakeMutationContext(const EHyperAIStudioDomainExecutionActionKind Phase)
	{
		FHyperAIStudioDomainDispatchContext Context;
		Context.Binding.PackId = FHyperAIStudioTextureGraphContracts::PackId;
		Context.Binding.ToolName = FHyperAIStudioTextureGraphContracts::MutationToolName;
		Context.Binding.VariantId = FHyperAIStudioTextureGraphContracts::MutationVariantId;
		Context.Binding.ExpectedAdapterFingerprint = FHyperAIStudioTextureGraphContracts::GetAdapterDescriptor().AdapterFingerprint;
		Context.Binding.ExpectedSafety = EHyperAIStudioDomainSafety::Edit;
		Context.Safety = EHyperAIStudioDomainSafety::Edit;
		Context.ActionKind = Phase;
		return Context;
	}

	FHyperAITextureGraphEditOp Op(const TCHAR* Kind, const TCHAR* Node = TEXT(""), const TCHAR* Pin = TEXT(""), const TCHAR* Value = TEXT(""))
	{
		FHyperAITextureGraphEditOp Result;
		Result.Kind = Kind;
		Result.Node = Node;
		Result.Pin = Pin;
		Result.Value = Value;
		return Result;
	}

	FHyperAITextureGraphEditOp AddNode(const TCHAR* Key, const TCHAR* Class)
	{
		FHyperAITextureGraphEditOp Result = Op(TEXT("add_node"));
		Result.NodeKey = Key;
		Result.ExpressionClass = Class;
		return Result;
	}

	FHyperAITextureGraphEditOp Connect(const TCHAR* From, const TCHAR* FromPin, const TCHAR* To, const TCHAR* ToPin)
	{
		FHyperAITextureGraphEditOp Result = Op(TEXT("connect"), From, FromPin);
		Result.ToNode = To;
		Result.ToPin = ToPin;
		return Result;
	}

	FHyperAITextureGraphEditOp SetOutput(const FString& BaseName)
	{
		FHyperAITextureGraphEditOp Result = Op(TEXT("set_output"), TEXT("0"));
		Result.BaseName = BaseName;
		Result.FolderPath = Folder;
		Result.Width = 256;
		Result.Height = 256;
		return Result;
	}

	/** Pumps the ticker, game-thread tasks and render commands the way Texture Graph's own blocking export does. */
	bool PumpUntil(TFunctionRef<bool()> Done, const double TimeoutSeconds)
	{
		const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
		while (!Done())
		{
			if (FPlatformTime::Seconds() > Deadline)
			{
				return false;
			}
			FTSTicker::GetCoreTicker().Tick(0.016f);
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
			FlushRenderingCommands();
		}
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioTextureGraphCohortTest,
	"HyperAIStudio.TextureGraph.Contracts.ExactThreeToolCohort",
	HyperAIStudio::TextureGraph::Tests::Flags)

bool FHyperAIStudioTextureGraphCohortTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TArray<FString> Names = FHyperAIStudioTextureGraphContracts::GetToolNames();
	TestEqual(TEXT("three tools"), Names.Num(), 3);
	const FHyperAIStudioDomainAdapterDescriptor& Descriptor = FHyperAIStudioTextureGraphContracts::GetAdapterDescriptor();
	TestEqual(TEXT("one variant per tool"), Descriptor.Variants.Num(), Names.Num());
	TestEqual(TEXT("pack id"), Descriptor.PackId, FString(FHyperAIStudioTextureGraphContracts::PackId));
	TestFalse(TEXT("adapter fingerprint sealed"), Descriptor.AdapterFingerprint.IsEmpty());

	int32 Callables = 0;
	for (TFieldIterator<UFunction> It(UHyperAIStudioTextureGraphToolset::StaticClass(), EFieldIteratorFlags::ExcludeSuper); It; ++It)
	{
		if (!It->HasMetaData(TEXT("AICallable")))
		{
			continue;
		}
		++Callables;
		TestTrue(*FString::Printf(TEXT("%s is a manifest tool"), *It->GetName()), Names.Contains(It->GetName()));
		const FString ToolTip = It->GetMetaData(TEXT("ToolTip"));
		TestTrue(*FString::Printf(TEXT("%s has a description within 256 characters"), *It->GetName()),
			!ToolTip.IsEmpty() && ToolTip.Len() <= 256);
	}
	TestEqual(TEXT("AICallable functions match the manifest"), Callables, Names.Num());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioTextureGraphFrontDoorTest,
	"HyperAIStudio.TextureGraph.Contracts.PlanFrontDoor",
	HyperAIStudio::TextureGraph::Tests::Flags)

bool FHyperAIStudioTextureGraphFrontDoorTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::TextureGraph::Tests;
	auto StatusOf = [](const FHyperAITextureGraphApplyPlanRequest& Request)
	{
		return FHyperAIStudioTextureGraphContracts::BuildPlan(Request).Status;
	};
	FHyperAITextureGraphApplyPlanRequest Request;
	Request.TargetPath = Folder + TEXT("/TG_FrontDoor.TG_FrontDoor");
	Request.bCreate = true;
	Request.Ops = {AddNode(TEXT("noise"), TEXT("Noise"))};

	FHyperAITextureGraphApplyPlanRequest Case = Request;
	Case.OperationId = TEXT("texture-graph-front-door");
	TestEqual(TEXT("dry run refuses submission fields"), StatusOf(Case), FString(TEXT("unexpected_submission_fields")));
	Case = Request;
	Case.bDryRun = false;
	TestEqual(TEXT("submission needs op id and plan hash"), StatusOf(Case), FString(TEXT("invalid_submission_fields")));
	Case = Request;
	Case.TargetPath = TEXT("/Game/NoObjectName");
	TestEqual(TEXT("non-canonical path"), StatusOf(Case), FString(TEXT("invalid_target_or_revision")));
	Case = Request;
	Case.ExpectedRevision = TEXT("sha256:") + FString::ChrN(64, TEXT('a'));
	TestEqual(TEXT("create takes no revision"), StatusOf(Case), FString(TEXT("invalid_target_or_revision")));
	Case = Request;
	Case.bCreate = false;
	TestEqual(TEXT("edit needs a revision"), StatusOf(Case), FString(TEXT("invalid_target_or_revision")));
	Case = Request;
	Case.bExport = true;
	Case.bSave = false;
	TestEqual(TEXT("export needs save"), StatusOf(Case), FString(TEXT("export_requires_save")));
	Case = Request;
	Case.Ops.Init(AddNode(TEXT("n"), TEXT("Noise")), FHyperAIStudioTextureGraphContracts::MaxOpsPerPlan + 1);
	TestEqual(TEXT("op bound"), StatusOf(Case), FString(TEXT("invalid_op_count")));
	Case = Request;
	Case.MaxOutputBytes = 1;
	TestEqual(TEXT("output bound"), StatusOf(Case), FString(TEXT("invalid_bounds")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioTextureGraphShadowTest,
	"HyperAIStudio.TextureGraph.Gate.ShadowPlanChecksEveryOp",
	HyperAIStudio::TextureGraph::Tests::Flags)

bool FHyperAIStudioTextureGraphShadowTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::TextureGraph::Tests;
	namespace Gate = HyperAIStudio::TextureGraph::Gate;
	const FString Target = Folder + TEXT("/TG_Shadow.TG_Shadow");
	auto Simulate = [&](const TArray<FHyperAITextureGraphEditOp>& Ops, FString& OutError, TMap<FString, int32>* OutKeys = nullptr,
		TArray<FHyperAITextureGraphOutput>* OutOutputs = nullptr)
	{
		TMap<FString, int32> Keys;
		TArray<FHyperAITextureGraphOutput> Outputs;
		TArray<FHyperAITextureGraphIssue> Issues;
		const bool bOk = Gate::SimulatePlan(nullptr, Target, Ops, true, Keys, Outputs, Issues, OutError);
		if (OutKeys) *OutKeys = Keys;
		if (OutOutputs) *OutOutputs = Outputs;
		return bOk;
	};

	FHyperAITextureGraphEditOp Frequency = Op(TEXT("set_pin_value"), TEXT("noise"), TEXT("Frequency"), TEXT("8"));
	FHyperAITextureGraphEditOp NoiseType = Op(TEXT("set_pin_value"), TEXT("noise"), TEXT("NoiseType"), TEXT("NOISETYPE_Perlin"));
	const TArray<FHyperAITextureGraphEditOp> Valid = {
		AddNode(TEXT("noise"), TEXT("TG_Expression_Noise")),
		Frequency,
		NoiseType,
		Connect(TEXT("noise"), TEXT("Output"), TEXT("0"), TEXT("Source")),
		SetOutput(TEXT("T_Shadow")),
		Op(TEXT("set_node_comment"), TEXT("noise"), TEXT(""), TEXT("Base noise"))};
	FString Error;
	TMap<FString, int32> Keys;
	TArray<FHyperAITextureGraphOutput> Outputs;
	TestTrue(*FString::Printf(TEXT("valid plan simulates (%s)"), *Error), Simulate(Valid, Error, &Keys, &Outputs));
	TestTrue(TEXT("add_node key receives a node id"), Keys.Contains(TEXT("noise")) && Keys[TEXT("noise")] > 0);
	TestEqual(TEXT("one output"), Outputs.Num(), 1);
	if (Outputs.Num() == 1)
	{
		TestEqual(TEXT("output texture path"), Outputs[0].TexturePath, Folder + TEXT("/T_Shadow.T_Shadow"));
		TestTrue(TEXT("output source connected"), Outputs[0].bSourceConnected);
		TestEqual(TEXT("output resolution"), Outputs[0].Width, 256);
	}

	auto ExpectRejected = [&](const TCHAR* What, TArray<FHyperAITextureGraphEditOp> Ops, const TCHAR* Needle)
	{
		FString OpError;
		const bool bOk = Simulate(Ops, OpError);
		TestFalse(*FString::Printf(TEXT("%s is rejected"), What), bOk);
		TestTrue(*FString::Printf(TEXT("%s names the problem (%s)"), What, *OpError), OpError.Contains(Needle));
	};
	ExpectRejected(TEXT("unknown class"), {AddNode(TEXT("x"), TEXT("TG_Expression_DoesNotExist"))}, TEXT("not a Texture Graph node class"));
	ExpectRejected(TEXT("numeric key"), {AddNode(TEXT("1x"), TEXT("Noise"))}, TEXT("node_key"));
	ExpectRejected(TEXT("unknown node"), {Op(TEXT("remove_node"), TEXT("42"))}, TEXT("No node"));
	ExpectRejected(TEXT("unknown pin"), {AddNode(TEXT("noise"), TEXT("Noise")), Op(TEXT("set_pin_value"), TEXT("noise"), TEXT("Nope"), TEXT("1"))}, TEXT("no pin"));
	ExpectRejected(TEXT("bad enum"), {AddNode(TEXT("noise"), TEXT("Noise")), Op(TEXT("set_pin_value"), TEXT("noise"), TEXT("NoiseType"), TEXT("Plaid"))}, TEXT("not a value"));
	ExpectRejected(TEXT("clamped value"), {AddNode(TEXT("noise"), TEXT("Noise")), Op(TEXT("set_pin_value"), TEXT("noise"), TEXT("Frequency"), TEXT("500"))}, TEXT("maximum"));
	ExpectRejected(TEXT("non-number"), {AddNode(TEXT("noise"), TEXT("Noise")), Op(TEXT("set_pin_value"), TEXT("noise"), TEXT("Octaves"), TEXT("lots"))}, TEXT("does not fit"));
	ExpectRejected(TEXT("output to output"), {AddNode(TEXT("a"), TEXT("Noise")), AddNode(TEXT("b"), TEXT("Noise")),
		Connect(TEXT("a"), TEXT("Output"), TEXT("b"), TEXT("Output"))}, TEXT("output pin to an input"));
	FHyperAITextureGraphEditOp BadOutput = SetOutput(TEXT("T_Bad"));
	BadOutput.Width = 300;
	ExpectRejected(TEXT("non power of two"), {BadOutput}, TEXT("power of two"));
	BadOutput = SetOutput(TEXT("T_Bad"));
	BadOutput.FolderPath = TEXT("/Engine/Textures");
	ExpectRejected(TEXT("export outside /Game"), {BadOutput}, TEXT("/Game folder"));
	ExpectRejected(TEXT("unknown kind"), {Op(TEXT("explode"), TEXT("0"))}, TEXT("Unknown kind"));
	ExpectRejected(TEXT("removed id reused"), {AddNode(TEXT("a"), TEXT("Noise")), Op(TEXT("remove_node"), TEXT("a")),
		AddNode(TEXT("b"), TEXT("Noise")), Op(TEXT("move_node"), TEXT("1"))}, TEXT("No node"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioTextureGraphAdapterTest,
	"HyperAIStudio.TextureGraph.Contracts.PayloadSealAndAdapterPhases",
	HyperAIStudio::TextureGraph::Tests::Flags)

bool FHyperAIStudioTextureGraphAdapterTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::TextureGraph::Tests;
	const TSharedRef<FHyperAIStudioTextureGraphEditOpsPayload, ESPMode::ThreadSafe> Payload =
		MakeShared<FHyperAIStudioTextureGraphEditOpsPayload, ESPMode::ThreadSafe>();
	Payload->TargetPath = Folder + TEXT("/TG_Missing.TG_Missing");
	Payload->BaseRevision = TEXT("sha256:") + FString::ChrN(64, TEXT('1'));
	Payload->Ops = {AddNode(TEXT("a"), TEXT("Noise")), AddNode(TEXT("b"), TEXT("Blend"))};
	Payload->SemanticFingerprint = FHyperAIStudioTextureGraphContracts::ComputeEditOpsSemanticFingerprint(*Payload);
	TestTrue(TEXT("fingerprint canonical"), FHyperAIStudioTextureGraphContracts::IsCanonicalSha256(Payload->SemanticFingerprint));

	FHyperAIStudioTextureGraphEditOpsPayload Reordered = *Payload;
	Swap(Reordered.Ops[0], Reordered.Ops[1]);
	TestNotEqual(TEXT("op order is sealed"), Payload->SemanticFingerprint,
		FHyperAIStudioTextureGraphContracts::ComputeEditOpsSemanticFingerprint(Reordered));
	FHyperAIStudioTextureGraphEditOpsPayload Exporting = *Payload;
	Exporting.bExport = true;
	TestNotEqual(TEXT("export choice is sealed"), Payload->SemanticFingerprint,
		FHyperAIStudioTextureGraphContracts::ComputeEditOpsSemanticFingerprint(Exporting));

	const TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> Clone = Payload->CloneImmutable();
	TestTrue(TEXT("clone detached"), &Clone.Get() != &Payload.Get());
	TestEqual(TEXT("clone seal"), Clone->GetSemanticFingerprint(), Payload->GetSemanticFingerprint());
	TestEqual(TEXT("clone size"), Clone->GetBoundedByteSize(), Payload->GetBoundedByteSize());

	FHyperAIStudioTextureGraphDomainAdapter Adapter;
	const FHyperAIStudioDomainAdapterResult Apply = Adapter.Execute(MakeMutationContext(EHyperAIStudioDomainExecutionActionKind::Apply), *Clone);
	TestEqual(TEXT("apply on a missing graph changes nothing"), Apply.Outcome, EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect);
	TestEqual(TEXT("missing graph status"), Apply.StatusCode, FString(TEXT("stale_revision")));
	const FHyperAIStudioDomainAdapterResult Save = Adapter.Execute(MakeMutationContext(EHyperAIStudioDomainExecutionActionKind::Save), *Clone);
	TestEqual(TEXT("post-apply failure reports a known effect"), Save.Outcome, EHyperAIStudioDomainDispatchOutcome::FailedAfterKnownEffect);

	Payload->Ops[0].ExpressionClass = TEXT("Drifted");
	TestEqual(TEXT("semantic drift rejected"),
		Adapter.Execute(MakeMutationContext(EHyperAIStudioDomainExecutionActionKind::Apply), *Payload).StatusCode,
		FString(TEXT("typed_payload_drift")));

	FHyperAIStudioDomainDispatchContext WrongSafety = MakeMutationContext(EHyperAIStudioDomainExecutionActionKind::Apply);
	WrongSafety.Safety = EHyperAIStudioDomainSafety::Read;
	TestEqual(TEXT("wrong safety rejected"), Adapter.Execute(WrongSafety, *Clone).StatusCode, FString(TEXT("typed_binding_mismatch")));

	FHyperAIStudioTextureGraphFreshVerifier Verifier;
	FHyperAIStudioTextureGraphMutationResultPayload NotCompleted;
	NotCompleted.Phase = TEXT("saved");
	NotCompleted.ContentKey = TEXT("sha256:") + FString::ChrN(64, TEXT('2'));
	FString Postcondition;
	FString VerifyError;
	TestFalse(TEXT("only a completed capture verifies"), Verifier.VerifyFreshExact(*Clone, NotCompleted, Postcondition, VerifyError));
	TestTrue(TEXT("no postcondition on failure"), Postcondition.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioTextureGraphEndToEndTest,
	"HyperAIStudio.TextureGraph.Mutation.EndToEndCreateEditExport",
	HyperAIStudio::TextureGraph::Tests::Flags)

bool FHyperAIStudioTextureGraphEndToEndTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::TextureGraph::Tests;
	namespace Gate = HyperAIStudio::TextureGraph::Gate;
	// The first session creates and exports the fixture; later sessions load it and edit it. Both leave it saved in
	// Content/__HyperAIStudioTests/TextureGraph, which can be deleted once testing is done.
	const FString Name = TEXT("TG_HyperAIE2E");
	const FString TargetPath = Folder / Name + TEXT(".") + Name;
	const FString TextureName = TEXT("T_HyperAIE2E");

	FHyperAITextureGraphInspectRequest InspectRequest;
	InspectRequest.TargetPath = TargetPath;
	InspectRequest.bLoad = true;
	InspectRequest.bIncludeExpressionCatalog = true;
	InspectRequest.MaxOutputBytes = FHyperAIStudioTextureGraphContracts::MaxOutputBytes;
	const FHyperAITextureGraphInspectReport Before = FHyperAIStudioTextureGraphContracts::Inspect(InspectRequest);
	const bool bCreate = Before.Status == TEXT("target_not_found");
	TestTrue(*FString::Printf(TEXT("fixture inspects or is absent (%s: %s)"), *Before.Status, *Before.Diagnostic), Before.bOk || bCreate);
	TestTrue(TEXT("expression catalog lists Noise"), Before.ExpressionClasses.Contains(TEXT("TG_Expression_Noise")));
	if (!Before.bOk && !bCreate)
	{
		return false;
	}

	FHyperAIStudioTextureGraphEditOpsPayload Payload;
	Payload.TargetPath = TargetPath;
	Payload.bCreate = bCreate;
	Payload.bSave = true;
	Payload.bExport = Before.bEngineAvailable;
	FString ExpectedFrequency = TEXT("8");
	if (bCreate)
	{
		Payload.Ops = {
			AddNode(TEXT("noise"), TEXT("Noise")),
			Op(TEXT("set_pin_value"), TEXT("noise"), TEXT("Frequency"), *ExpectedFrequency),
			Connect(TEXT("noise"), TEXT("Output"), TEXT("0"), TEXT("Source")),
			SetOutput(TextureName),
			Op(TEXT("set_node_comment"), TEXT("noise"), TEXT(""), TEXT("Base noise"))};
	}
	else
	{
		const FHyperAITextureGraphNode* Noise = Before.Nodes.FindByPredicate([](const FHyperAITextureGraphNode& Node)
		{
			return Node.ExpressionClass == TEXT("TG_Expression_Noise");
		});
		const FHyperAITextureGraphPin* Pin = Noise ? Noise->Pins.FindByPredicate([](const FHyperAITextureGraphPin& Candidate)
		{
			return Candidate.Name == TEXT("Frequency");
		}) : nullptr;
		if (!Pin)
		{
			AddError(TEXT("The saved fixture has no Noise Frequency pin; delete Content/__HyperAIStudioTests/TextureGraph and rerun."));
			return false;
		}
		ExpectedFrequency = FCString::Atof(*Pin->Value) == 8.0f ? TEXT("6") : TEXT("8");
		Payload.BaseRevision = Before.Revision;
		Payload.Ops = {Op(TEXT("set_pin_value"), *FString::FromInt(Noise->NodeId), TEXT("Frequency"), *ExpectedFrequency)};
	}
	Payload.SemanticFingerprint = FHyperAIStudioTextureGraphContracts::ComputeEditOpsSemanticFingerprint(Payload);

	if (FHyperAIStudioTextureGraphContracts::IsRegistrationAllowed(FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled()))
	{
		// Catalog-admitted: run the real journaled pipeline.
		FHyperAITextureGraphApplyPlanRequest Plan;
		Plan.TargetPath = TargetPath;
		Plan.bCreate = bCreate;
		Plan.ExpectedRevision = Payload.BaseRevision;
		Plan.Ops = Payload.Ops;
		Plan.bExport = Payload.bExport;
		const FHyperAITextureGraphApplyPlanReport DryRun = FHyperAIStudioTextureGraphContracts::BuildPlan(Plan);
		TestTrue(*FString::Printf(TEXT("dry run prepares (%s: %s)"), *DryRun.Status, *DryRun.Diagnostic), DryRun.bOk && DryRun.bTrustedPrepared);
		if (!DryRun.bOk)
		{
			return false;
		}
		Plan.bDryRun = false;
		Plan.OperationId = TEXT("texture-graph-e2e-") + FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
		Plan.ExpectedPlanHash = DryRun.PlanHash;
		const FHyperAITextureGraphApplyPlanReport Submitted = FHyperAIStudioTextureGraphContracts::BuildPlan(Plan);
		TestTrue(*FString::Printf(TEXT("submitted (%s: %s)"), *Submitted.Status, *Submitted.Diagnostic), Submitted.bExecutionSubmitted);
		FHyperAIStudioTypedArtifactOperationStatus Status;
		FString StatusError;
		PumpUntil([&]()
		{
			return !Submitted.bExecutionSubmitted
				|| (FHyperAIStudioTrustedExecutionFacade::QueryStatus(Plan.OperationId, Status, StatusError) && Status.bTerminal);
		}, 30.0);
		TestEqual(*FString::Printf(TEXT("operation completed (%s)"), *Status.Diagnostic), Status.Status, FString(TEXT("completed")));
	}
	else
	{
		// Not catalog-admitted yet: drive the adapter through the executor's exact phase order instead.
		AddInfo(TEXT("texture_graph is not in the generated catalog; exercising adapter phases directly."));
		FHyperAIStudioTextureGraphDomainAdapter Adapter;
		TArray<EHyperAIStudioDomainExecutionActionKind> Phases = {EHyperAIStudioDomainExecutionActionKind::Apply};
		if (Payload.bExport)
		{
			Phases.Add(EHyperAIStudioDomainExecutionActionKind::Compile);
		}
		Phases.Append({EHyperAIStudioDomainExecutionActionKind::Validate, EHyperAIStudioDomainExecutionActionKind::Save,
			EHyperAIStudioDomainExecutionActionKind::VerifyFresh});
		FHyperAIStudioDomainAdapterResult Last;
		for (const EHyperAIStudioDomainExecutionActionKind Phase : Phases)
		{
			Last = Adapter.Execute(MakeMutationContext(Phase), Payload);
			TestEqual(*FString::Printf(TEXT("phase %d succeeds (%s: %s)"), static_cast<int32>(Phase), *Last.StatusCode, *Last.Diagnostic),
				Last.Outcome, EHyperAIStudioDomainDispatchOutcome::Succeeded);
			if (Last.Outcome != EHyperAIStudioDomainDispatchOutcome::Succeeded)
			{
				return false;
			}
		}
		FHyperAIStudioTextureGraphFreshVerifier Verifier;
		FString Postcondition;
		FString VerifyError;
		TestTrue(*FString::Printf(TEXT("fresh verification passes (%s)"), *VerifyError),
			Last.Payload.IsValid() && Verifier.VerifyFreshExact(Payload, *Last.Payload, Postcondition, VerifyError));
	}

	InspectRequest.bIncludeExpressionCatalog = false;
	const FHyperAITextureGraphInspectReport After = FHyperAIStudioTextureGraphContracts::Inspect(InspectRequest);
	TestTrue(TEXT("graph inspects after the edit"), After.bOk);
	TestFalse(TEXT("graph saved"), After.bPackageDirty);
	TestNotEqual(TEXT("revision moved"), After.Revision, Before.Revision);
	const FHyperAITextureGraphNode* Noise = After.Nodes.FindByPredicate([](const FHyperAITextureGraphNode& Node)
	{
		return Node.ExpressionClass == TEXT("TG_Expression_Noise");
	});
	const FHyperAITextureGraphPin* Frequency = Noise ? Noise->Pins.FindByPredicate([](const FHyperAITextureGraphPin& Pin)
	{
		return Pin.Name == TEXT("Frequency");
	}) : nullptr;
	TestTrue(*FString::Printf(TEXT("frequency is %s (%s)"), *ExpectedFrequency, Frequency ? *Frequency->Value : TEXT("missing")),
		Frequency && FCString::Atof(*Frequency->Value) == FCString::Atof(*ExpectedFrequency));
	TestTrue(TEXT("noise feeds the output"), After.Edges.ContainsByPredicate([&](const FHyperAITextureGraphEdge& Edge)
	{
		return Noise && Edge.FromNode == Noise->NodeId && Edge.ToPin == TEXT("Source");
	}));
	TestTrue(TEXT("comment kept"), Noise && Noise->Comment == TEXT("Base noise"));
	TestTrue(TEXT("added node was laid out left of the output"), Noise && Noise->PosX < 0);

	if (!Payload.bExport)
	{
		AddWarning(TEXT("Texture Graph's engine is not running in this session, so export was not exercised."));
		return true;
	}
	FHyperAITextureGraphValidateRequest ValidateRequest;
	ValidateRequest.TargetPath = TargetPath;
	ValidateRequest.Policy = TEXT("exported");
	FHyperAITextureGraphValidateReport Exported;
	const bool bFinished = PumpUntil([&]()
	{
		Exported = FHyperAIStudioTextureGraphContracts::Validate(ValidateRequest);
		return Exported.bValid;
	}, 60.0);
	FString Codes;
	for (const FHyperAITextureGraphIssue& Issue : Exported.Issues)
	{
		Codes += Issue.Code + TEXT(" ");
	}
	TestTrue(*FString::Printf(TEXT("export finishes and saves the texture (%s; %s)"), *Exported.Diagnostic, *Codes), bFinished);
	TestTrue(TEXT("the texture is where the output says"),
		Exported.Outputs.Num() == 1 && Exported.Outputs[0].TexturePath == Folder / TextureName + TEXT(".") + TextureName
			&& Exported.Outputs[0].TextureState == TEXT("saved"));
	return true;
}

#endif
