// Games by Hyper 2026.

#include "HyperAIStudioNetworkingToolset.h"

#include "HAL/PlatformTime.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Character.h"
#include "GameFramework/DefaultPawn.h"
#include "Engine/GameInstance.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "Misc/AutomationTest.h"

#include <limits>

#if WITH_DEV_AUTOMATION_TESTS

namespace HyperAIStudio::Networking::Tests
{
	FString Hash(const FString& Value)
	{
		return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Value);
	}

	TSharedRef<FHyperAIStudioNetworkingTypedPayload, ESPMode::ThreadSafe> MakePayload(
		const FString& Domain = TEXT("network"),
		const FString& ToolName = TEXT("hyper_network_apply_plan"))
	{
		const auto Payload = MakeShared<FHyperAIStudioNetworkingTypedPayload, ESPMode::ThreadSafe>();
		Payload->Domain = Domain;
		Payload->ToolName = ToolName;
		Payload->BaseRevision = Hash(TEXT("networking-base"));
		FHyperAIStudioNetworkingBackendOperation Operation;
		Operation.Domain = Domain;
		Operation.Kind = EHyperAIStudioNetworkingOperationKind::NetworkClassSetReplication;
		Operation.Type = TEXT("class.set_replication");
		Operation.TargetPath = TEXT("/Game/Tests/BP_Network.BP_Network");
		Operation.ExpectedRevision = Hash(TEXT("networking-target"));
		Operation.Replicates = 1;
		Payload->Operations.Add(Operation);
		Payload->SemanticFingerprint =
			FHyperAIStudioNetworkingContracts::ComputePayloadSemanticFingerprint(
				Payload->Domain, Payload->ToolName, Payload->Operations, Payload->BaseRevision);
		return Payload;
	}

	bool PrepareArtifact(
		const TSharedRef<FHyperAIStudioNetworkingTypedPayload, ESPMode::ThreadSafe>& Payload,
		FHyperAIStudioPreparedTypedArtifact& OutPrepared,
		FString& OutError)
	{
		const auto& Descriptor = FHyperAIStudioNetworkingContracts::GetBaseAdapterDescriptor();
		FHyperAIStudioDomainBinding Binding;
		Binding.PackId = FHyperAIStudioNetworkingContracts::PackId;
		Binding.ToolName = Payload->ToolName;
		Binding.VariantId = Payload->Domain == TEXT("network")
			? FHyperAIStudioNetworkingContracts::NetworkMutationVariantId
			: FHyperAIStudioNetworkingContracts::FrameworkMutationVariantId;
		Binding.ExpectedSafety = EHyperAIStudioDomainSafety::Edit;
		Binding.CanonicalProjectId = TEXT("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
		Binding.ExpectedAdapterFingerprint = Descriptor.AdapterFingerprint;
		Binding.ExpectedAdapterGeneration = 1;
		Binding.ExpectedRegistryEpoch = 1;
		Binding.Prerequisites.PackId = Binding.PackId;
		Binding.Prerequisites.bPackEnabled = true;
		Binding.Prerequisites.Revision = 1;
		Binding.Prerequisites.Observations = {
			{TEXT("module.Engine"), EHyperAIStudioDomainPrerequisiteState::Available},
			{TEXT("module.EngineSettings"), EHyperAIStudioDomainPrerequisiteState::Available},
			{TEXT("module.AssetRegistry"), EHyperAIStudioDomainPrerequisiteState::Available}};
		Binding.Prerequisites.Fingerprint =
			FHyperAIStudioDomainAdapterRegistry::ComputePrerequisiteFingerprint(Binding.Prerequisites);
		Binding.Admission.PackId = Binding.PackId;
		Binding.Admission.Revision = 1;
		Binding.Admission.Fingerprint =
			FHyperAIStudioDomainAdapterRegistry::ComputeAdmissionFingerprint(Binding.Admission);

		FHyperAIStudioTypedArtifactContract Contract;
		Contract.Binding = Binding;
		Contract.ArtifactTypeId = Payload->GetTypeId();
		Contract.ArtifactSchemaFingerprint = Payload->GetSchemaFingerprint();
		Contract.ArtifactSemanticFingerprint = Payload->GetSemanticFingerprint();
		Contract.EffectTarget = Payload->Domain + TEXT(":") + Payload->BaseRevision;
		Contract.DeadlineMs = 1000;
		Contract.MaxNativeOperations = 8;
		Contract.MaxGameThreadMs = 200;
		Contract.MaxOutputBytes = 8192;
		Contract.MaxResultBytes = 256;
		Contract.StageLifetimeMs = FHyperAIStudioNetworkingContracts::StageLifetimeMs;
		Contract.bCompileOnce = true;
		Contract.bSaveOnce = true;
		Contract.bValidateOnce = true;
		Contract.bVerifyFreshOnce = true;
		return FHyperAIStudioTypedArtifactExecutor::Prepare(Contract, OutPrepared, OutError);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioNetworkingManifestTest,
	"HyperAIStudio.NativeTools.NetworkingGameFramework.ManifestAndCoverage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioNetworkingManifestTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("Exact pack id"), FString(FHyperAIStudioNetworkingContracts::PackId),
		FString(TEXT("networking_game_framework")));
	TestEqual(TEXT("Exact source cohort"), FString(FHyperAIStudioNetworkingContracts::AtomicCohortId),
		FString(TEXT("cohort.source.hyperaistudionetworkingtoolset.v1")));
	const auto& Manifest = FHyperAIStudioNetworkingContracts::GetManifest();
	TestEqual(TEXT("Exactly six atomic callables"), Manifest.Num(), 6);
	const TArray<FString> ExactNames = {TEXT("hyper_network_inspect"),
		TEXT("hyper_network_apply_plan"), TEXT("hyper_network_validate"),
		TEXT("hyper_game_framework_inspect"), TEXT("hyper_game_framework_apply_plan"),
		TEXT("hyper_game_framework_validate")};
	for (int32 Index = 0; Index < Manifest.Num(); ++Index)
	{
		TestEqual(TEXT("Exact callable order/name"), Manifest[Index].Name, ExactNames[Index]);
		TestEqual(TEXT("Single owner Toolset"), Manifest[Index].QualifiedToolset,
			FString(TEXT("HyperAIStudioNetworking.HyperAIStudioNetworkingToolset")));
		const UFunction* Function = UHyperAIStudioNetworkingToolset::StaticClass()->FindFunctionByName(
			FName(*Manifest[Index].Name));
		TestNotNull(TEXT("Manifest function is reflected"), Function);
		if (Function) TestTrue(TEXT("Manifest function is AICallable"),
			Function->HasMetaData(TEXT("AICallable")));
	}
	const auto& Variants = FHyperAIStudioNetworkingFacade::GetVariantDescriptors();
	TestEqual(TEXT("Exact coverage matrix"), Variants.Num(), 9);
	TestEqual(TEXT("Four network variants"),
		Variants.FilterByPredicate([](const auto& Value)
		{
			return Value.Domain == TEXT("network");
		}).Num(), 4);
	TestEqual(TEXT("Five Game Framework variants"),
		Variants.FilterByPredicate([](const auto& Value)
		{
			return Value.Domain == TEXT("game_framework");
		}).Num(), 5);
	TestFalse(TEXT("No variant claims an executable mutator before the async host"),
		Variants.ContainsByPredicate([](const auto& Value)
		{
			return Value.bApplyBackendExecutable;
		}));
	TestTrue(TEXT("Network defaults are inspect+plan, staged-only"),
		Variants[0].Domain == TEXT("network") && Variants[0].bInspectImplemented
		&& Variants[0].bPlanSchemaImplemented && !Variants[0].bApplyBackendExecutable);
	TestTrue(TEXT("Authoritative RPC graph inspection declares BlueprintGraph prerequisite"),
		Variants[1].RequiredModules.Contains(TEXT("BlueprintGraph")));
	TestTrue(TEXT("Ordinary CRUD remains delegated to Epic"),
		Variants[0].DelegatedEpicCases.Contains(TEXT("ordinary_actor_and_property_crud")));
	TestTrue(TEXT("Broad synchronous Asset Registry inventory is explicitly unsupported"),
		Variants[3].UnsupportedCases.Contains(TEXT("synchronous_broad_asset_inventory")));
	TestFalse(TEXT("Package data never overclaims cached Blueprint lineage"),
		Variants[3].SupportedCases.Contains(TEXT("exact_target_cached_blueprint_class_lineage")));
	TestTrue(TEXT("Cached Blueprint identity/revision requires later async inventory"),
		Variants[3].UnsupportedCases.Contains(
			TEXT("synchronous_cached_blueprint_identity_or_lineage"))
		&& Variants[3].UnsupportedCases.Contains(
			TEXT("cached_revision_without_async_inventory")));
	TestTrue(TEXT("Framework cached lineage is not claimed synchronously"),
		Variants[4].UnsupportedCases.Contains(
			TEXT("synchronous_cached_framework_identity_or_lineage")));
	TestTrue(TEXT("WorldSettings override has a typed plan schema but remains staged-only"),
		Variants[6].Variant == TEXT("world_runtime")
		&& Variants[6].bPlanSchemaImplemented && !Variants[6].bApplyBackendExecutable);
	TestTrue(TEXT("Online observation prohibits provider effects"),
		Variants.Last().UnsupportedCases.Contains(TEXT("login_cloud_or_session_calls"))
		&& Variants.Last().UnsupportedCases.Contains(TEXT("oss_module_loading")));
	TestFalse(TEXT("Source candidate cannot production-register"),
		FHyperAIStudioNetworkingContracts::IsRegistrationAllowed(false));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioNetworkingClosedSchemaTest,
	"HyperAIStudio.NativeTools.NetworkingGameFramework.ClosedSchemas",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioNetworkingClosedSchemaTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Networking::Tests;
	FHyperAIStudioNetworkingBackendOperation Backend;
	FString Code;
	FString Error;
	FHyperAINetworkPlanOperation Network;
	Network.TargetPath = TEXT("/Game/Tests/BP_Network.BP_Network");
	Network.ExpectedRevision = Hash(TEXT("target"));
	Network.Type = TEXT("class.set_replication");
	Network.Replicates = 1;
	TestTrue(TEXT("Typed actor replication is accepted"),
		FHyperAIStudioNetworkingContracts::ValidateNetworkOperationShape(
			Network, Backend, Code, Error));
	TestEqual(TEXT("Kind is sealed"), static_cast<uint8>(Backend.Kind),
		static_cast<uint8>(EHyperAIStudioNetworkingOperationKind::NetworkClassSetReplication));

	Network = {};
	Network.TargetPath = TEXT("/Game/Tests/BP_Network.BP_Network");
	Network.ExpectedRevision = Hash(TEXT("target"));
	Network.Type = TEXT("class.set_update_policy");
	Network.NetUpdateFrequency = 30.0;
	Network.MinNetUpdateFrequency = 10.0;
	Network.NetPriority = 1.5;
	TestTrue(TEXT("Finite update policy is accepted"),
		FHyperAIStudioNetworkingContracts::ValidateNetworkOperationShape(
			Network, Backend, Code, Error));
	Network.NetPriority = std::numeric_limits<double>::quiet_NaN();
	TestFalse(TEXT("NaN policy fails closed"),
		FHyperAIStudioNetworkingContracts::ValidateNetworkOperationShape(
			Network, Backend, Code, Error));

	Network = {};
	Network.TargetPath = TEXT("/Game/Tests/BP_Network.BP_Network");
	Network.ExpectedRevision = Hash(TEXT("target"));
	Network.Type = TEXT("rpc.configure_existing");
	Network.MemberName = TEXT("ServerUse");
	Network.RpcMode = TEXT("server");
	Network.Reliable = 1;
	Network.WithValidation = 1;
	TestTrue(TEXT("Existing server RPC configuration is accepted"),
		FHyperAIStudioNetworkingContracts::ValidateNetworkOperationShape(
			Network, Backend, Code, Error));
	Network.RpcMode = TEXT("client");
	TestFalse(TEXT("Validation on a client RPC fails closed"),
		FHyperAIStudioNetworkingContracts::ValidateNetworkOperationShape(
			Network, Backend, Code, Error));
	Network.Type = TEXT("console.execute");
	TestFalse(TEXT("Console command is outside the closed vocabulary"),
		FHyperAIStudioNetworkingContracts::ValidateNetworkOperationShape(
			Network, Backend, Code, Error));
	Network = {};
	Network.TargetPath = TEXT("/Game/Tests/BP_Network.BP_Network");
	Network.ExpectedRevision = Hash(TEXT("target"));
	Network.Type = TEXT("property.set_replication");
	Network.MemberName = TEXT("SquadState");
	Network.Replicates = 1;
	Network.ReplicationCondition = TEXT("net_group");
	TestTrue(TEXT("UE 5.8 NetGroup replication condition is typed"),
		FHyperAIStudioNetworkingContracts::ValidateNetworkOperationShape(
			Network, Backend, Code, Error));
	Network = {};
	Network.TargetPath = TEXT("/Game/Tests/BP_Network.BP_Network");
	Network.ExpectedRevision = Hash(TEXT("target"));
	Network.Type = TEXT("class.set_relevancy");
	Network.AlwaysRelevant = 0;
	Network.OnlyRelevantToOwner = 1;
	Network.UseOwnerRelevancy = 0;
	TestTrue(TEXT("Closed relevancy policy is accepted"),
		FHyperAIStudioNetworkingContracts::ValidateNetworkOperationShape(
			Network, Backend, Code, Error));
	Network = {};
	Network.TargetPath = TEXT("/Game/Tests/BP_Network.BP_Network");
	Network.ExpectedRevision = Hash(TEXT("target"));
	Network.Type = TEXT("class.set_dormancy");
	Network.Dormancy = TEXT("dormant_all");
	TestTrue(TEXT("Closed dormancy policy is accepted"),
		FHyperAIStudioNetworkingContracts::ValidateNetworkOperationShape(
			Network, Backend, Code, Error));
	Network = {};
	Network.TargetPath = TEXT("/Game/Tests/BP_Network.BP_Network");
	Network.ExpectedRevision = Hash(TEXT("target"));
	Network.Type = TEXT("component.set_replicated");
	Network.MemberName = TEXT("InventoryComponent");
	Network.Replicates = 1;
	TestTrue(TEXT("Named default-component replication is accepted"),
		FHyperAIStudioNetworkingContracts::ValidateNetworkOperationShape(
			Network, Backend, Code, Error));

	FHyperAIGameFrameworkPlanOperation Framework;
	Framework.Type = TEXT("framework.create_class");
	Framework.TargetPath = TEXT("/Game/Tests/BP_Mode.BP_Mode");
	Framework.Role = TEXT("game_mode");
	Framework.ParentClassPath = TEXT("/Script/Engine.GameModeBase");
	TestTrue(TEXT("Typed Framework class lifecycle create is accepted"),
		FHyperAIStudioNetworkingContracts::ValidateGameFrameworkOperationShape(
			Framework, Backend, Code, Error));
	FHyperAIGameFrameworkPlanOperation NonPrimaryCreate = Framework;
	NonPrimaryCreate.TargetPath = TEXT("/Game/Tests/BP_Mode.DifferentObjectName");
	TestFalse(TEXT("Framework create absence requires a package-primary object name"),
		FHyperAIStudioNetworkingContracts::ValidateGameFrameworkOperationShape(
			NonPrimaryCreate, Backend, Code, Error));
	TestEqual(TEXT("Framework non-primary create rejection is stable"), Code,
		FString(TEXT("invalid_framework_create_shape")));
	Framework.Type = TEXT("ini.set_value");
	TestFalse(TEXT("Raw INI mutation is outside the closed vocabulary"),
		FHyperAIStudioNetworkingContracts::ValidateGameFrameworkOperationShape(
			Framework, Backend, Code, Error));

	Framework = {};
	Framework.Type = TEXT("project.set_defaults");
	Framework.TargetPath = TEXT("project:game_maps_settings");
	Framework.ExpectedRevision = Hash(TEXT("settings"));
	Framework.GameModeClassPath = TEXT("/Script/Engine.GameModeBase");
	TestTrue(TEXT("Closed project default operation is accepted"),
		FHyperAIStudioNetworkingContracts::ValidateGameFrameworkOperationShape(
			Framework, Backend, Code, Error));
	Framework = {};
	Framework.Type = TEXT("world.set_game_mode_override");
	Framework.TargetPath = TEXT("/Game/Maps/TestMap.TestMap");
	Framework.ExpectedRevision = Hash(TEXT("world"));
	Framework.GameModeClassPath = TEXT("/Script/Engine.GameModeBase");
	TestTrue(TEXT("Closed world override operation is accepted"),
		FHyperAIStudioNetworkingContracts::ValidateGameFrameworkOperationShape(
			Framework, Backend, Code, Error));
	Framework = {};
	Framework.Type = TEXT("game_mode.set_class_defaults");
	Framework.TargetPath = TEXT("/Game/Tests/BP_Mode.BP_Mode");
	Framework.ExpectedRevision = Hash(TEXT("mode"));
	Framework.GameStateClassPath = TEXT("/Script/Engine.GameStateBase");
	TestTrue(TEXT("Closed GameMode defaults operation is accepted"),
		FHyperAIStudioNetworkingContracts::ValidateGameFrameworkOperationShape(
			Framework, Backend, Code, Error));
	Framework = {};
	Framework.Type = TEXT("game_session.set_limits");
	Framework.TargetPath = TEXT("/Game/Tests/BP_Session.BP_Session");
	Framework.ExpectedRevision = Hash(TEXT("session"));
	Framework.MaxPlayers = 32;
	Framework.MaxSpectators = 4;
	Framework.MaxSplitscreensPerConnection = 2;
	Framework.RequiresPushToTalk = 1;
	TestTrue(TEXT("Closed GameSession limits operation is accepted"),
		FHyperAIStudioNetworkingContracts::ValidateGameFrameworkOperationShape(
			Framework, Backend, Code, Error));

	FHyperAINetworkInspectRequest BadInspect;
	BadInspect.Projection = TEXT("raw_reflection");
	TestEqual(TEXT("Inspect rejects open-ended projections before capture"),
		UHyperAIStudioNetworkingToolset::hyper_network_inspect(BadInspect).Status,
		FString(TEXT("invalid_request_bounds")));
	BadInspect = {};
	BadInspect.Cursor = TEXT("offset:0");
	TestEqual(TEXT("Inspect rejects unbound offset cursors"),
		UHyperAIStudioNetworkingToolset::hyper_network_inspect(BadInspect).Status,
		FString(TEXT("invalid_cursor")));
	FHyperAINetworkValidateRequest BadValidate;
	BadValidate.ExpectedRevision = TEXT("not-a-hash");
	TestEqual(TEXT("Validate rejects malformed CAS"),
		UHyperAIStudioNetworkingToolset::hyper_network_validate(BadValidate).Status,
		FString(TEXT("invalid_request_bounds")));
	FHyperAINetworkingApplyPlanRequest EmptyNetworkPlan;
	TestEqual(TEXT("Empty network plan fails closed"),
		UHyperAIStudioNetworkingToolset::hyper_network_apply_plan(EmptyNetworkPlan).Status,
		FString(TEXT("invalid_request_bounds")));
	FHyperAIGameFrameworkApplyPlanRequest EmptyFrameworkPlan;
	TestEqual(TEXT("Empty Framework plan fails closed"),
		UHyperAIStudioNetworkingToolset::hyper_game_framework_apply_plan(EmptyFrameworkPlan).Status,
		FString(TEXT("invalid_request_bounds")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioNetworkingRevisionAndValidationTest,
	"HyperAIStudio.NativeTools.NetworkingGameFramework.RevisionAndValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioNetworkingRevisionAndValidationTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Networking::Tests;
	FHyperAIStudioNetworkValueSnapshot A;
	A.ScopeFingerprint = Hash(TEXT("scope"));
	FHyperAINetworkRecord First;
	First.Kind = TEXT("class_default");
	First.StableId = TEXT("network.class:/Game/A.A");
	First.TargetPath = TEXT("/Game/A.A");
	First.bLoaded = true;
	First.bDetailsComplete = true;
	First.bRevisionComplete = true;
	First.bReplicates = true;
	First.Dormancy = TEXT("awake");
	First.NetUpdateFrequency = 30.0;
	First.MinNetUpdateFrequency = 10.0;
	First.NetPriority = 1.0;
	FHyperAINetworkRecord Second = First;
	Second.StableId = TEXT("network.class:/Game/B.B");
	Second.TargetPath = TEXT("/Game/B.B");
	A.Records = {Second, First};
	const FString AHash = FHyperAIStudioNetworkingContracts::ComputeNetworkSnapshotRevision(A);
	FHyperAIStudioNetworkValueSnapshot B;
	B.ScopeFingerprint = Hash(TEXT("scope"));
	B.Records = {First, Second};
	const FString BHash = FHyperAIStudioNetworkingContracts::ComputeNetworkSnapshotRevision(B);
	TestEqual(TEXT("Revision is stable under input ordering"), AHash, BHash);
	B.Records[0].NetPriority = 2.0;
	const FString Changed = FHyperAIStudioNetworkingContracts::ComputeNetworkSnapshotRevision(B);
	TestNotEqual(TEXT("Semantic state changes revision"), Changed, BHash);
	FHyperAIStudioNetworkValueSnapshot Duplicate;
	Duplicate.ScopeFingerprint = Hash(TEXT("duplicate"));
	Duplicate.Records = {First, First};
	FHyperAIStudioNetworkingContracts::ComputeNetworkSnapshotRevision(Duplicate);
	TestFalse(TEXT("Duplicate stable identities fail CAS completeness closed"),
		Duplicate.bComplete);

	FHyperAIStudioNetworkValueSnapshot NonFinite;
	NonFinite.ScopeFingerprint = Hash(TEXT("nonfinite"));
	First.NetPriority = std::numeric_limits<double>::quiet_NaN();
	NonFinite.Records.Add(First);
	FHyperAIStudioNetworkingContracts::ComputeNetworkSnapshotRevision(NonFinite);
	TestFalse(TEXT("Non-finite snapshot cannot be CAS-complete"), NonFinite.bComplete);
	bool bTruncated = false;
	const auto NetworkIssues = FHyperAIStudioNetworkingContracts::ValidateNetworkSnapshot(
		NonFinite, false, false, 16, bTruncated);
	TestTrue(TEXT("Independent validator reports NaN/Inf"),
		NetworkIssues.ContainsByPredicate([](const auto& Issue)
		{
			return Issue.Code == TEXT("non_finite_network_numeric");
		}));

	FHyperAIStudioGameFrameworkValueSnapshot Framework;
	Framework.ScopeFingerprint = Hash(TEXT("framework"));
	FHyperAIGameFrameworkRecord Settings;
	Settings.Kind = TEXT("project_settings");
	Settings.StableId = TEXT("project:game_maps_settings");
	Settings.TargetPath = Settings.StableId;
	Settings.bRevisionComplete = true;
	Settings.bLoaded = true;
	Settings.bDetailsComplete = true;
	Framework.Records.Add(Settings);
	FHyperAIStudioNetworkingContracts::ComputeGameFrameworkSnapshotRevision(Framework);
	bTruncated = false;
	const auto FrameworkIssues =
		FHyperAIStudioNetworkingContracts::ValidateGameFrameworkSnapshot(
			Framework, false, false, 16, bTruncated);
	TestTrue(TEXT("Missing project GameMode is independently detected"),
		FrameworkIssues.ContainsByPredicate([](const auto& Issue)
		{
			return Issue.Code == TEXT("missing_global_game_mode");
		}));
	TestTrue(TEXT("Missing GameInstance is independently detected"),
		FrameworkIssues.ContainsByPredicate([](const auto& Issue)
		{
			return Issue.Code == TEXT("missing_game_instance_class");
		}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioNetworkingP1RegressionTest,
	"HyperAIStudio.NativeTools.NetworkingGameFramework.P1NoLoadIdentityAndCAS",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioNetworkingP1RegressionTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Networking::Tests;
	TestTrue(TEXT("Traversal remains allowed immediately below the object cap"),
		FHyperAIStudioNetworkingContracts::IsCaptureTraversalAllowed(
			FHyperAIStudioNetworkingContracts::MaxObjectsScanned - 1, 0, 10.0, 11.0));
	TestFalse(TEXT("8192-object cap stops traversal fail-closed"),
		FHyperAIStudioNetworkingContracts::IsCaptureTraversalAllowed(
			FHyperAIStudioNetworkingContracts::MaxObjectsScanned, 0, 10.0, 11.0));
	TestFalse(TEXT("A count beyond 8192 can never resume traversal"),
		FHyperAIStudioNetworkingContracts::IsCaptureTraversalAllowed(
			FHyperAIStudioNetworkingContracts::MaxObjectsScanned + 1, 0, 10.0, 11.0));
	TestFalse(TEXT("Expired deadline stops traversal fail-closed"),
		FHyperAIStudioNetworkingContracts::IsCaptureTraversalAllowed(0, 0, 11.0, 11.0));

	FString AssetPath;
	TestTrue(TEXT("Unloaded generated-class path resolves to its exact Blueprint asset"),
		FHyperAIStudioNetworkingContracts::ResolveGeneratedClassAssetPath(
			TEXT("/Game/Framework/BP_Child.BP_Child_C"), AssetPath));
	TestEqual(TEXT("Exact _C suffix is removed only from the object name"), AssetPath,
		FString(TEXT("/Game/Framework/BP_Child.BP_Child")));
	TestTrue(TEXT("A cached BP parent-chain hop uses the same exact resolver"),
		FHyperAIStudioNetworkingContracts::ResolveGeneratedClassAssetPath(
			TEXT("/Game/Framework/BP_Parent.BP_Parent_C"), AssetPath));
	TestFalse(TEXT("A misleading GameMode-like asset name is never a lineage proof"),
		FHyperAIStudioNetworkingContracts::ResolveGeneratedClassAssetPath(
			TEXT("/Game/Framework/FakeGameModeAsset.FakeGameModeAsset"), AssetPath));
	TestFalse(TEXT("Oversized export-text metadata is rejected before path conversion"),
		FHyperAIStudioNetworkingContracts::ResolveGeneratedClassAssetPath(
			FString(TEXT("Blueprint'")) + FString::ChrN(
				FHyperAIStudioNetworkingContracts::MaxPathCharacters + 1, TEXT('A')) + TEXT("'"),
			AssetPath));
	FHyperAIStudioNetworkValueSnapshot PathBoundSnapshot;
	FString PathBoundStatus;
	FString PathBoundDiagnostic;
	TestFalse(TEXT("Oversized targets fail before the capture copies or sorts them"),
		FHyperAIStudioNetworkingFacade::CaptureNetwork(
			{FString::ChrN(FHyperAIStudioNetworkingContracts::MaxPathCharacters + 1, TEXT('A'))},
			TEXT("all"), false, false, PathBoundSnapshot,
			PathBoundStatus, PathBoundDiagnostic, 100));
	TestEqual(TEXT("Oversized Network target has a stable rejection"), PathBoundStatus,
		FString(TEXT("invalid_target_path")));
	FHyperAIStudioNetworkValueSnapshot AliasSnapshot;
	FString AliasStatus;
	FString AliasDiagnostic;
	TestFalse(TEXT("Blueprint asset and GeneratedClass aliases are deduplicated fail-closed"),
		FHyperAIStudioNetworkingFacade::CaptureNetwork(
			TArray<FString>{TEXT("/Game/Framework/BP_Child.BP_Child"),
				TEXT("/Game/Framework/BP_Child.BP_Child_C")},
			TEXT("class_defaults"), false, false, AliasSnapshot,
			AliasStatus, AliasDiagnostic, 100));
	TestEqual(TEXT("Alias dedupe has an exact status"), AliasStatus,
		FString(TEXT("duplicate_semantic_target")));
	TestEqual(TEXT("Character is classified by loaded IsChildOf, not by path text"),
		FHyperAIStudioNetworkingContracts::ClassifyLoadedFrameworkRole(ACharacter::StaticClass()),
		FString(TEXT("pawn")));
	TestEqual(TEXT("A different native Pawn subclass is classified structurally"),
		FHyperAIStudioNetworkingContracts::ClassifyLoadedFrameworkRole(ADefaultPawn::StaticClass()),
		FString(TEXT("pawn")));
	TestEqual(TEXT("Native GameInstance is classified structurally"),
		FHyperAIStudioNetworkingContracts::ClassifyLoadedFrameworkRole(UGameInstance::StaticClass()),
		FString(TEXT("game_instance")));
	TestTrue(TEXT("UpToDate Blueprint may be edited"),
		FHyperAIStudioNetworkingContracts::IsBlueprintCompileStatusEditable(TEXT("up_to_date")));
	TestTrue(TEXT("UpToDateWithWarnings Blueprint may be edited"),
		FHyperAIStudioNetworkingContracts::IsBlueprintCompileStatusEditable(
			TEXT("up_to_date_with_warnings")));
	TestFalse(TEXT("Dirty Blueprint blocks edit planning"),
		FHyperAIStudioNetworkingContracts::IsBlueprintCompileStatusEditable(TEXT("dirty")));
	TestFalse(TEXT("Error Blueprint blocks edit planning"),
		FHyperAIStudioNetworkingContracts::IsBlueprintCompileStatusEditable(TEXT("error")));
	TestFalse(TEXT("BeingCreated Blueprint blocks edit planning"),
		FHyperAIStudioNetworkingContracts::IsBlueprintCompileStatusEditable(TEXT("being_created")));
	TestFalse(TEXT("A native class cannot masquerade as a current Blueprint GeneratedClass"),
		FHyperAIStudioNetworkingContracts::IsCurrentGeneratedClass(
			nullptr, ACharacter::StaticClass()));

	FHyperAIStudioNetworkValueSnapshot BroadInventory;
	FString BroadStatus;
	FString BroadDiagnostic;
	TestTrue(TEXT("Broad on-disk request returns a bounded fail-closed snapshot"),
		FHyperAIStudioNetworkingFacade::CaptureNetwork(TArray<FString>{},
			TEXT("on_disk_assets"), false,
			true, BroadInventory, BroadStatus, BroadDiagnostic, 100));
	TestFalse(TEXT("Broad synchronous inventory never claims CAS completeness"),
		BroadInventory.bComplete);
	TestTrue(TEXT("Broad inventory explicitly requires an async generation cache"),
		BroadInventory.CaptureIssues.ContainsByPredicate([](const auto& Issue)
		{
			return Issue.Code == TEXT("broad_inventory_requires_async_cache");
		}));

	FHyperAIStudioNetworkValueSnapshot CompileA;
	CompileA.ScopeFingerprint = Hash(TEXT("compile-state"));
	FHyperAINetworkRecord ClassRecord;
	ClassRecord.Kind = TEXT("class_default");
	ClassRecord.StableId = TEXT("network.class:/Game/BP_Current.BP_Current");
	ClassRecord.TargetPath = TEXT("/Game/BP_Current.BP_Current");
	ClassRecord.AssetPath = ClassRecord.TargetPath;
	ClassRecord.ClassPath = TEXT("/Game/BP_Current.BP_Current_C");
	ClassRecord.BlueprintCompileStatus = TEXT("up_to_date");
	ClassRecord.bGeneratedClassCurrent = true;
	ClassRecord.bRevisionComplete = true;
	ClassRecord.NetUpdateFrequency = 30.0;
	ClassRecord.MinNetUpdateFrequency = 10.0;
	ClassRecord.NetPriority = 1.0;
	CompileA.Records.Add(ClassRecord);
	const FString CurrentRevision =
		FHyperAIStudioNetworkingContracts::ComputeNetworkSnapshotRevision(CompileA);
	FHyperAIStudioNetworkValueSnapshot CompileB;
	CompileB.ScopeFingerprint = Hash(TEXT("compile-state"));
	ClassRecord.BlueprintCompileStatus = TEXT("dirty");
	CompileB.Records.Add(ClassRecord);
	const FString DirtyRevision =
		FHyperAIStudioNetworkingContracts::ComputeNetworkSnapshotRevision(CompileB);
	TestNotEqual(TEXT("Blueprint compile state participates in canonical CAS"),
		CurrentRevision, DirtyRevision);
	bool bCompileIssuesTruncated = false;
	const auto DirtyCompileIssues =
		FHyperAIStudioNetworkingContracts::ValidateNetworkSnapshot(
			CompileB, false, false, 16, bCompileIssuesTruncated);
	TestTrue(TEXT("Dirty/current-class state is independently edit-blocking"),
		DirtyCompileIssues.ContainsByPredicate([](const auto& Issue)
		{
			return Issue.Code == TEXT("blueprint_not_current_or_compiled");
		}));

	FHyperAIStudioGameFrameworkValueSnapshot WorldA;
	WorldA.ScopeFingerprint = Hash(TEXT("world-settings"));
	FHyperAIGameFrameworkRecord WorldSettings;
	WorldSettings.Kind = TEXT("world_settings");
	WorldSettings.Role = TEXT("world");
	WorldSettings.StableId = TEXT("framework.world_settings:/Game/Maps/Test.Test");
	WorldSettings.TargetPath = TEXT("/Game/Maps/Test.Test");
	WorldSettings.WorldPath = WorldSettings.TargetPath;
	WorldSettings.WorldSettingsPath =
		TEXT("/Game/Maps/Test.Test:PersistentLevel.WorldSettings_1");
	WorldSettings.PackageName = TEXT("/Game/Maps/Test");
	WorldSettings.GameModeClassPath = TEXT("/Script/Engine.GameModeBase");
	WorldSettings.bLoaded = true;
	WorldSettings.bDetailsComplete = true;
	WorldSettings.bRevisionComplete = true;
	WorldA.Records.Add(WorldSettings);
	const FString WorldRevision =
		FHyperAIStudioNetworkingContracts::ComputeGameFrameworkSnapshotRevision(WorldA);
	FHyperAIStudioGameFrameworkValueSnapshot WorldB;
	WorldB.ScopeFingerprint = Hash(TEXT("world-settings"));
	WorldSettings.GameModeClassPath = TEXT("/Script/Engine.GameMode");
	WorldB.Records.Add(WorldSettings);
	const FString OverrideRevision =
		FHyperAIStudioNetworkingContracts::ComputeGameFrameworkSnapshotRevision(WorldB);
	TestNotEqual(TEXT("WorldSettings DefaultGameMode override changes exact record CAS"),
		WorldRevision, OverrideRevision);
	WorldSettings.bPackageDirty = true;
	FHyperAIStudioGameFrameworkValueSnapshot WorldDirty;
	WorldDirty.ScopeFingerprint = Hash(TEXT("world-settings"));
	WorldDirty.Records.Add(WorldSettings);
	TestNotEqual(TEXT("World package dirty state participates in exact CAS"),
		OverrideRevision,
		FHyperAIStudioNetworkingContracts::ComputeGameFrameworkSnapshotRevision(WorldDirty));
	FHyperAIStudioGameFrameworkValueSnapshot DuplicateFramework;
	DuplicateFramework.ScopeFingerprint = Hash(TEXT("framework-dedupe"));
	DuplicateFramework.Records = {WorldSettings, WorldSettings};
	FHyperAIStudioNetworkingContracts::ComputeGameFrameworkSnapshotRevision(DuplicateFramework);
	TestFalse(TEXT("Duplicate Generated/Skeleton/REINST projections cannot remain CAS-complete"),
		DuplicateFramework.bComplete);

	FHyperAIStudioNetworkValueSnapshot InvalidSelectorSnapshot;
	InvalidSelectorSnapshot.ScopeFingerprint = Hash(TEXT("selector-signature"));
	FHyperAINetworkElementView ForgedRpc;
	ForgedRpc.Kind = TEXT("rpc");
	ForgedRpc.Name = TEXT("ServerForged");
	ForgedRpc.StableId = TEXT("rpc:/Game/BP_Current.BP_Current:ServerForged");
	ForgedRpc.RpcMode = TEXT("server");
	ForgedRpc.DeclarationSource = TEXT("function_graph");
	ForgedRpc.OwnerClassPath = ClassRecord.ClassPath;
	ForgedRpc.bAuthoritativeDeclaration = true;
	ForgedRpc.bSignatureValid = false;
	ForgedRpc.bReplicated = true;
	FHyperAINetworkElementView Property;
	Property.Kind = TEXT("property");
	Property.Name = TEXT("Health");
	Property.StableId = TEXT("property:/Game/BP_Current.BP_Current:Health");
	Property.RepNotifyFunction = TEXT("OnRep_Health");
	Property.DeclarationSource = TEXT("new_variable");
	Property.OwnerClassPath = ClassRecord.ClassPath;
	Property.bAuthoritativeDeclaration = true;
	Property.bSignatureValid = true;
	Property.bReplicated = true;
	FHyperAINetworkElementView InvalidRepNotify;
	InvalidRepNotify.Kind = TEXT("rep_notify_function");
	InvalidRepNotify.Name = TEXT("OnRep_Health");
	InvalidRepNotify.StableId =
		TEXT("rep_notify:/Game/BP_Current.BP_Current:OnRep_Health");
	InvalidRepNotify.DeclarationSource = TEXT("function_graph");
	InvalidRepNotify.OwnerClassPath = TEXT("/Game/BP_Parent.BP_Parent_C");
	InvalidRepNotify.bAuthoritativeDeclaration = false;
	InvalidRepNotify.bSignatureValid = false;
	InvalidRepNotify.bReplicated = true;
	ClassRecord.BlueprintCompileStatus = TEXT("up_to_date");
	ClassRecord.Elements = {ForgedRpc, Property, InvalidRepNotify};
	TestTrue(TEXT("Owner-bound NewVariables property is an authoritative selector"),
		FHyperAIStudioNetworkingContracts::HasAuthoritativeDeclaredSelector(
			ClassRecord, TEXT("property"), TEXT("property"), TEXT("Health"),
			TEXT("new_variable"), true));
	TestFalse(TEXT("A FunctionGraph cannot masquerade as an RPC custom-event selector"),
		FHyperAIStudioNetworkingContracts::HasAuthoritativeDeclaredSelector(
			ClassRecord, TEXT("function"), TEXT("rpc"), TEXT("ServerForged"),
			TEXT("custom_event"), true));
	TestFalse(TEXT("Inherited/owner-mismatched RepNotify is rejected"),
		FHyperAIStudioNetworkingContracts::HasAuthoritativeDeclaredSelector(
			ClassRecord, TEXT("rep_notify_function"), TEXT("rep_notify_function"),
			TEXT("OnRep_Health"), TEXT("function_graph"), true));
	InvalidSelectorSnapshot.Records.Add(ClassRecord);
	FHyperAIStudioNetworkingContracts::ComputeNetworkSnapshotRevision(InvalidSelectorSnapshot);
	bool bTruncated = false;
	const auto SelectorIssues = FHyperAIStudioNetworkingContracts::ValidateNetworkSnapshot(
		InvalidSelectorSnapshot, false, false, 16, bTruncated);
	TestTrue(TEXT("Generated/internal or wrong-source RPC selector is rejected"),
		SelectorIssues.ContainsByPredicate([](const auto& Issue)
		{
			return Issue.Code == TEXT("rpc_declaration_or_signature_invalid");
		}));
	TestTrue(TEXT("Inherited/invalid RepNotify signature is not accepted as a selector"),
		SelectorIssues.ContainsByPredicate([](const auto& Issue)
		{
			return Issue.Code == TEXT("missing_rep_notify_function");
		}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioNetworkingAdapterStageTest,
	"HyperAIStudio.NativeTools.NetworkingGameFramework.AdapterAndIdempotentStage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioNetworkingAdapterStageTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Networking::Tests;
	FHyperAIStudioNetworkingStagingService::Reset();
	const auto Payload = MakePayload();
	const auto Detached = Payload->CloneImmutable();
	TestTrue(TEXT("Clone is detached"), &Detached.Get() != &Payload.Get());
	TestEqual(TEXT("Clone preserves semantic fingerprint"), Detached->GetSemanticFingerprint(),
		Payload->GetSemanticFingerprint());
	const int32 DetachedBytes = Detached->GetBoundedByteSize();
	Payload->Operations[0].TargetPath += TEXT("_source_mutation");
	TestEqual(TEXT("Deep clone does not alias nested strings"), Detached->GetBoundedByteSize(),
		DetachedBytes);
	Payload->Operations[0].TargetPath = TEXT("/Game/Tests/BP_Network.BP_Network");

	const FString BeforeKindTamper = Payload->SemanticFingerprint;
	Payload->Operations[0].Kind = EHyperAIStudioNetworkingOperationKind::NetworkClassSetDormancy;
	const FString AfterKindTamper =
		FHyperAIStudioNetworkingContracts::ComputePayloadSemanticFingerprint(
			Payload->Domain, Payload->ToolName, Payload->Operations, Payload->BaseRevision);
	TestNotEqual(TEXT("Operation Kind is part of the semantic seal"),
		BeforeKindTamper, AfterKindTamper);
	Payload->Operations[0].Kind = EHyperAIStudioNetworkingOperationKind::NetworkClassSetReplication;
	FHyperAIStudioNetworkingTypedPayload Oversized;
	Oversized.Domain = TEXT("game_framework");
	Oversized.ToolName = TEXT("hyper_game_framework_apply_plan");
	Oversized.BaseRevision = Hash(TEXT("oversized"));
	FHyperAIStudioNetworkingBackendOperation LargeOperation;
	LargeOperation.Domain = Oversized.Domain;
	LargeOperation.Kind = EHyperAIStudioNetworkingOperationKind::FrameworkGameModeSetClassDefaults;
	LargeOperation.Type = TEXT("game_mode.set_class_defaults");
	LargeOperation.TargetPath = FString::ChrN(1024, TEXT('t'));
	LargeOperation.GameStateClassPath = FString::ChrN(1024, TEXT('a'));
	LargeOperation.PlayerControllerClassPath = FString::ChrN(1024, TEXT('b'));
	LargeOperation.PlayerStateClassPath = FString::ChrN(1024, TEXT('c'));
	LargeOperation.PawnClassPath = FString::ChrN(1024, TEXT('d'));
	LargeOperation.HUDClassPath = FString::ChrN(1024, TEXT('e'));
	LargeOperation.GameSessionClassPath = FString::ChrN(1024, TEXT('f'));
	LargeOperation.SpectatorClassPath = FString::ChrN(1024, TEXT('g'));
	for (int32 Index = 0; Index < FHyperAIStudioNetworkingContracts::MaxOperations; ++Index)
		Oversized.Operations.Add(LargeOperation);
	TestTrue(TEXT("Aggregate payload estimator exceeds the shared 1 MiB bound"),
		Oversized.GetBoundedByteSize() > FHyperAIStudioDomainLimits::MaxRequestBytes);

	FHyperAIStudioDomainDispatchContext Context;
	Context.Binding.PackId = FHyperAIStudioNetworkingContracts::PackId;
	Context.Binding.ToolName = TEXT("hyper_network_apply_plan");
	Context.Binding.VariantId = FHyperAIStudioNetworkingContracts::NetworkMutationVariantId;
	Context.Binding.ExpectedSafety = EHyperAIStudioDomainSafety::Edit;
	Context.Binding.ExpectedAdapterFingerprint =
		FHyperAIStudioNetworkingContracts::GetBaseAdapterDescriptor().AdapterFingerprint;
	Context.Safety = EHyperAIStudioDomainSafety::Edit;
	Context.ActionKind = EHyperAIStudioDomainExecutionActionKind::Apply;
	FHyperAIStudioNetworkingDomainAdapter Adapter;
	const auto Blocked = Adapter.Execute(Context, Payload.Get());
	TestEqual(TEXT("Exact adapter remains staged-only"), Blocked.StatusCode,
		FString(TEXT("staged_backend_required")));
	TestFalse(TEXT("No false result payload"), Blocked.Payload.IsValid());
	Context.Binding.ToolName = TEXT("hyper_network_execute_console");
	TestEqual(TEXT("Unknown tool binding fails exact"),
		Adapter.Execute(Context, Payload.Get()).StatusCode,
		FString(TEXT("typed_binding_mismatch")));

	FHyperAIStudioPreparedTypedArtifact Prepared;
	FString Error;
	TestTrue(TEXT("Shared executor seals typed plan"), PrepareArtifact(Payload, Prepared, Error));
	const auto Forged = MakePayload();
	Forged->Operations[0].Kind = EHyperAIStudioNetworkingOperationKind::NetworkClassSetDormancy;
	Forged->SemanticFingerprint =
		FHyperAIStudioNetworkingContracts::ComputePayloadSemanticFingerprint(
			Forged->Domain, Forged->ToolName, Forged->Operations, Forged->BaseRevision);
	FHyperAIStudioPreparedTypedArtifact ForgedPrepared;
	TestTrue(TEXT("Shared generic seal alone accepts an internally consistent opaque DTO"),
		PrepareArtifact(Forged, ForgedPrepared, Error));
	FHyperAIStudioNetworkingStagedArtifact ForgedArtifact;
	ForgedArtifact.Prepared = ForgedPrepared;
	ForgedArtifact.Payload = Forged;
	ForgedArtifact.CanonicalProjectId = TEXT("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
	ForgedArtifact.OperationId = TEXT("networking-forged-001");
	ForgedArtifact.StageId = FHyperAIStudioNetworkingContracts::ComputeStageId(
		ForgedArtifact.CanonicalProjectId, ForgedArtifact.OperationId,
		ForgedPrepared.PlanHash, ForgedPrepared.EffectFingerprint);
	ForgedArtifact.ExpiresMonotonicMs = static_cast<int64>(FPlatformTime::Seconds() * 1000.0)
		+ FHyperAIStudioNetworkingContracts::StageLifetimeMs;
	bool bForgedReplay = false;
	TestFalse(TEXT("Domain staging rejects a Type-to-Kind discriminant forgery"),
		FHyperAIStudioNetworkingStagingService::Stage(
			ForgedArtifact, bForgedReplay, Error));
	FHyperAIStudioNetworkingStagedArtifact Artifact;
	Artifact.Prepared = Prepared;
	Artifact.Payload = Payload;
	Artifact.CanonicalProjectId = TEXT("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
	Artifact.OperationId = TEXT("networking-stage-001");
	Artifact.StageId = FHyperAIStudioNetworkingContracts::ComputeStageId(
		Artifact.CanonicalProjectId, Artifact.OperationId,
		Prepared.PlanHash, Prepared.EffectFingerprint);
	Artifact.ExpiresMonotonicMs = static_cast<int64>(FPlatformTime::Seconds() * 1000.0)
		+ FHyperAIStudioNetworkingContracts::StageLifetimeMs;
	bool bReplay = false;
	TestTrue(TEXT("First exact stage succeeds"),
		FHyperAIStudioNetworkingStagingService::Stage(Artifact, bReplay, Error));
	TestFalse(TEXT("First stage is not replay"), bReplay);
	TestTrue(TEXT("Exact idempotent replay succeeds"),
		FHyperAIStudioNetworkingStagingService::Stage(Artifact, bReplay, Error));
	TestTrue(TEXT("Replay is reported"), bReplay);
	TestEqual(TEXT("Store remains one entry"),
		FHyperAIStudioNetworkingStagingService::NumStaged(), 1);
	FHyperAIStudioNetworkingStagingService::Reset();
	return true;
}

#endif
