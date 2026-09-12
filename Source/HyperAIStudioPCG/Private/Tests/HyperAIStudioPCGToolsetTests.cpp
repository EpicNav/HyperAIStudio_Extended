// Games by Hyper 2026.

#include "HyperAIStudioPCGToolset.h"

#include "HyperAIStudioExtensionRuntime.h"
#include "Misc/AutomationTest.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace HyperAIStudio::PCG::Tests
{
	FString Hash(const FString& Value)
	{
		return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Value);
	}

	FHyperAIPCGPinRecord Pin(
		const FString& Id,
		const FString& Direction,
		const int32 EdgeCount)
	{
		FHyperAIPCGPinRecord Value;
		Value.PinId = Id;
		Value.Direction = Direction;
		Value.Label = Direction == TEXT("input") ? TEXT("In") : TEXT("Out");
		Value.bAllowsMultipleConnections = true;
		Value.bAllowsMultipleData = true;
		Value.EdgeCount = EdgeCount;
		Value.AllowedTypeFingerprint = Hash(TEXT("type-any"));
		Value.PropertiesFingerprint = Hash(TEXT("pin-properties"));
		return Value;
	}

	FHyperAIStudioPCGValueSnapshot Snapshot()
	{
		FHyperAIStudioPCGValueSnapshot Value;
		Value.Graph.TargetPath = TEXT("/Game/PCG/G_Test.G_Test");
		Value.Graph.ClassPath = TEXT("/Script/PCG.PCGGraph");
		Value.Graph.PackageName = TEXT("/Game/PCG/G_Test");
		Value.Graph.bLoaded = true;
		Value.Graph.bWasLoadedFromDisk = true;
		Value.Graph.DiskExistence = TEXT("exists");
		Value.Graph.PackageSavedHash = TEXT("0123456789012345678901234567890123456789");
		Value.Graph.DiskSize = 1024;
		Value.Graph.GraphPropertiesFingerprint = Hash(TEXT("graph-properties"));
		Value.Graph.UserParametersFingerprint = Hash(TEXT("graph-parameters"));
		Value.Graph.NodeCount = 2;
		Value.Graph.EdgeCount = 1;

		FHyperAIPCGNodeRecord Input;
		Input.NodeId = TEXT("node-input");
		Input.Role = TEXT("graph_input");
		Input.SettingsInterfaceClass = TEXT("/Script/PCG.PCGGraphInputOutputSettings");
		Input.SettingsClass = Input.SettingsInterfaceClass;
		Input.SettingsFingerprint = Hash(TEXT("input-settings"));
		Input.bSettingsProjectionComplete = true;
		Input.NodePropertiesFingerprint = Hash(TEXT("input-node-properties"));
		Input.OutputPins.Add(Pin(TEXT("pin-from"), TEXT("output"), 1));
		Input.SemanticFingerprint = FHyperAIStudioPCGContracts::ComputeNodeFingerprint(Input);

		FHyperAIPCGNodeRecord Output;
		Output.NodeId = TEXT("node-output");
		Output.Role = TEXT("graph_output");
		Output.SettingsInterfaceClass = TEXT("/Script/PCG.PCGGraphInputOutputSettings");
		Output.SettingsClass = Output.SettingsInterfaceClass;
		Output.SettingsFingerprint = Hash(TEXT("output-settings"));
		Output.bSettingsProjectionComplete = true;
		Output.NodePropertiesFingerprint = Hash(TEXT("output-node-properties"));
		Output.InputPins.Add(Pin(TEXT("pin-to"), TEXT("input"), 1));
		Output.SemanticFingerprint = FHyperAIStudioPCGContracts::ComputeNodeFingerprint(Output);
		Value.Nodes = {Input, Output};

		FHyperAIStudioPCGEdgeState Edge;
		Edge.FromNodeId = Input.NodeId;
		Edge.FromPinId = Input.OutputPins[0].PinId;
		Edge.ToNodeId = Output.NodeId;
		Edge.ToPinId = Output.InputPins[0].PinId;
		Edge.EdgeId = FHyperAIStudioPCGContracts::ComputeEdgeFingerprint(Edge);
		Value.Edges.Add(Edge);
		Value.Graph.PersistedRevision =
			FHyperAIStudioPCGContracts::ComputePersistedRevision(Value);
		Value.Graph.bRevisionComplete =
			FHyperAIStudioPCGContracts::IsCanonicalSha256(Value.Graph.PersistedRevision);
		Value.bComplete = Value.Graph.bRevisionComplete;
		return Value;
	}

	FHyperAIPCGComponentRecord Component()
	{
		FHyperAIPCGComponentRecord Value;
		Value.bPresent = true;
		Value.ComponentPath =
			TEXT("/Game/Maps/Test.Test:PersistentLevel.PCGActor.PCGComponent");
		Value.ComponentClassPath = TEXT("/Script/PCG.PCGComponent");
		Value.OwnerPath = TEXT("/Game/Maps/Test.Test:PersistentLevel.PCGActor");
		Value.GraphPath = TEXT("/Game/PCG/G_Test.G_Test");
		Value.GraphInstancePath =
			TEXT("/Game/Maps/Test.Test:PersistentLevel.PCGActor.PCGComponent:PCGGraphInstance");
		Value.GraphInterfacePath = Value.GraphPath;
		Value.PackageName = TEXT("/Game/Maps/Test");
		Value.bWasLoadedFromDisk = true;
		Value.DiskExistence = TEXT("exists");
		Value.PackageSavedHash = TEXT("0123456789012345678901234567890123456789");
		Value.DiskSize = 2048;
		Value.ComponentPropertiesFingerprint = Hash(TEXT("component-properties"));
		Value.GraphInstancePropertiesFingerprint = Hash(TEXT("graph-instance-properties"));
		Value.InstanceParametersFingerprint = Hash(TEXT("instance-parameters"));
		Value.InstanceParameterCount = 2;
		Value.InstanceOverrideMaskFingerprint = Hash(TEXT("instance-override-mask"));
		Value.InstanceOverrideCount = 1;
		Value.SchedulingPolicyFingerprint = Hash(TEXT("scheduling-policy"));
		Value.OwnerTransformFingerprint = Hash(TEXT("owner-transform"));
		Value.bActivated = true;
		Value.Seed = 42;
		Value.GenerationTrigger = 1;
		Value.GenerationTaskId = TEXT("0");
		Value.CleanupTaskId = TEXT("0");
		Value.bVolatileStateStable = true;
		Value.PersistedFingerprint =
			FHyperAIStudioPCGContracts::ComputeComponentPersistedFingerprint(Value);
		Value.bPersistedProjectionComplete =
			FHyperAIStudioPCGContracts::IsCanonicalSha256(Value.PersistedFingerprint);
		Value.VolatileObservationFingerprint =
			FHyperAIStudioPCGContracts::ComputeComponentVolatileObservationFingerprint(Value);
		return Value;
	}

	TSharedRef<FHyperAIStudioPCGLifecyclePayload, ESPMode::ThreadSafe> LifecyclePayload()
	{
		const TSharedRef<FHyperAIStudioPCGLifecyclePayload, ESPMode::ThreadSafe> Value =
			MakeShared<FHyperAIStudioPCGLifecyclePayload, ESPMode::ThreadSafe>();
		Value->GraphPath = TEXT("/Game/PCG/G_Test.G_Test");
		Value->ComponentPath =
			TEXT("/Game/Maps/Test.Test:PersistentLevel.PCGActor.PCGComponent");
		Value->BasePersistedRevision = Hash(TEXT("graph-revision"));
		Value->BaseComponentFingerprint = Hash(TEXT("component-revision"));
		Value->BaseVolatileObservationFingerprint = Hash(TEXT("component-observation"));
		Value->Lifecycle = TEXT("generate_validate_cleanup");
		Value->ValidationPolicy = TEXT("generation_ready");
		Value->ExpectedTerminalState = TEXT("clean");
		Value->AsyncDeadlineMs = 60000;
		Value->bRemoveGeneratedComponentsOnCleanup = true;
		Value->SemanticFingerprint =
			FHyperAIStudioPCGContracts::ComputeLifecycleSemanticFingerprint(*Value);
		return Value;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioPCGManifestDelegationTest,
	"HyperAIStudio.NativeTools.PCG.ManifestDelegationAndUnsupportedMatrix",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioPCGManifestDelegationTest::RunTest(const FString& Parameters)
{
	const auto& Manifest = FHyperAIStudioPCGContracts::GetManifest();
	TestEqual(TEXT("pack id"), FString(FHyperAIStudioPCGContracts::PackId),
		FString(TEXT("pcg")));
	TestEqual(TEXT("optional module type"),
		FString(FHyperAIStudioPCGContracts::RequiredModuleType), FString(TEXT("Editor")));
	TestEqual(TEXT("explicit loading phase"),
		FString(FHyperAIStudioPCGContracts::RequiredLoadingPhase), FString(TEXT("None")));
	TestEqual(TEXT("atomic source cohort"),
		FString(FHyperAIStudioPCGContracts::AtomicCohortId),
		FString(TEXT("cohort.source.hyperaistudiopcgtoolset.v1")));
	TestEqual(TEXT("exact blocking PCG group"),
		FString(FHyperAIStudioPCGContracts::RequirementGroupId),
		FString(TEXT("pcg")));
	TestEqual(TEXT("exact live probe id"),
		FString(FHyperAIStudioPCGContracts::LiveProbeId),
		FString(TEXT("probe.pcg")));
	TestEqual(TEXT("Exactly three AICallable contracts"), Manifest.Num(), 3);
	TestEqual(TEXT("Inspect name"), Manifest[0].Name, FString(TEXT("hyper_pcg_inspect")));
	TestEqual(TEXT("Apply name"), Manifest[1].Name, FString(TEXT("hyper_pcg_apply_plan")));
	TestEqual(TEXT("Validate name"), Manifest[2].Name, FString(TEXT("hyper_pcg_validate")));
	for (const FHyperAIStudioPCGManifestEntry& Entry : Manifest)
	{
		TestEqual(TEXT("Atomic cohort has one owner"), Entry.QualifiedToolset,
			FString(TEXT("HyperAIStudioPCG.HyperAIStudioPCGToolset")));
	}
	const auto& Epic = FHyperAIStudioPCGContracts::GetEpicDelegates();
	const auto& Requirements = FHyperAIStudioPCGContracts::GetCapabilityRequirementCoordinates();
	TestEqual(TEXT("All Epic PCG callables delegated exactly"), Epic.Num(), 31);
	TestEqual(TEXT("All product PCG requirements represented exactly"), Requirements.Num(), 16);
	TSet<FString> UniqueEpic;
	TSet<FString> UniqueRequirements;
	for (const FString& Name : Epic) UniqueEpic.Add(Name);
	for (const FString& Coordinate : Requirements) UniqueRequirements.Add(Coordinate);
	TestEqual(TEXT("Epic delegate names are unique"), UniqueEpic.Num(), 31);
	TestEqual(TEXT("Product requirement coordinates are unique"), UniqueRequirements.Num(), 16);
	const TArray<FString> ExactEpic = {
		TEXT("CreateGraph"), TEXT("GetGraphStructure"), TEXT("SetGraphParams"),
		TEXT("RemoveGraphParams"), TEXT("GetGraphSchema"), TEXT("GetGraphDescription"),
		TEXT("SetGraphDescription"), TEXT("ListGraphInstances"), TEXT("SpawnGraphInstance"),
		TEXT("ExecuteGraphInstance"), TEXT("GetGraphInstanceParams"),
		TEXT("SetGraphInstanceParams"), TEXT("ResetGraphInstanceParams"),
		TEXT("ListNativeNodes"), TEXT("ListAvailableSubgraphs"), TEXT("GetNativeNodeSchema"),
		TEXT("AddNode"), TEXT("AddSubgraphNode"), TEXT("UpdateNode"),
		TEXT("SetNodeComment"), TEXT("GetNodeInfo"), TEXT("RepositionNode"),
		TEXT("RemoveNode"), TEXT("ConnectNodePins"), TEXT("DisconnectNodePins"),
		TEXT("GetNodeDataView"), TEXT("AddCommentBox"), TEXT("UpdateCommentBox"),
		TEXT("RemoveCommentBox"), TEXT("DrawSpline"), TEXT("RunPCGInstantGraph")};
	TestTrue(TEXT("Delegation matrix order is frozen to UE 5.8 audit"), Epic == ExactEpic);
	const TArray<FString> ExactRequirements = {
		TEXT("capability.pcg.graph.create"), TEXT("capability.pcg.graph.inspect"),
		TEXT("capability.pcg.graph.configure"), TEXT("capability.pcg.graph.execute"),
		TEXT("capability.pcg.graph.parameters"), TEXT("capability.pcg.node.add"),
		TEXT("capability.pcg.node.configure"), TEXT("capability.pcg.node.comment"),
		TEXT("capability.pcg.node.connect"), TEXT("capability.pcg.graph.instance"),
		TEXT("capability.pcg.graph.generate"), TEXT("capability.pcg.volume.spawn"),
		TEXT("capability.pcg.spline.draw"), TEXT("capability.pcg.subgraph.add"),
		TEXT("capability.pcg.worldgen.capture"), TEXT("capability.pcg.lifecycle.validate")};
	TestTrue(TEXT("Product requirement order is deterministic"),
		Requirements == ExactRequirements);
	TestTrue(TEXT("Spatial callable delegated"), Epic.Contains(TEXT("RunPCGInstantGraph")));
	TestTrue(TEXT("Graph authoring delegated"), Epic.Contains(TEXT("CreateGraph"))
		&& Epic.Contains(TEXT("DrawSpline")));
	const auto Matrix = FHyperAIStudioPCGContracts::GetCapabilityMatrix();
	TestEqual(TEXT("One truthful PCG capability row"), Matrix.Num(), 1);
	TestFalse(TEXT("Non-dry execution is not implemented"),
		Matrix[0].bMutationExecutionImplemented);
	TestTrue(TEXT("No-load case explicit"), Matrix[0].UnsupportedCases.Contains(
		TEXT("asset_or_map_load_open_or_broad_asset_registry_materialization")));
	TestTrue(TEXT("Volatile cursor case explicit"), Matrix[0].UnsupportedCases.Contains(
		TEXT("paging_while_component_task_or_refresh_is_volatile")));
	TestFalse(TEXT("Source cohort remains production fail-closed"),
		FHyperAIStudioPCGContracts::IsRegistrationAllowed(false));
	const FHyperAIStudioDomainAdapterDescriptor& Descriptor =
		FHyperAIStudioPCGContracts::GetAdapterDescriptor();
	TestEqual(TEXT("exact three adapter variants"), Descriptor.Variants.Num(), 3);
	TestEqual(TEXT("lifecycle authority remains ExternalEffect"),
		static_cast<uint8>(Descriptor.Variants[1].Safety),
		static_cast<uint8>(EHyperAIStudioDomainSafety::ExternalEffect));
	TestTrue(TEXT("PCG requirement groups are blocking"),
		Descriptor.ApplicableNonBlockingRequirementGroupIds.IsEmpty());
	TestEqual(TEXT("Inspect result explicitly pages both structural inventories"),
		FHyperAIStudioPCGContracts::InspectResultSchemaFingerprint(),
		FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
			TEXT("pcg.inspect.result.v1|exact_graph|separate_component_persisted_and_volatile|paged_nodes_and_edges|issues|capabilities")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioPCGBoundsAndNoLoadTest,
	"HyperAIStudio.NativeTools.PCG.BoundsBeforeProjectionAndNoLoad",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioPCGBoundsAndNoLoadTest::RunTest(const FString& Parameters)
{
	FString Error;
	TestFalse(TEXT("Negative count rejected before projection"),
		FHyperAIStudioPCGContracts::AdmitContainerBeforeProjection(-1, 0, 0, 64, Error));
	TestEqual(TEXT("Negative count reason"), Error, FString(TEXT("container_count_exceeded")));
	TestFalse(TEXT("513 elements rejected before allocation"),
		FHyperAIStudioPCGContracts::AdmitContainerBeforeProjection(513, 0, 0, 64, Error));
	TestFalse(TEXT("Aggregate work rejected"),
		FHyperAIStudioPCGContracts::AdmitContainerBeforeProjection(
			2, FHyperAIStudioPCGContracts::MaxProjectionWorkUnits - 1, 0, 1, Error));
	TestFalse(TEXT("Aggregate bytes rejected"),
		FHyperAIStudioPCGContracts::AdmitContainerBeforeProjection(
			2, 0, FHyperAIStudioPCGContracts::MaxProjectionBytes - 1, 1, Error));
	TestTrue(TEXT("Small container admitted"),
		FHyperAIStudioPCGContracts::AdmitContainerBeforeProjection(4, 8, 64, 32, Error));
	TestTrue(TEXT("Compact sparse container admitted"),
		FHyperAIStudioPCGContracts::AdmitSparseContainerBeforeProjection(
			4, 4, 8, 64, 32, Error));
	TestFalse(TEXT("Sparse container slot fanout rejected before iteration"),
		FHyperAIStudioPCGContracts::AdmitSparseContainerBeforeProjection(
			1, FHyperAIStudioPCGContracts::MaxContainerElements * 2 + 1,
			0, 0, static_cast<int32>(sizeof(FGuid)), Error));
	TestEqual(TEXT("Sparse slot reason"), Error,
		FString(TEXT("sparse_container_slot_count_exceeded")));
	TSet<FGuid> SparseOverrideIds;
	for (int32 Index = 0;
		Index < FHyperAIStudioPCGContracts::MaxContainerElements * 4; ++Index)
	{
		SparseOverrideIds.Add(FGuid(1, 2, 3, static_cast<uint32>(Index + 1)));
	}
	for (int32 Index = 0;
		Index < FHyperAIStudioPCGContracts::MaxContainerElements * 4 - 1; ++Index)
	{
		SparseOverrideIds.Remove(FGuid(1, 2, 3, static_cast<uint32>(Index + 1)));
	}
	TestEqual(TEXT("Sparse override fixture keeps one value"), SparseOverrideIds.Num(), 1);
	TestTrue(TEXT("Sparse override fixture retains high slot fanout"),
		SparseOverrideIds.GetMaxIndex()
			> FHyperAIStudioPCGContracts::MaxContainerElements * 2);
	TestFalse(TEXT("Actual sparse override shape is rejected before range iteration"),
		FHyperAIStudioPCGContracts::AdmitSparseContainerBeforeProjection(
			SparseOverrideIds.Num(), SparseOverrideIds.GetMaxIndex(), 0, 0,
			static_cast<int32>(sizeof(FGuid)), Error));
	TestEqual(TEXT("Asset Registry unknown is never absence"),
		FHyperAIStudioPCGContracts::ClassifyAssetRegistryExistence(
			UE::AssetRegistry::EExists::Unknown), FString(TEXT("unknown")));
	TestTrue(TEXT("Canonical graph path"), FHyperAIStudioPCGContracts::IsCanonicalGraphPath(
		TEXT("/Game/PCG/G_Test.G_Test")));
	TestFalse(TEXT("Search expression is not a graph path"),
		FHyperAIStudioPCGContracts::IsCanonicalGraphPath(TEXT("/Game/PCG/*.PCGGraph")));
	TestTrue(TEXT("Canonical component subobject path"),
		FHyperAIStudioPCGContracts::IsCanonicalComponentPath(
			TEXT("/Game/Maps/Test.Test:PersistentLevel.Actor.PCGComponent")));
	TestNull(TEXT("ResolveObject does not materialize an unloaded graph"),
		FHyperAIStudioPCGContracts::ResolveAlreadyLoadedGraph(
			TEXT("/Game/PCG/DefinitelyNotLoaded_PCGAudit.DefinitelyNotLoaded_PCGAudit")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioPCGCASAndValidatorTest,
	"HyperAIStudio.NativeTools.PCG.CASAndIndependentValueValidator",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioPCGCASAndValidatorTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::PCG::Tests;
	FHyperAIStudioPCGValueSnapshot Base = Snapshot();
	TestTrue(TEXT("Synthetic graph revision complete"), Base.Graph.bRevisionComplete);
	TArray<FHyperAIPCGIssue> Issues;
	FString ValidatorFingerprint;
	TestTrue(TEXT("Detached validator completes"),
		FHyperAIStudioPCGContracts::ValidateValueSnapshot(
			Base, TEXT("structural"), 32, Issues, ValidatorFingerprint));
	TestEqual(TEXT("Closed structural snapshot has no errors"), Issues.Num(), 0);
	TestTrue(TEXT("Validator result sealed"),
		FHyperAIStudioPCGContracts::IsCanonicalSha256(ValidatorFingerprint));

	FHyperAIStudioPCGValueSnapshot PinDrift = Base;
	PinDrift.Nodes[0].OutputPins[0].Label = TEXT("OutChanged");
	PinDrift.Nodes[0].SemanticFingerprint =
		FHyperAIStudioPCGContracts::ComputeNodeFingerprint(PinDrift.Nodes[0]);
	const FString PinDriftRevision =
		FHyperAIStudioPCGContracts::ComputePersistedRevision(PinDrift);
	TestNotEqual(TEXT("Same counts but changed pin content changes graph CAS"),
		PinDriftRevision, Base.Graph.PersistedRevision);
	FHyperAIStudioPCGValueSnapshot SettingsDrift = Base;
	SettingsDrift.Nodes[0].SettingsFingerprint = Hash(TEXT("changed-settings"));
	SettingsDrift.Nodes[0].SemanticFingerprint =
		FHyperAIStudioPCGContracts::ComputeNodeFingerprint(SettingsDrift.Nodes[0]);
	TestNotEqual(TEXT("Same counts but changed settings changes graph CAS"),
		FHyperAIStudioPCGContracts::ComputePersistedRevision(SettingsDrift),
		Base.Graph.PersistedRevision);
	FHyperAIStudioPCGValueSnapshot DirtyDrift = Base;
	DirtyDrift.Graph.bPackageDirty = true;
	TestNotEqual(TEXT("Package dirty state changes graph CAS"),
		FHyperAIStudioPCGContracts::ComputePersistedRevision(DirtyDrift),
		Base.Graph.PersistedRevision);
	FHyperAIStudioPCGValueSnapshot SavedIdentityDrift = Base;
	SavedIdentityDrift.Graph.PackageSavedHash = TEXT("1123456789012345678901234567890123456789");
	TestNotEqual(TEXT("Persisted package identity changes graph CAS"),
		FHyperAIStudioPCGContracts::ComputePersistedRevision(SavedIdentityDrift),
		Base.Graph.PersistedRevision);
	FHyperAIStudioPCGValueSnapshot ParameterDrift = Base;
	ParameterDrift.Graph.UserParametersFingerprint = Hash(TEXT("changed-parameters"));
	TestNotEqual(TEXT("Graph parameter projection changes graph CAS"),
		FHyperAIStudioPCGContracts::ComputePersistedRevision(ParameterDrift),
		Base.Graph.PersistedRevision);

	FHyperAIStudioPCGValueSnapshot StaleNodeSeal = Base;
	StaleNodeSeal.Nodes[0].OutputPins[0].Label = TEXT("OutChangedWithoutReseal");
	Issues.Reset();
	TestTrue(TEXT("Detached stale node values produce sealed invalid evidence"),
		FHyperAIStudioPCGContracts::ValidateValueSnapshot(
			StaleNodeSeal, TEXT("structural"), 32, Issues, ValidatorFingerprint));
	TestTrue(TEXT("Detached validator recomputes node value seals"),
		Issues.ContainsByPredicate([](const FHyperAIPCGIssue& Issue)
		{
			return Issue.Code == TEXT("node_settings_projection_incomplete")
				|| Issue.Code == TEXT("persisted_revision_value_mismatch");
		}));

	FHyperAIStudioPCGValueSnapshot Dangling = Base;
	Dangling.Edges[0].ToPinId = TEXT("missing-pin");
	Issues.Reset();
	TestTrue(TEXT("Invalid values still produce complete validation evidence"),
		FHyperAIStudioPCGContracts::ValidateValueSnapshot(
			Dangling, TEXT("structural"), 32, Issues, ValidatorFingerprint));
	TestTrue(TEXT("Dangling edge found independently"), Issues.ContainsByPredicate(
		[](const FHyperAIPCGIssue& Issue)
		{
			return Issue.Code == TEXT("dangling_or_duplicate_edge");
		}));
	TArray<FHyperAIPCGIssue> TruncatedIssues;
	FString TruncatedFingerprint;
	TestFalse(TEXT("Issue truncation is never reported complete"),
		FHyperAIStudioPCGContracts::ValidateValueSnapshot(
			Dangling, TEXT("structural"), 1, TruncatedIssues, TruncatedFingerprint));
	FHyperAIStudioPCGValueSnapshot Unsupported = Base;
	Unsupported.Graph.bRevisionComplete = false;
	Unsupported.Graph.PersistedRevision.Reset();
	Unsupported.bComplete = false;
	FHyperAIPCGIssue UnsupportedIssue;
	UnsupportedIssue.Code = TEXT("unsupported_property_kind");
	UnsupportedIssue.Severity = TEXT("error");
	UnsupportedIssue.StableId = Hash(TEXT("unsupported-property"));
	Unsupported.CaptureIssues.Add(UnsupportedIssue);
	Issues.Reset();
	TestTrue(TEXT("Unsupported values yield sealed invalid evidence, never PASS"),
		FHyperAIStudioPCGContracts::ValidateValueSnapshot(
			Unsupported, TEXT("structural"), 32, Issues, ValidatorFingerprint));
	TestTrue(TEXT("Unsupported/truncated property forces revision incomplete"),
		Issues.ContainsByPredicate([](const FHyperAIPCGIssue& Issue)
		{
			return Issue.Code == TEXT("revision_incomplete");
		}));

	FHyperAIStudioPCGValueSnapshot WithComponent = Base;
	WithComponent.Component = Component();
	Issues.Reset();
	TestTrue(TEXT("Generation-ready detached validator completes"),
		FHyperAIStudioPCGContracts::ValidateValueSnapshot(
			WithComponent, TEXT("generation_ready"), 32, Issues, ValidatorFingerprint));
	TestEqual(TEXT("Closed component snapshot is generation-ready"), Issues.Num(), 0);

	FHyperAIStudioPCGValueSnapshot ComponentDrift = WithComponent;
	const FString OriginalComponentFingerprint =
		ComponentDrift.Component.PersistedFingerprint;
	ComponentDrift.Component.InstanceOverrideMaskFingerprint =
		Hash(TEXT("changed-instance-override-mask"));
	TestNotEqual(TEXT("Override mask changes component CAS"),
		FHyperAIStudioPCGContracts::ComputeComponentPersistedFingerprint(
			ComponentDrift.Component), OriginalComponentFingerprint);
	Issues.Reset();
	TestTrue(TEXT("Component drift yields sealed invalid evidence"),
		FHyperAIStudioPCGContracts::ValidateValueSnapshot(
			ComponentDrift, TEXT("structural"), 32, Issues, ValidatorFingerprint));
	TestTrue(TEXT("Detached validator recomputes component CAS"),
		Issues.ContainsByPredicate([](const FHyperAIPCGIssue& Issue)
		{
			return Issue.Code == TEXT("component_persisted_value_mismatch");
		}));

	const FString GraphRevisionWithComponentA =
		FHyperAIStudioPCGContracts::ComputePersistedRevision(WithComponent);
	WithComponent.Component.bGenerating = true;
	WithComponent.Component.bVolatileStateStable = false;
	WithComponent.Component.VolatileObservationFingerprint =
		FHyperAIStudioPCGContracts::ComputeComponentVolatileObservationFingerprint(
			WithComponent.Component);
	TestEqual(TEXT("Volatile/component state never contaminates graph revision"),
		FHyperAIStudioPCGContracts::ComputePersistedRevision(WithComponent),
		GraphRevisionWithComponentA);
	TestEqual(TEXT("Volatile state never contaminates persisted component CAS"),
		FHyperAIStudioPCGContracts::ComputeComponentPersistedFingerprint(
			WithComponent.Component), OriginalComponentFingerprint);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioPCGCursorVolatilityTest,
	"HyperAIStudio.NativeTools.PCG.CursorRejectsLiveTaskStateChange",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioPCGCursorVolatilityTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::PCG::Tests;
	const FString GraphPath = TEXT("/Game/PCG/G_Test.G_Test");
	const FString ComponentPath =
		TEXT("/Game/Maps/Test.Test:PersistentLevel.Actor.PCGComponent");
	const FString Revision = Hash(TEXT("graph-revision"));
	const FString ComponentFingerprint = Hash(TEXT("component-fingerprint"));
	FHyperAIPCGComponentRecord Stable;
	Stable.bPresent = true;
	Stable.bPersistedProjectionComplete = true;
	Stable.PersistedFingerprint = ComponentFingerprint;
	Stable.bVolatileStateStable = true;
	TestTrue(TEXT("Terminal component permits paging"),
		FHyperAIStudioPCGContracts::IsPagingStable(Stable));
	const FString Cursor = FHyperAIStudioPCGContracts::BuildCursor(
		GraphPath, ComponentPath, Revision, ComponentFingerprint, 24, 24);
	int32 Offset = 0;
	TestTrue(TEXT("Stable cursor parses"), FHyperAIStudioPCGContracts::ParseCursor(
		Cursor, GraphPath, ComponentPath, Revision, ComponentFingerprint, 24, Offset));
	TestEqual(TEXT("Cursor offset"), Offset, 24);
	TestFalse(TEXT("Persisted component drift invalidates cursor"),
		FHyperAIStudioPCGContracts::ParseCursor(Cursor, GraphPath, ComponentPath,
			Revision, Hash(TEXT("changed-component")), 24, Offset));
	const FString EdgePageCursor = FHyperAIStudioPCGContracts::BuildCursor(
		GraphPath, ComponentPath, Revision, ComponentFingerprint, 24,
		FHyperAIStudioPCGContracts::MaxNodes + 1);
	TestTrue(TEXT("Combined cursor can address the bounded edge page"),
		FHyperAIStudioPCGContracts::ParseCursor(EdgePageCursor, GraphPath, ComponentPath,
			Revision, ComponentFingerprint, 24, Offset));
	TestTrue(TEXT("Cursor past the combined node/edge bound is never minted"),
		FHyperAIStudioPCGContracts::BuildCursor(GraphPath, ComponentPath, Revision,
			ComponentFingerprint, 24,
			FHyperAIStudioPCGContracts::MaxSnapshotItems + 1).IsEmpty());
	Stable.bGenerating = true;
	Stable.bVolatileStateStable = false;
	TestFalse(TEXT("Live task transition makes recaptured snapshot unpageable"),
		FHyperAIStudioPCGContracts::IsPagingStable(Stable));
	Stable.bGenerating = false;
	Stable.bCleaningUp = true;
	TestFalse(TEXT("Cleanup transition also makes snapshot unpageable"),
		FHyperAIStudioPCGContracts::IsPagingStable(Stable));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioPCGClonePrepareAndAdapterTest,
	"HyperAIStudio.NativeTools.PCG.ImmutableClonePurePrepareAndZeroEffectAdapter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioPCGClonePrepareAndAdapterTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::PCG::Tests;
	const auto Payload = LifecyclePayload();
	const TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> Clone =
		Payload->CloneImmutable();
	TestTrue(TEXT("Clone is detached"), &Clone.Get() != &Payload.Get());
	TestEqual(TEXT("Clone semantic seal"), Clone->GetSemanticFingerprint(),
		Payload->GetSemanticFingerprint());
	Payload->GraphPath = TEXT("/Game/PCG/G_Changed.G_Changed");
	TestNotEqual(TEXT("Clone does not alias mutable source fields"),
		static_cast<const FHyperAIStudioPCGLifecyclePayload&>(Clone.Get()).GraphPath,
		Payload->GraphPath);
	const auto VolatileDrift = LifecyclePayload();
	VolatileDrift->BaseVolatileObservationFingerprint = Hash(TEXT("changed-observation"));
	VolatileDrift->SemanticFingerprint =
		FHyperAIStudioPCGContracts::ComputeLifecycleSemanticFingerprint(*VolatileDrift);
	TestNotEqual(TEXT("Lifecycle intent seals the fresh volatile observation"),
		VolatileDrift->SemanticFingerprint, Clone->GetSemanticFingerprint());

	const auto PurePayload = LifecyclePayload();
	const auto& Descriptor = FHyperAIStudioPCGContracts::GetAdapterDescriptor();
	FHyperAIStudioDomainBinding Binding;
	Binding.PackId = FHyperAIStudioPCGContracts::PackId;
	Binding.ToolName = TEXT("hyper_pcg_apply_plan");
	Binding.VariantId = FHyperAIStudioPCGContracts::MutationVariantId;
	Binding.ExpectedSafety = EHyperAIStudioDomainSafety::ExternalEffect;
	Binding.CanonicalProjectId = TEXT("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
	Binding.ExpectedAdapterFingerprint = Descriptor.AdapterFingerprint;
	Binding.ExpectedAdapterGeneration = 1;
	Binding.ExpectedRegistryEpoch = 1;
	Binding.Prerequisites.PackId = Binding.PackId;
	Binding.Prerequisites.bPackEnabled = true;
	Binding.Prerequisites.Revision = 1;
	Binding.Prerequisites.Observations = {
		{TEXT("module.PCG"), EHyperAIStudioDomainPrerequisiteState::Available},
		{TEXT("module.PCGEditor"), EHyperAIStudioDomainPrerequisiteState::Available}};
	Binding.Prerequisites.Fingerprint =
		FHyperAIStudioDomainAdapterRegistry::ComputePrerequisiteFingerprint(Binding.Prerequisites);
	Binding.Admission.PackId = Binding.PackId;
	Binding.Admission.Revision = 1;
	Binding.Admission.Fingerprint =
		FHyperAIStudioDomainAdapterRegistry::ComputeAdmissionFingerprint(Binding.Admission);
	FHyperAIStudioTypedArtifactContract Contract;
	Contract.Binding = Binding;
	Contract.ArtifactTypeId = PurePayload->GetTypeId();
	Contract.ArtifactSchemaFingerprint = PurePayload->GetSchemaFingerprint();
	Contract.ArtifactSemanticFingerprint = PurePayload->GetSemanticFingerprint();
	Contract.EffectTarget = TEXT("pcg:test-component");
	Contract.DeadlineMs = 1000;
	Contract.MaxNativeOperations = 8;
	Contract.MaxGameThreadMs = 200;
	Contract.MaxOutputBytes = 8192;
	Contract.MaxResultBytes = 256;
	Contract.StageLifetimeMs = 15000;
	Contract.bSaveOnce = false;
	Contract.bValidateOnce = true;
	Contract.bVerifyFreshOnce = true;
	FHyperAIStudioPreparedTypedArtifact Prepared;
	FString Error;
	TestTrue(TEXT("Public pure typed-artifact Prepare seals the PCG intent"),
		FHyperAIStudioTypedArtifactExecutor::Prepare(Contract, Prepared, Error));
	TestTrue(TEXT("Pure plan hash is canonical"),
		FHyperAIStudioPCGContracts::IsCanonicalSha256(Prepared.PlanHash));

	FHyperAIStudioDomainDispatchContext Context;
	Context.Binding = Binding;
	Context.Safety = EHyperAIStudioDomainSafety::ExternalEffect;
	Context.ActionKind = EHyperAIStudioDomainExecutionActionKind::Apply;
	FHyperAIStudioPCGDomainAdapter Adapter;
	const FHyperAIStudioDomainAdapterResult Result = Adapter.Execute(Context, *PurePayload);
	TestEqual(TEXT("Adapter rejects before effect"),
		static_cast<uint8>(Result.Outcome),
		static_cast<uint8>(EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect));
	TestEqual(TEXT("Exact async continuation blocker"), Result.StatusCode,
		FString(FHyperAIStudioPCGContracts::NonDryCallableState));
	TestFalse(TEXT("Rejected adapter emits no result payload"), Result.Payload.IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioPCGReflectionContractTest,
	"HyperAIStudio.NativeTools.PCG.ExactAICallableReflectionContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioPCGReflectionContractTest::RunTest(const FString& Parameters)
{
	TSet<FString> Names;
	for (TFieldIterator<UFunction> It(UHyperAIStudioPCGToolset::StaticClass(),
		EFieldIteratorFlags::ExcludeSuper); It; ++It)
	{
		if (It->HasMetaData(TEXT("AICallable"))) Names.Add(It->GetName());
	}
	TestEqual(TEXT("Reflection exposes exactly three AICallables"), Names.Num(), 3);
	TestTrue(TEXT("Exact reflected inspect"), Names.Contains(TEXT("hyper_pcg_inspect")));
	TestTrue(TEXT("Exact reflected apply"), Names.Contains(TEXT("hyper_pcg_apply_plan")));
	TestTrue(TEXT("Exact reflected validate"), Names.Contains(TEXT("hyper_pcg_validate")));
	TestNull(TEXT("No raw script field"),
		FHyperAIPCGApplyPlanRequest::StaticStruct()->FindPropertyByName(TEXT("Script")));
	TestNull(TEXT("No raw JSON field"),
		FHyperAIPCGApplyPlanRequest::StaticStruct()->FindPropertyByName(TEXT("Json")));
	TestNull(TEXT("No client authorization token"),
		FHyperAIPCGApplyPlanRequest::StaticStruct()->FindPropertyByName(TEXT("AuthorizationToken")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
