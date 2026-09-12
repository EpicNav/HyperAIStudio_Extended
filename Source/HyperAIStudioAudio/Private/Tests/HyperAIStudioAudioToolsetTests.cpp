// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HyperAIStudioAudioToolset.h"

#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "DocumentTemplates/MetasoundFrontendDocumentVertexTemplate.h"
#include "MetasoundFrontendDocument.h"
#include "MetasoundFrontendLiteral.h"
#include "MetasoundFrontendNodeClassRegistry.h"
#include "Misc/AutomationTest.h"
#include "Misc/AssetRegistryInterface.h"
#include "Misc/PackageName.h"
#include "Sound/DialogueWave.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundMix.h"
#include "Sound/SoundNodeWavePlayer.h"
#include "Sound/SoundWave.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace HyperAIStudio::Audio::Tests
{
	FString Sha(const TCHAR Character)
	{
		return TEXT("sha256:") + FString::ChrN(64, Character);
	}

	FString UniqueAssetPath(const TCHAR* Stem)
	{
		const FString AssetName = FString::Printf(TEXT("%s_%s"), Stem,
			*FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower());
		return FString::Printf(TEXT("/Game/__HyperAIStudioAutomation/%s.%s"),
			*AssetName, *AssetName);
	}

	FHyperAIAudioPlanOperation SoundCueCreate(const FString& Target)
	{
		FHyperAIAudioPlanOperation Operation;
		Operation.Variant = TEXT("sound_cue.create");
		Operation.TargetPath = Target;
		return Operation;
	}

	bool InitializeRegisteredDependency(
		FMetasoundFrontendClass& Dependency,
		const FName DataType = TEXT("Float"),
		const bool bInputNode = true)
	{
		Metasound::Frontend::INodeClassRegistry* Registry =
			Metasound::Frontend::INodeClassRegistry::Get();
		Metasound::Frontend::FNodeRegistryKey Key;
		const bool bResolved = bInputNode
			? Metasound::Frontend::INodeClassRegistry::GetInputNodeRegistryKeyForDataType(
				DataType, EMetasoundFrontendVertexAccessType::Reference, Key)
			: Metasound::Frontend::INodeClassRegistry::GetOutputNodeRegistryKeyForDataType(
				DataType, EMetasoundFrontendVertexAccessType::Reference, Key);
		if (!Registry || !bResolved || !Key.IsValid() || !Registry->IsNodeRegistered(Key)
			|| !Registry->FindFrontendClassFromRegistered(Key, Dependency)) return false;
		// The dependency identity is document-local; the registry key and exact class
		// interface remain the native registration authority.
		Dependency.ID = FGuid::NewGuid();
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIAudioManifestTest,
	"HyperAIStudio.NativeTools.AudioMetaSound.ManifestRegistrationAndSurface",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIAudioManifestTest::RunTest(const FString& Parameters)
{
	const TArray<FHyperAIAudioManifestEntry>& Manifest = FHyperAIStudioAudioContracts::GetManifest();
	TestEqual(TEXT("exact three-tool manifest"), Manifest.Num(), 3);
	TestEqual(TEXT("exact combined pack"), FString(FHyperAIStudioAudioContracts::PackId),
		FString(TEXT("audio_metasound")));
	TestEqual(TEXT("exact source cohort"), FString(FHyperAIStudioAudioContracts::AtomicCohortId),
		FString(TEXT("cohort.source.hyperaistudioaudiotoolset.v1")));

	TSet<FString> ManifestNames;
	for (const FHyperAIAudioManifestEntry& Entry : Manifest)
	{
		ManifestNames.Add(Entry.Name);
		TestEqual(TEXT("single qualified toolset"), Entry.QualifiedToolset,
			FHyperAIStudioAudioContracts::GetQualifiedToolsetName());
	}
	TSet<FString> ReflectedNames;
	for (TFieldIterator<UFunction> It(UHyperAIStudioAudioToolset::StaticClass(),
		EFieldIteratorFlags::ExcludeSuper); It; ++It)
	{
		if (It->HasMetaData(TEXT("AICallable"))) ReflectedNames.Add(It->GetName());
	}
	TestEqual(TEXT("exactly three reflected AICallables"), ReflectedNames.Num(), 3);
	TestEqual(TEXT("manifest/reflection exact set size"), ManifestNames.Num(), ReflectedNames.Num());
	for (const FString& Name : ManifestNames)
	{
		const FString Label = TEXT("reflected: ") + Name;
		TestTrue(*Label, ReflectedNames.Contains(Name));
	}

	TArray<FString> ExactNames = ManifestNames.Array();
	ExactNames.Sort();
	FHyperAIStudioExtensionCohortAdmission Admission;
	TestTrue(TEXT("generated catalog is sole cohort authority"),
		FHyperAIStudioExtensionRuntime::QueryExactGeneratedCohort(
			FHyperAIStudioAudioContracts::PackId,
			FHyperAIStudioAudioContracts::AtomicCohortId,
			ExactNames, Admission));
	TestTrue(TEXT("catalog valid"), Admission.bCatalogValid);
	TestTrue(TEXT("all and only three tools atomic"), Admission.bExactCohortMatch);
	TestTrue(TEXT("pack remains explicit-load optional"), Admission.bOptionalPack);
	TestEqual(TEXT("implementation is source candidate"), Admission.State,
		EHyperAIStudioExtensionAdmissionState::SourceCandidate);
	TestFalse(TEXT("source candidate never production-registers"),
		FHyperAIStudioAudioContracts::IsRegistrationAllowed(false));

	TestNull(TEXT("no client authorization token"),
		FHyperAIAudioApplyPlanRequest::StaticStruct()->FindPropertyByName(TEXT("AuthorizationToken")));
	TestNull(TEXT("no raw file path"),
		FHyperAIAudioPlanOperation::StaticStruct()->FindPropertyByName(TEXT("FilePath")));
	TestNull(TEXT("no script body"),
		FHyperAIAudioPlanOperation::StaticStruct()->FindPropertyByName(TEXT("Script")));
	TestNull(TEXT("no console command"),
		FHyperAIAudioPlanOperation::StaticStruct()->FindPropertyByName(TEXT("ConsoleCommand")));
	TestNull(TEXT("no arbitrary node class"),
		FHyperAIAudioPlanOperation::StaticStruct()->FindPropertyByName(TEXT("NodeClass")));
	TestNull(TEXT("no sample/DSP byte buffer"),
		FHyperAIAudioPlanOperation::StaticStruct()->FindPropertyByName(TEXT("SampleBuffer")));
	const bool bOwned = FHyperAIStudioCapabilityRuntimeIndex::IsOwned(
		UHyperAIStudioAudioToolset::StaticClass(),
		FHyperAIStudioAudioContracts::GetQualifiedToolsetName());
	TestEqual(TEXT("ownership truth matches registration contract when owned"),
		bOwned && FHyperAIStudioAudioContracts::IsRegistrationAllowed(true), bOwned);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIAudioSchemaTest,
	"HyperAIStudio.NativeTools.AudioMetaSound.ClosedOperationSchemas",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIAudioSchemaTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Audio::Tests;
	FString Error;
	FHyperAIAudioBackendOperation Backend;
	const FString CueTarget = UniqueAssetPath(TEXT("SC_Audio"));
	FHyperAIAudioPlanOperation Cue = SoundCueCreate(CueTarget);
	TestTrue(TEXT("SoundCue typed create accepted"),
		FHyperAIStudioAudioContracts::ValidateOperationShape(Cue, Backend, Error));

	FHyperAIAudioPlanOperation Configure;
	Configure.Variant = TEXT("sound_wave.configure");
	Configure.TargetPath = TEXT("/Game/Audio/W_Test.W_Test");
	Configure.ExpectedRevision = Sha(TEXT('a'));
	FHyperAIAudioParameterValue Volume;
	Volume.Name = TEXT("volume");
	Volume.Type = TEXT("float");
	Volume.FloatValue = 0.75;
	Configure.Parameters.Add(Volume);
	TestTrue(TEXT("closed SoundWave configure accepted"),
		FHyperAIStudioAudioContracts::ValidateOperationShape(Configure, Backend, Error));
	Configure.Parameters[0].StringValue = TEXT("hidden second value");
	TestFalse(TEXT("ambiguous typed parameter rejected"),
		FHyperAIStudioAudioContracts::ValidateOperationShape(Configure, Backend, Error));
	Configure.Parameters[0].StringValue.Reset();
	Configure.Parameters[0].Name = TEXT("arbitrary_reflected_property");
	TestFalse(TEXT("arbitrary property-bag parameter rejected"),
		FHyperAIStudioAudioContracts::ValidateOperationShape(Configure, Backend, Error));

	Cue.NodeKind = TEXT("gain");
	TestFalse(TEXT("unused semantic fields rejected"),
		FHyperAIStudioAudioContracts::ValidateOperationShape(Cue, Backend, Error));

	FHyperAIAudioPlanOperation MetaNode;
	MetaNode.Variant = TEXT("metasound.add_node");
	MetaNode.TargetPath = TEXT("/Game/Audio/MS_Test.MS_Test");
	MetaNode.ExpectedRevision = Sha(TEXT('b'));
	MetaNode.NodeId = FGuid::NewGuid();
	MetaNode.NodeKind = TEXT("gain");
	MetaNode.NodeDescriptorVersion = 1;
	TestTrue(TEXT("closed semantic MetaSound node accepted when prerequisite is loaded"),
		FHyperAIStudioAudioContracts::ValidateOperationShape(MetaNode, Backend, Error));
	TestTrue(TEXT("typed descriptor exposes deterministic ports"),
		!Backend.DescriptorInputs.IsEmpty() && !Backend.DescriptorOutputs.IsEmpty());
	MetaNode.NodeKind = TEXT("/Script/Arbitrary.CustomDSPNode");
	TestFalse(TEXT("arbitrary node/class construction rejected"),
		FHyperAIStudioAudioContracts::ValidateOperationShape(MetaNode, Backend, Error));

	FHyperAIAudioPlanOperation Connect;
	Connect.Variant = TEXT("metasound.connect");
	Connect.TargetPath = TEXT("/Game/Audio/MS_Test.MS_Test");
	Connect.ExpectedRevision = Sha(TEXT('c'));
	Connect.FromNodeId = FGuid::NewGuid();
	Connect.FromVertexId = FGuid::NewGuid();
	Connect.ToNodeId = FGuid::NewGuid();
	Connect.ToVertexId = FGuid::NewGuid();
	TestTrue(TEXT("exact persisted MetaSound connection accepted"),
		FHyperAIStudioAudioContracts::ValidateOperationShape(Connect, Backend, Error));
	Connect.ToVertexId.Invalidate();
	TestFalse(TEXT("partial connection identity rejected"),
		FHyperAIStudioAudioContracts::ValidateOperationShape(Connect, Backend, Error));
	Connect.ToPortName = TEXT("audio");
	TestTrue(TEXT("semantic descriptor port alternative accepted"),
		FHyperAIStudioAudioContracts::ValidateOperationShape(Connect, Backend, Error));

	FHyperAIAudioPlanOperation Playback;
	Playback.Variant = TEXT("audio.play");
	Playback.TargetPath = TEXT("/Game/Audio/W_Test.W_Test");
	TestFalse(TEXT("playback prohibited"),
		FHyperAIStudioAudioContracts::ValidateOperationShape(Playback, Backend, Error));
	FHyperAIAudioPlanOperation Capture = Playback;
	Capture.Variant = TEXT("audio_capture.start");
	TestFalse(TEXT("microphone capture start prohibited"),
		FHyperAIStudioAudioContracts::ValidateOperationShape(Capture, Backend, Error));
	FHyperAIAudioPlanOperation RawDsp = Playback;
	RawDsp.Variant = TEXT("synthesis.execute_dsp");
	TestFalse(TEXT("raw DSP execution prohibited"),
		FHyperAIStudioAudioContracts::ValidateOperationShape(RawDsp, Backend, Error));

	FHyperAIAudioPlanOperation DomainWave;
	DomainWave.Variant = TEXT("sound_wave.configure");
	DomainWave.TargetPath = TEXT("/Game/Audio/W_Domain.W_Domain");
	DomainWave.ExpectedRevision = Sha(TEXT('d'));
	FHyperAIAudioParameterValue TooLoud;
	TooLoud.Name = TEXT("volume");
	TooLoud.Type = TEXT("float");
	TooLoud.FloatValue = 4.01;
	DomainWave.Parameters.Add(TooLoud);
	TestFalse(TEXT("variant-specific volume domain enforced"),
		FHyperAIStudioAudioContracts::ValidateOperationShape(DomainWave, Backend, Error));
	FHyperAIAudioPlanOperation Concurrency;
	Concurrency.Variant = TEXT("sound_concurrency.create");
	Concurrency.TargetPath = UniqueAssetPath(TEXT("SCON_VolumeScale"));
	FHyperAIAudioParameterValue VolumeScale;
	VolumeScale.Name = TEXT("volume_scale");
	VolumeScale.Type = TEXT("float");
	VolumeScale.FloatValue = 0.0;
	Concurrency.Parameters = {VolumeScale};
	TestTrue(TEXT("volume_scale lower boundary accepted"),
		FHyperAIStudioAudioContracts::ValidateOperationShape(Concurrency, Backend, Error));
	Concurrency.Parameters[0].FloatValue = 1.0;
	TestTrue(TEXT("volume_scale upper boundary accepted"),
		FHyperAIStudioAudioContracts::ValidateOperationShape(Concurrency, Backend, Error));
	Concurrency.Parameters[0].FloatValue = -UE_DOUBLE_SMALL_NUMBER;
	TestFalse(TEXT("volume_scale below zero rejected"),
		FHyperAIStudioAudioContracts::ValidateOperationShape(Concurrency, Backend, Error));
	Concurrency.Parameters[0].FloatValue = 1.0 + UE_DOUBLE_SMALL_NUMBER;
	TestFalse(TEXT("volume_scale above one rejected"),
		FHyperAIStudioAudioContracts::ValidateOperationShape(Concurrency, Backend, Error));
	FHyperAIAudioPlanOperation Attenuation;
	Attenuation.Variant = TEXT("sound_attenuation.create");
	Attenuation.TargetPath = UniqueAssetPath(TEXT("ATT_Domain"));
	FHyperAIAudioParameterValue Algorithm;
	Algorithm.Name = TEXT("distance_algorithm");
	Algorithm.Type = TEXT("string");
	Algorithm.StringValue = TEXT("arbitrary_algorithm");
	Attenuation.Parameters.Add(Algorithm);
	TestFalse(TEXT("closed enum domain enforced"),
		FHyperAIStudioAudioContracts::ValidateOperationShape(Attenuation, Backend, Error));
	FHyperAIAudioPlanOperation VolumeActor;
	VolumeActor.Variant = TEXT("audio_gameplay_volume.create");
	VolumeActor.TargetPath = UniqueAssetPath(TEXT("AGV_Domain"));
	VolumeActor.Location = FVector(1.0e14, 0.0, 0.0);
	VolumeActor.Extent = FVector(100.0);
	TestFalse(TEXT("UE world location bound enforced before optional adapter"),
		FHyperAIStudioAudioContracts::ValidateOperationShape(VolumeActor, Backend, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIAudioCloneTest,
	"HyperAIStudio.NativeTools.AudioMetaSound.ImmutablePayload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIAudioCloneTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Audio::Tests;
	FHyperAIAudioTypedArtifactPayload Source;
	Source.TypeId = TEXT("hyperai.payload.audio.plan.v1");
	Source.SchemaFingerprint = Sha(TEXT('a'));
	Source.PackId = TEXT("audio_metasound");
	Source.BaseRevision = Sha(TEXT('b'));
	Source.CanonicalOperations = {TEXT("sound-cue-create"), TEXT("metasound-node-add")};
	Source.CanonicalShadowStates = {TEXT("/Game/Audio/A.A=sha256:shadow")};
	const FString Original = Source.GetSemanticFingerprint();
	const TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> Clone =
		Source.CloneImmutable();
	TestTrue(TEXT("clone detached"),
		&Clone.Get() != static_cast<const IHyperAIStudioTypedArtifactPayload*>(&Source));
	TestEqual(TEXT("clone initially exact"), Clone->GetSemanticFingerprint(), Original);
	Source.CanonicalOperations[0] = TEXT("mutated-after-clone");
	Source.CanonicalOperations.Add(TEXT("new-operation"));
	Source.CanonicalShadowStates[0] = TEXT("mutated-shadow");
	TestNotEqual(TEXT("source identity changes"), Source.GetSemanticFingerprint(), Original);
	TestEqual(TEXT("deep clone remains immutable"), Clone->GetSemanticFingerprint(), Original);
	const FHyperAIAudioTypedArtifactPayload& Concrete =
		static_cast<const FHyperAIAudioTypedArtifactPayload&>(Clone.Get());
	TestEqual(TEXT("detached first operation"), Concrete.CanonicalOperations[0],
		FString(TEXT("sound-cue-create")));
	TestEqual(TEXT("detached shadow state"), Concrete.CanonicalShadowStates[0],
		FString(TEXT("/Game/Audio/A.A=sha256:shadow")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIAudioPlanTest,
	"HyperAIStudio.NativeTools.AudioMetaSound.PlanCASBatchingAndFailClosed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIAudioPlanTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Audio::Tests;
	const FString Target = UniqueAssetPath(TEXT("SC_Plan"));
	FHyperAIAudioApplyPlanRequest Request;
	Request.Operations.Add(SoundCueCreate(Target));
	FHyperAIAudioPlanOperation Configure;
	Configure.Variant = TEXT("sound_cue.configure");
	Configure.TargetPath = Target;
	FHyperAIAudioParameterValue Volume;
	Volume.Name = TEXT("volume_multiplier");
	Volume.Type = TEXT("float");
	Volume.FloatValue = 0.8;
	Configure.Parameters.Add(Volume);
	FHyperAIAudioParameterValue Pitch = Volume;
	Pitch.Name = TEXT("pitch_multiplier");
	Pitch.FloatValue = 1.1;
	Configure.Parameters.Add(Pitch);
	Request.Operations.Add(Configure);
	const FHyperAIAudioApplyPlanReport First =
		UHyperAIStudioAudioToolset::hyper_audio_apply_plan(Request);
	FHyperAIAudioApplyPlanRequest Reordered = Request;
	Reordered.Operations[1].Parameters.Swap(0, 1);
	const FHyperAIAudioApplyPlanReport Second =
		UHyperAIStudioAudioToolset::hyper_audio_apply_plan(Reordered);
	TestTrue(TEXT("create-to-configure one-call dry-run succeeds"), First.bOk);
	TestEqual(TEXT("all apply plans external effect"), First.SafetyClass,
		FString(TEXT("external_effect")));
	TestTrue(TEXT("trusted grant always required"), First.bRequiresTrustedAuthorization);
	TestEqual(TEXT("dry-run status exposes grant requirement"), First.Status,
		FString(TEXT("valid_requires_trusted_authorization")));
	TestFalse(TEXT("dry-run never submits"), First.bExecutionSubmitted);
	TestEqual(TEXT("effect summary operation count"), First.Effects.OperationCount, 2);
	TestEqual(TEXT("one create"), First.Effects.CreateCount, 1);
	TestEqual(TEXT("one update"), First.Effects.UpdateCount, 1);
	TestFalse(TEXT("no playback requested"), First.Effects.bPlaybackRequested);
	TestFalse(TEXT("no mic capture requested"), First.Effects.bMicrophoneCaptureRequested);
	TestEqual(TEXT("semantic parameter ordering is deterministic"), First.PlanHash, Second.PlanHash);
	TestEqual(TEXT("deterministic effect hash"), First.EffectFingerprint,
		Second.EffectFingerprint);
	TestTrue(TEXT("shared Prepare contract sealed"),
		First.PreparedContractFingerprint.StartsWith(TEXT("sha256:")));

	Request.bDryRun = false;
	Request.OperationId = TEXT("audio-plan-test-operation-001");
	Request.ExpectedPlanHash = First.PlanHash;
	const FHyperAIAudioApplyPlanReport NonDry =
		UHyperAIStudioAudioToolset::hyper_audio_apply_plan(Request);
	TestFalse(TEXT("source candidate never reports mutation success"), NonDry.bOk);
	TestFalse(TEXT("source candidate never submits"), NonDry.bExecutionSubmitted);
	TestEqual(TEXT("async trusted backend required"), NonDry.Status,
		FString(TEXT("staged_backend_required")));
	Request.ExpectedPlanHash = Sha(TEXT('f'));
	const FHyperAIAudioApplyPlanReport WrongHash =
		UHyperAIStudioAudioToolset::hyper_audio_apply_plan(Request);
	TestEqual(TEXT("wrong echoed plan hash rejected"), WrongHash.Status,
		FString(TEXT("expected_plan_hash_mismatch")));

	const FString PackageName = TEXT("/Game/__HyperAIStudioAutomation/SoundCueCAS_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
	UPackage* Package = CreatePackage(*PackageName);
	USoundCue* Cue = Package ? NewObject<USoundCue>(Package, FName(TEXT("SoundCueFixture")), RF_Transient) : nullptr;
	TestNotNull(TEXT("loaded SoundCue CAS fixture"), Cue);
	if (Cue)
	{
		const FString Revision = FHyperAIStudioAudioContracts::ComputeLoadedTargetRevision(
			Cue->GetPathName());
		TestTrue(TEXT("loaded revision canonical"), Revision.StartsWith(TEXT("sha256:")));
		FHyperAIAudioApplyPlanRequest CasRequest;
		FHyperAIAudioPlanOperation Op;
		Op.Variant = TEXT("sound_cue.configure");
		Op.TargetPath = Cue->GetPathName();
		Op.ExpectedRevision = Revision;
		CasRequest.Operations.Add(Op);
		const FHyperAIAudioApplyPlanReport CasOk =
			UHyperAIStudioAudioToolset::hyper_audio_apply_plan(CasRequest);
		TestTrue(TEXT("exact loaded CAS passes dry-run"), CasOk.bOk);
		Cue->VolumeMultiplier = 0.37f;
		const FString ChangedRevision = FHyperAIStudioAudioContracts::ComputeLoadedTargetRevision(
			Cue->GetPathName());
		TestNotEqual(TEXT("selected persisted property changes revision"), ChangedRevision, Revision);
		const FHyperAIAudioApplyPlanReport CasBad =
			UHyperAIStudioAudioToolset::hyper_audio_apply_plan(CasRequest);
		TestEqual(TEXT("stale loaded CAS rejected"), CasBad.Status, FString(TEXT("cas_failed")));
		Cue->MarkAsGarbage();
	}
	if (Package) Package->SetDirtyFlag(false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIAudioSnapshotTest,
	"HyperAIStudio.NativeTools.AudioMetaSound.SnapshotPagingValidationAndPrerequisites",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIAudioSnapshotTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Audio::Tests;
	FHyperAIAudioValueSnapshot Snapshot;
	Snapshot.Scope = TEXT("loaded_only");
	Snapshot.bComplete = true;
	FHyperAIAudioRecord Wave;
	Wave.Variant = TEXT("sound_wave");
	Wave.StableId = TEXT("sound_wave:/Game/Audio/W.W");
	Wave.ObjectPath = TEXT("/Game/Audio/W.W");
	Wave.ClassPath = TEXT("/Script/Engine.SoundWave");
	Wave.Revision = Sha(TEXT('a'));
	Wave.bLoaded = true;
	Snapshot.Records.Add(Wave);
	FHyperAIAudioRecord Meta;
	Meta.Variant = TEXT("metasound_source");
	Meta.StableId = TEXT("metasound_source:/Game/Audio/MS.MS");
	Meta.ObjectPath = TEXT("/Game/Audio/MS.MS");
	Meta.ClassPath = TEXT("/Script/MetasoundEngine.MetaSoundSource");
	Meta.Revision = Sha(TEXT('b'));
	Meta.bLoaded = true;
	Meta.Details.Add(TEXT("frontend_valid=false"));
	Snapshot.Records.Add(Meta);
	const FString FirstRevision = FHyperAIStudioAudioContracts::ComputeSnapshotRevision(Snapshot);
	const FString SecondRevision = FHyperAIStudioAudioContracts::ComputeSnapshotRevision(Snapshot);
	TestEqual(TEXT("snapshot hash deterministic"), FirstRevision, SecondRevision);
	bool bTruncated = false;
	const TArray<FHyperAIAudioIssue> Issues =
		FHyperAIStudioAudioContracts::ValidateSnapshot(Snapshot, 32, bTruncated);
	TestTrue(TEXT("invalid SoundWave metrics detected"), Issues.ContainsByPredicate(
		[](const FHyperAIAudioIssue& Issue)
		{
			return Issue.Code == TEXT("sound_wave_metrics_invalid");
		}));
	TestTrue(TEXT("invalid persisted MetaSound frontend detected"), Issues.ContainsByPredicate(
		[](const FHyperAIAudioIssue& Issue)
		{
			return Issue.Code == TEXT("metasound_frontend_invalid")
				&& Issue.Severity == TEXT("error");
		}));

	FHyperAIAudioInspectRequest InvalidProjection;
	InvalidProjection.Projection = {TEXT("identity"), TEXT("identity")};
	const FHyperAIAudioInspectReport ProjectionReport =
		UHyperAIStudioAudioToolset::hyper_audio_inspect(InvalidProjection);
	TestFalse(TEXT("duplicate projection rejected"), ProjectionReport.bOk);
	FHyperAIAudioInspectRequest InvalidCursor;
	InvalidCursor.Cursor = TEXT("v1|sha256:bad|0");
	const FHyperAIAudioInspectReport CursorReport =
		UHyperAIStudioAudioToolset::hyper_audio_inspect(InvalidCursor);
	TestFalse(TEXT("stale cursor rejected"), CursorReport.bOk);
	TestEqual(TEXT("cursor status stable"), CursorReport.Status,
		FString(TEXT("stale_or_invalid_cursor")));

	FHyperAIAudioInspectRequest MatrixRequest;
	MatrixRequest.DeadlineMs = 1;
	const FHyperAIAudioInspectReport MatrixReport =
		UHyperAIStudioAudioToolset::hyper_audio_inspect(MatrixRequest);
	TestEqual(TEXT("complete optional prerequisite matrix exposed"),
		MatrixReport.Prerequisites.Num(), 13);
	TSet<FString> Families;
	for (const FHyperAIAudioPrerequisite& Prerequisite : MatrixReport.Prerequisites)
	{
		Families.Add(Prerequisite.Family);
		TestTrue(TEXT("closed prerequisite state"),
			Prerequisite.State == TEXT("available") || Prerequisite.State == TEXT("disabled")
			|| Prerequisite.State == TEXT("not_loaded") || Prerequisite.State == TEXT("unavailable"));
	}
		TestEqual(TEXT("matrix families unique"), Families.Num(), 13);

		FHyperAIAudioInspectRequest BoundedMatrixRequest;
		BoundedMatrixRequest.DeadlineMs = 1;
		BoundedMatrixRequest.MaxOutputBytes = 4096;
		const FHyperAIAudioInspectReport BoundedMatrixReport =
			UHyperAIStudioAudioToolset::hyper_audio_inspect(BoundedMatrixRequest);
		TestFalse(TEXT("prerequisite evidence never escapes an undersized envelope"),
			BoundedMatrixReport.bOk);
		TestTrue(TEXT("undersized prerequisite envelope is explicit"),
			BoundedMatrixReport.bTruncated
			&& BoundedMatrixReport.Status == TEXT("output_budget_too_small"));
		TestTrue(TEXT("only the bounded prerequisite prefix is returned"),
			BoundedMatrixReport.Prerequisites.Num() < MatrixReport.Prerequisites.Num());
		return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIAudioPersistedStateTest,
	"HyperAIStudio.NativeTools.AudioMetaSound.PersistedStateRecursiveCanonicalization",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIAudioPersistedStateTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Audio::Tests;
	const FString MixPackageName = TEXT("/Game/__HyperAIStudioAutomation/AudioMixState_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
	UPackage* MixPackage = CreatePackage(*MixPackageName);
	USoundMix* Mix = MixPackage
		? NewObject<USoundMix>(MixPackage, FName(TEXT("AudioMixState")), RF_Public | RF_Standalone)
		: nullptr;
	TestNotNull(TEXT("recursive array fixture"), Mix);
	if (Mix)
	{
		FSoundClassAdjuster Adjuster;
		Adjuster.VolumeAdjuster = 0.25f;
		Mix->SoundClassEffects.Add(Adjuster);
		MixPackage->SetDirtyFlag(true);
		const FString First = FHyperAIStudioAudioContracts::ComputeLoadedTargetRevision(Mix->GetPathName());
		Mix->SoundClassEffects[0].VolumeAdjuster = 0.75f;
		const FString Second = FHyperAIStudioAudioContracts::ComputeLoadedTargetRevision(Mix->GetPathName());
		TestTrue(TEXT("same-count initial revision complete"), First.StartsWith(TEXT("sha256:")));
		TestTrue(TEXT("same-count changed revision complete"), Second.StartsWith(TEXT("sha256:")));
		TestNotEqual(TEXT("same-count nested value changes dirty-already revision"), First, Second);
	}

	const FString CuePackageName = TEXT("/Game/__HyperAIStudioAutomation/AudioSoftState_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
	UPackage* CuePackage = CreatePackage(*CuePackageName);
	USoundCue* Cue = CuePackage
		? NewObject<USoundCue>(CuePackage, FName(TEXT("AudioSoftState")), RF_Public | RF_Standalone)
		: nullptr;
	USoundNodeWavePlayer* Player = Cue
		? NewObject<USoundNodeWavePlayer>(Cue, FName(TEXT("NestedWavePlayer")), RF_Transactional)
		: nullptr;
	const FString WavePackageAName = TEXT("/Game/__HyperAIStudioAutomation/WaveSoftA_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
	const FString WavePackageBName = TEXT("/Game/__HyperAIStudioAutomation/WaveSoftB_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
	UPackage* WavePackageA = CreatePackage(*WavePackageAName);
	UPackage* WavePackageB = CreatePackage(*WavePackageBName);
	USoundWave* WaveA = WavePackageA
		? NewObject<USoundWave>(WavePackageA,
			FName(*FPackageName::GetLongPackageAssetName(WavePackageAName)), RF_Public | RF_Standalone) : nullptr;
	USoundWave* WaveB = WavePackageB
		? NewObject<USoundWave>(WavePackageB,
			FName(*FPackageName::GetLongPackageAssetName(WavePackageBName)), RF_Public | RF_Standalone) : nullptr;
	TestNotNull(TEXT("nested soft player"), Player);
	TestNotNull(TEXT("nested soft wave A"), WaveA);
	TestNotNull(TEXT("nested soft wave B"), WaveB);
	if (Cue && Player && WaveA && WaveB)
	{
		Cue->FirstNode = Player;
#if WITH_EDITORONLY_DATA
		Cue->AllNodes.Add(Player);
#endif
		Player->SetSoundWave(WaveA);
		const FString WithA = FHyperAIStudioAudioContracts::ComputeLoadedTargetRevision(Cue->GetPathName());
		Player->SetSoundWave(WaveB);
		const FString WithB = FHyperAIStudioAudioContracts::ComputeLoadedTargetRevision(Cue->GetPathName());
		TestTrue(TEXT("nested soft revision A complete"), WithA.StartsWith(TEXT("sha256:")));
		TestTrue(TEXT("nested soft revision B complete"), WithB.StartsWith(TEXT("sha256:")));
		TestNotEqual(TEXT("nested subobject soft-path mutation changes revision"), WithA, WithB);
		FHyperAIAudioInspectRequest Inspect;
		Inspect.ObjectPaths = {Cue->GetPathName()};
		const FHyperAIAudioInspectReport InspectReport =
			UHyperAIStudioAudioToolset::hyper_audio_inspect(Inspect);
		TestTrue(TEXT("nested soft reference is projected without loading"),
			!InspectReport.Records.IsEmpty()
			&& InspectReport.Records[0].References.Contains(WaveB->GetPathName()));
	}

	const FString DialoguePackageName = TEXT("/Game/__HyperAIStudioAutomation/AudioLimit_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
	UPackage* DialoguePackage = CreatePackage(*DialoguePackageName);
	UDialogueWave* Dialogue = DialoguePackage
		? NewObject<UDialogueWave>(DialoguePackage, FName(TEXT("AudioLimit")), RF_Public | RF_Standalone)
		: nullptr;
	if (Dialogue)
	{
		Dialogue->SpokenText = FString::ChrN(65537, TEXT('x'));
		TestTrue(TEXT("oversized persisted leaf makes revision unavailable"),
			FHyperAIStudioAudioContracts::ComputeLoadedTargetRevision(Dialogue->GetPathName()).IsEmpty());
		FHyperAIAudioInspectRequest LimitInspect;
		LimitInspect.ObjectPaths = {Dialogue->GetPathName()};
		const FHyperAIAudioInspectReport LimitReport =
			UHyperAIStudioAudioToolset::hyper_audio_inspect(LimitInspect);
		TestFalse(TEXT("oversized persisted leaf marks snapshot revision incomplete"),
			LimitReport.bRevisionComplete);
		TestTrue(TEXT("incomplete record never fabricates a CAS revision"),
			!LimitReport.Records.IsEmpty() && LimitReport.Records[0].Revision.IsEmpty());
	}

	for (UObject* Object : {static_cast<UObject*>(Mix), static_cast<UObject*>(Cue),
		static_cast<UObject*>(Player), static_cast<UObject*>(WaveA), static_cast<UObject*>(WaveB),
		static_cast<UObject*>(Dialogue)})
		if (Object) Object->MarkAsGarbage();
	for (UPackage* Package : {MixPackage, CuePackage, WavePackageA, WavePackageB, DialoguePackage})
		if (Package) Package->SetDirtyFlag(false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIAudioMetaSoundDocumentTest,
	"HyperAIStudio.NativeTools.AudioMetaSound.PersistedFrontendHashAndMalformedFixtures",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIAudioMetaSoundDocumentTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Audio::Tests;
	auto InitializeMinimalDocument = [](FMetasoundFrontendDocument& Document)
	{
		Document.Metadata.Version.Name = TEXT("HyperAI.TestDocument");
		Document.RootGraph.ID = FGuid::NewGuid();
		Document.RootGraph.InitDefaultGraphPage();
	};
	auto Validate = [&](const TCHAR* Label, const FMetasoundFrontendDocument& Document,
		FString& OutRevision)
	{
		bool bComplete = false;
		const bool bValid = FHyperAIStudioAudioContracts::ValidateMetaSoundDocumentForTest(
			Document, OutRevision, bComplete);
		TestTrue(Label, bComplete && bValid && OutRevision.StartsWith(TEXT("sha256:")));
		return bValid;
	};

	FMetasoundFrontendDocument Baseline;
	InitializeMinimalDocument(Baseline);
	FString BaselineRevision;
	if (!Validate(TEXT("minimal persisted frontend fixture valid"), Baseline, BaselineRevision))
		return true;

	FMetasoundFrontendDocument WithDefault = Baseline;
	FMetasoundFrontendClassInput Input;
	Input.VertexID = FGuid::NewGuid();
	Input.Name = TEXT("Gain");
	Input.TypeName = TEXT("Float");
	FMetasoundFrontendLiteral DefaultLiteral;
	DefaultLiteral.Set(0.25f);
	Input.InitDefault(MoveTemp(DefaultLiteral));
	WithDefault.RootGraph.GetDefaultInterface().Inputs.Add(MoveTemp(Input));
	FString DefaultRevision;
	Validate(TEXT("root input default fixture valid"), WithDefault, DefaultRevision);
	TestNotEqual(TEXT("root member/default participates in full document hash"),
		DefaultRevision, BaselineRevision);
	WithDefault.RootGraph.GetDefaultInterface().Inputs[0]
		.FindDefaultChecked(Metasound::Frontend::DefaultPageID).Set(0.75f);
	FString ChangedDefaultRevision;
	Validate(TEXT("changed root default fixture valid"), WithDefault, ChangedDefaultRevision);
	TestNotEqual(TEXT("same-count literal value mutation changes document hash"),
		ChangedDefaultRevision, DefaultRevision);

	FMetasoundFrontendDocument WithInterface = Baseline;
	FMetasoundFrontendVersion InterfaceVersion;
	InterfaceVersion.Name = TEXT("HyperAI.TestInterface");
	WithInterface.Interfaces.Add(InterfaceVersion);
	FString InterfaceRevision;
	Validate(TEXT("declared interface fixture valid"), WithInterface, InterfaceRevision);
	TestNotEqual(TEXT("interfaces participate in full document hash"),
		InterfaceRevision, BaselineRevision);
	FMetasoundFrontendDocument DuplicateInterface = WithInterface;
	const int32 InterfaceCountBeforeDuplicateAdd = DuplicateInterface.Interfaces.Num();
	DuplicateInterface.Interfaces.Add(InterfaceVersion);
	FString DuplicateInterfaceRevision;
	TestEqual(TEXT("declared interface set ignores duplicate version adds"),
		DuplicateInterface.Interfaces.Num(), InterfaceCountBeforeDuplicateAdd);
	Validate(TEXT("duplicate interface add remains a valid set"),
		DuplicateInterface, DuplicateInterfaceRevision);
	TestEqual(TEXT("idempotent interface add preserves document hash"),
		DuplicateInterfaceRevision, InterfaceRevision);

	FMetasoundFrontendDocument WithDependency = Baseline;
	FMetasoundFrontendClass Dependency;
	if (!TestTrue(TEXT("real UE Float input node is registered"),
		InitializeRegisteredDependency(Dependency))) return true;
	WithDependency.Dependencies.Add(MoveTemp(Dependency));
	FString DependencyRevision;
	Validate(TEXT("dependency fixture valid"), WithDependency, DependencyRevision);
	TestNotEqual(TEXT("dependencies participate in full document hash"),
		DependencyRevision, BaselineRevision);
	FMetasoundFrontendDocument GraphTypedDependency = WithDependency;
	GraphTypedDependency.Dependencies[0].Metadata.SetType(EMetasoundFrontendClassType::Graph);
	FString InvalidDependencyRevision;
	bool bInvalidDependencyComplete = false;
	TestFalse(TEXT("Graph-typed serialized dependency rejected before registry-key construction"),
		FHyperAIStudioAudioContracts::ValidateMetaSoundDocumentForTest(
			GraphTypedDependency, InvalidDependencyRevision, bInvalidDependencyComplete));
	TestTrue(TEXT("invalid dependency metadata remains bounded"), bInvalidDependencyComplete);
	FMetasoundFrontendDocument DuplicateDependencyKey = WithDependency;
	FMetasoundFrontendClass SameKeyDependency = DuplicateDependencyKey.Dependencies[0];
	SameKeyDependency.ID = FGuid::NewGuid();
	DuplicateDependencyKey.Dependencies.Add(MoveTemp(SameKeyDependency));
	bInvalidDependencyComplete = false;
	TestFalse(TEXT("duplicate dependency registry key rejected"),
		FHyperAIStudioAudioContracts::ValidateMetaSoundDocumentForTest(
			DuplicateDependencyKey, InvalidDependencyRevision, bInvalidDependencyComplete));
	TestTrue(TEXT("duplicate dependency key remains bounded"), bInvalidDependencyComplete);
	FMetasoundFrontendDocument UnregisteredDependency = Baseline;
	FMetasoundFrontendClass SyntacticallyValidUnregistered = WithDependency.Dependencies[0];
	SyntacticallyValidUnregistered.ID = FGuid::NewGuid();
	SyntacticallyValidUnregistered.Metadata.SetClassName(FMetasoundFrontendClassName(
		TEXT("HyperAI.Unregistered"), TEXT("SyntacticallyValid"), TEXT("Fixture")));
	UnregisteredDependency.Dependencies.Add(MoveTemp(SyntacticallyValidUnregistered));
	bInvalidDependencyComplete = false;
	TestFalse(TEXT("syntactically valid but unregistered dependency rejected"),
		FHyperAIStudioAudioContracts::ValidateMetaSoundDocumentForTest(
			UnregisteredDependency, InvalidDependencyRevision, bInvalidDependencyComplete));
	TestTrue(TEXT("unregistered registry lookup remains bounded"), bInvalidDependencyComplete);
	FMetasoundFrontendDocument WrongDependencyMetadata = WithDependency;
	const EMetasoundFrontendClassAccessFlags RegisteredAccess =
		WrongDependencyMetadata.Dependencies[0].Metadata.GetAccessFlags();
	WrongDependencyMetadata.Dependencies[0].Metadata.SetAccessFlags(
		RegisteredAccess == EMetasoundFrontendClassAccessFlags::None
			? EMetasoundFrontendClassAccessFlags::Referenceable
			: EMetasoundFrontendClassAccessFlags::None);
	bInvalidDependencyComplete = false;
	TestFalse(TEXT("registered dependency metadata mismatch rejected"),
		FHyperAIStudioAudioContracts::ValidateMetaSoundDocumentForTest(
			WrongDependencyMetadata, InvalidDependencyRevision, bInvalidDependencyComplete));
	TestTrue(TEXT("dependency metadata mismatch remains bounded"), bInvalidDependencyComplete);
	FMetasoundFrontendDocument WrongDependencyOutput = WithDependency;
	if (!WrongDependencyOutput.Dependencies[0].GetDefaultInterface().Outputs.IsEmpty())
	{
		WrongDependencyOutput.Dependencies[0].GetDefaultInterface().Outputs[0].Name =
			TEXT("RegistryOutputMismatch");
		bInvalidDependencyComplete = false;
		TestFalse(TEXT("registered dependency output mismatch rejected"),
			FHyperAIStudioAudioContracts::ValidateMetaSoundDocumentForTest(
				WrongDependencyOutput, InvalidDependencyRevision, bInvalidDependencyComplete));
		TestTrue(TEXT("dependency output mismatch remains bounded"), bInvalidDependencyComplete);
	}
	FMetasoundFrontendDocument RegisteredOutputDependency = Baseline;
	FMetasoundFrontendClass OutputDependency;
	if (!TestTrue(TEXT("real UE Float output node is registered"),
		InitializeRegisteredDependency(OutputDependency, TEXT("Float"), false))) return true;
	RegisteredOutputDependency.Dependencies.Add(OutputDependency);
	FString RegisteredOutputRevision;
	Validate(TEXT("registered output-node dependency fixture valid"),
		RegisteredOutputDependency, RegisteredOutputRevision);
	if (!RegisteredOutputDependency.Dependencies[0].GetDefaultInterface().Inputs.IsEmpty())
	{
		RegisteredOutputDependency.Dependencies[0].GetDefaultInterface().Inputs[0].TypeName =
			TEXT("Bool");
		bInvalidDependencyComplete = false;
		TestFalse(TEXT("registered dependency input mismatch rejected"),
			FHyperAIStudioAudioContracts::ValidateMetaSoundDocumentForTest(
				RegisteredOutputDependency, InvalidDependencyRevision, bInvalidDependencyComplete));
		TestTrue(TEXT("dependency input mismatch remains bounded"), bInvalidDependencyComplete);
	}

	FMetasoundFrontendDocument WithSubgraph = Baseline;
	FMetasoundFrontendGraphClass Subgraph;
	Subgraph.ID = FGuid::NewGuid();
	WithSubgraph.Subgraphs.Add(MoveTemp(Subgraph));
	FString SubgraphRevision;
	Validate(TEXT("subgraph fixture valid"), WithSubgraph, SubgraphRevision);
	TestNotEqual(TEXT("subgraphs participate in full document hash"),
		SubgraphRevision, BaselineRevision);
	FMetasoundFrontendDocument RootAndSubgraphDefaultPages = Baseline;
	FMetasoundFrontendGraphClass DefaultPageSubgraph;
	DefaultPageSubgraph.ID = FGuid::NewGuid();
	DefaultPageSubgraph.InitDefaultGraphPage();
	RootAndSubgraphDefaultPages.Subgraphs.Add(MoveTemp(DefaultPageSubgraph));
	FString RootAndSubgraphRevision;
	Validate(TEXT("root and subgraph may each own zero DefaultPageID"),
		RootAndSubgraphDefaultPages, RootAndSubgraphRevision);
	TestEqual(TEXT("UE default page is intentionally zero GUID"),
		RootAndSubgraphDefaultPages.RootGraph.GetConstGraphPages()[0].PageID,
		Metasound::Frontend::DefaultPageID);

	FMetasoundFrontendDocument SameDirectionalName = Baseline;
	FMetasoundFrontendClassInput SameNameInput;
	SameNameInput.VertexID = FGuid::NewGuid();
	SameNameInput.Name = TEXT("Value");
	SameNameInput.TypeName = TEXT("Float");
	FMetasoundFrontendClassOutput SameNameOutput;
	SameNameOutput.VertexID = FGuid::NewGuid();
	SameNameOutput.Name = SameNameInput.Name;
	SameNameOutput.TypeName = SameNameInput.TypeName;
	SameDirectionalName.RootGraph.GetDefaultInterface().Inputs.Add(SameNameInput);
	SameDirectionalName.RootGraph.GetDefaultInterface().Outputs.Add(SameNameOutput);
	FString SameDirectionalNameRevision;
	Validate(TEXT("input and output may share one interface name"),
		SameDirectionalName, SameDirectionalNameRevision);

	FMetasoundFrontendDocument WrongTypedDefault = WithDefault;
	WrongTypedDefault.RootGraph.GetDefaultInterface().Inputs[0]
		.FindDefaultChecked(Metasound::Frontend::DefaultPageID).Set(TArray<float>{0.25f});
	FString WrongTypedRevision;
	bool bWrongTypedComplete = false;
	TestFalse(TEXT("class default literal/data-type mismatch rejected"),
		FHyperAIStudioAudioContracts::ValidateMetaSoundDocumentForTest(
			WrongTypedDefault, WrongTypedRevision, bWrongTypedComplete));
	TestTrue(TEXT("typed mismatch remains a complete bounded observation"), bWrongTypedComplete);

	FMetasoundFrontendDocument SharedVertexInstances = Baseline;
	FMetasoundFrontendClass SharedVertexDependency;
	if (!TestTrue(TEXT("shared-vertex fixture registry class available"),
		InitializeRegisteredDependency(SharedVertexDependency))) return true;
	auto MakeSharedVertexNode = [&](const FName Name)
	{
		FMetasoundFrontendNode Node;
		Node.UpdateID(FGuid::NewGuid());
		Node.ClassID = SharedVertexDependency.ID;
		Node.Name = Name;
		Node.Interface = FMetasoundFrontendNodeInterface(
			SharedVertexDependency.GetDefaultInterface());
		for (FMetasoundFrontendVertex& Vertex : Node.Interface.Inputs) Vertex.Name = Name;
		for (FMetasoundFrontendVertex& Vertex : Node.Interface.Outputs) Vertex.Name = Name;
		return Node;
	};
	FMetasoundFrontendGraph& SharedVertexGraph = SharedVertexInstances.RootGraph.FindGraphChecked(
		Metasound::Frontend::DefaultPageID);
	SharedVertexGraph.Nodes.Add(MakeSharedVertexNode(TEXT("InstanceA")));
	SharedVertexGraph.Nodes.Add(MakeSharedVertexNode(TEXT("InstanceB")));
	SharedVertexInstances.Dependencies.Add(MoveTemp(SharedVertexDependency));
	FString SharedVertexRevision;
	Validate(TEXT("two class instances may share class vertex GUIDs"),
		SharedVertexInstances, SharedVertexRevision);

	FMetasoundFrontendDocument EnvironmentNode = Baseline;
	FMetasoundFrontendGraphClass EnvironmentClass;
	EnvironmentClass.ID = FGuid::NewGuid();
	EnvironmentClass.InitDefaultGraphPage();
	FMetasoundFrontendClassEnvironmentVariable EnvironmentVariable;
	EnvironmentVariable.Name = TEXT("HyperAI.Environment");
	EnvironmentVariable.TypeName = TEXT("Float");
	EnvironmentVariable.bIsRequired = true;
	EnvironmentClass.GetDefaultInterface().Environment.Add(EnvironmentVariable);
	FMetasoundFrontendNode EnvironmentInstance;
	EnvironmentInstance.UpdateID(FGuid::NewGuid());
	EnvironmentInstance.ClassID = EnvironmentClass.ID;
	EnvironmentInstance.Name = TEXT("EnvironmentInstance");
	EnvironmentInstance.Interface = FMetasoundFrontendNodeInterface(
		EnvironmentClass.GetDefaultInterface());
	TestFalse(TEXT("UE class-to-node environment vertex intentionally has no VertexID"),
		EnvironmentInstance.Interface.Environment[0].VertexID.IsValid());
	EnvironmentNode.RootGraph.FindGraphChecked(
		Metasound::Frontend::DefaultPageID).Nodes.Add(EnvironmentInstance);
	EnvironmentNode.Subgraphs.Add(EnvironmentClass);
	FString EnvironmentRevision;
	Validate(TEXT("environment node with unset VertexID is valid"),
		EnvironmentNode, EnvironmentRevision);
	FMetasoundFrontendDocument DuplicateEnvironment = EnvironmentNode;
	const FMetasoundFrontendVertex DuplicateEnvironmentVariable =
		DuplicateEnvironment.RootGraph.FindGraphChecked(Metasound::Frontend::DefaultPageID)
			.Nodes[0].Interface.Environment[0];
	DuplicateEnvironment.RootGraph.FindGraphChecked(Metasound::Frontend::DefaultPageID)
		.Nodes[0].Interface.Environment.Add(DuplicateEnvironmentVariable);
	FString EnvironmentInvalidRevision;
	bool bEnvironmentComplete = false;
	TestFalse(TEXT("duplicate node environment name rejected"),
		FHyperAIStudioAudioContracts::ValidateMetaSoundDocumentForTest(
			DuplicateEnvironment, EnvironmentInvalidRevision, bEnvironmentComplete));
	TestTrue(TEXT("duplicate environment fixture remains bounded"), bEnvironmentComplete);
	FMetasoundFrontendDocument WrongEnvironmentName = EnvironmentNode;
	WrongEnvironmentName.RootGraph.FindGraphChecked(Metasound::Frontend::DefaultPageID)
		.Nodes[0].Interface.Environment[0].Name = TEXT("HyperAI.WrongEnvironment");
	bEnvironmentComplete = false;
	TestFalse(TEXT("node environment name mismatch rejected"),
		FHyperAIStudioAudioContracts::ValidateMetaSoundDocumentForTest(
			WrongEnvironmentName, EnvironmentInvalidRevision, bEnvironmentComplete));
	TestTrue(TEXT("environment name mismatch remains bounded"), bEnvironmentComplete);
	FMetasoundFrontendDocument WrongEnvironmentType = EnvironmentNode;
	WrongEnvironmentType.RootGraph.FindGraphChecked(Metasound::Frontend::DefaultPageID)
		.Nodes[0].Interface.Environment[0].TypeName = TEXT("Bool");
	bEnvironmentComplete = false;
	TestFalse(TEXT("node environment type mismatch rejected"),
		FHyperAIStudioAudioContracts::ValidateMetaSoundDocumentForTest(
			WrongEnvironmentType, EnvironmentInvalidRevision, bEnvironmentComplete));
	TestTrue(TEXT("environment type mismatch remains bounded"), bEnvironmentComplete);

	FMetasoundFrontendDocument WithTemplate = Baseline;
	WithTemplate.Template.InitializeAs<FMetaSoundFrontendDocumentVertexTemplate>();
	FString TemplateRevision;
	Validate(TEXT("frontend template fixture valid"), WithTemplate, TemplateRevision);
	TestNotEqual(TEXT("template dynamic type/state participates in full document hash"),
		TemplateRevision, BaselineRevision);

	FMetasoundFrontendDocument DuplicateMember = WithDefault;
	const FMetasoundFrontendClassInput DuplicateRootInput =
		DuplicateMember.RootGraph.GetDefaultInterface().Inputs[0];
	DuplicateMember.RootGraph.GetDefaultInterface().Inputs.Add(DuplicateRootInput);
	FString MalformedRevision;
	bool bMalformedComplete = false;
	TestFalse(TEXT("duplicate root member fixture rejected"),
		FHyperAIStudioAudioContracts::ValidateMetaSoundDocumentForTest(
			DuplicateMember, MalformedRevision, bMalformedComplete));
	TestTrue(TEXT("malformed member fixture remains bounded"), bMalformedComplete);

	FMetasoundFrontendDocument DuplicateLiteral = Baseline;
	FMetasoundFrontendClass LiteralDependency;
	if (!TestTrue(TEXT("literal fixture registry class available"),
		InitializeRegisteredDependency(LiteralDependency, TEXT("Float"), false))) return true;
	if (!TestTrue(TEXT("registered Float output node exposes an input"),
		!LiteralDependency.GetDefaultInterface().Inputs.IsEmpty())) return true;
	const FMetasoundFrontendClassInput& ClassInput =
		LiteralDependency.GetDefaultInterface().Inputs[0];
	FMetasoundFrontendNode Node;
	Node.UpdateID(FGuid::NewGuid());
	Node.ClassID = LiteralDependency.ID;
	Node.Name = TEXT("LiteralNode");
	Node.Interface = FMetasoundFrontendNodeInterface(
		LiteralDependency.GetDefaultInterface());
	for (FMetasoundFrontendVertex& Vertex : Node.Interface.Inputs) Vertex.Name = Node.Name;
	for (FMetasoundFrontendVertex& Vertex : Node.Interface.Outputs) Vertex.Name = Node.Name;
	const FMetasoundFrontendVertex& NodeInput = Node.Interface.Inputs[0];
	FMetasoundFrontendVertexLiteral FirstLiteral;
	FirstLiteral.VertexID = NodeInput.VertexID;
	FirstLiteral.Value.Set(0.25f);
	FMetasoundFrontendVertexLiteral SecondLiteral = FirstLiteral;
	SecondLiteral.Value.Set(0.75f);
	Node.InputLiterals = {FirstLiteral, SecondLiteral};
	DuplicateLiteral.Dependencies.Add(MoveTemp(LiteralDependency));
	DuplicateLiteral.RootGraph.FindGraphChecked(
		Metasound::Frontend::DefaultPageID).Nodes.Add(MoveTemp(Node));
	FMetasoundFrontendDocument OneInputLiteral = DuplicateLiteral;
	FMetasoundFrontendNode& LiteralNode = OneInputLiteral.RootGraph.FindGraphChecked(
		Metasound::Frontend::DefaultPageID).Nodes[0];
	LiteralNode.InputLiterals.SetNum(1);
	FString InputLiteralRevisionA;
	Validate(TEXT("single node input literal fixture valid"),
		OneInputLiteral, InputLiteralRevisionA);
	LiteralNode.InputLiterals[0].Value.Set(0.5f);
	FString InputLiteralRevisionB;
	Validate(TEXT("changed node input literal fixture valid"),
		OneInputLiteral, InputLiteralRevisionB);
	TestNotEqual(TEXT("InputLiterals values participate in full document hash"),
		InputLiteralRevisionA, InputLiteralRevisionB);
	bMalformedComplete = false;
	TestFalse(TEXT("duplicate node input literal fixture rejected"),
		FHyperAIStudioAudioContracts::ValidateMetaSoundDocumentForTest(
			DuplicateLiteral, MalformedRevision, bMalformedComplete));
	TestTrue(TEXT("malformed literal fixture remains bounded"), bMalformedComplete);

	FMetasoundFrontendDocument TypeMismatchEdge = Baseline;
	FMetasoundFrontendClass FromClass;
	if (!TestTrue(TEXT("edge source registry class available"),
		InitializeRegisteredDependency(FromClass, TEXT("Float"), true))) return true;
	if (!TestTrue(TEXT("registered Float input node exposes an output"),
		!FromClass.GetDefaultInterface().Outputs.IsEmpty())) return true;
	const FMetasoundFrontendClassOutput& ClassOutput = FromClass.GetDefaultInterface().Outputs[0];
	FMetasoundFrontendClass ToClass;
	if (!TestTrue(TEXT("edge target registry class available"),
		InitializeRegisteredDependency(ToClass, TEXT("Int32"), false))) return true;
	if (!TestTrue(TEXT("registered Int32 output node exposes an input"),
		!ToClass.GetDefaultInterface().Inputs.IsEmpty())) return true;
	const FMetasoundFrontendClassInput& MismatchedClassInput = ToClass.GetDefaultInterface().Inputs[0];
	FMetasoundFrontendNode FromNode;
	FromNode.UpdateID(FGuid::NewGuid());
	FromNode.ClassID = FromClass.ID;
	FromNode.Name = TEXT("FromNode");
	FromNode.Interface = FMetasoundFrontendNodeInterface(FromClass.GetDefaultInterface());
	const FMetasoundFrontendVertex& FromVertex = FromNode.Interface.Outputs[0];
	FMetasoundFrontendNode ToNode;
	ToNode.UpdateID(FGuid::NewGuid());
	ToNode.ClassID = ToClass.ID;
	ToNode.Name = TEXT("ToNode");
	ToNode.Interface = FMetasoundFrontendNodeInterface(ToClass.GetDefaultInterface());
	const FMetasoundFrontendVertex& ToVertex = ToNode.Interface.Inputs[0];
	FMetasoundFrontendEdge MismatchedEdge;
	MismatchedEdge.FromNodeID = FromNode.GetID();
	MismatchedEdge.FromVertexID = FromVertex.VertexID;
	MismatchedEdge.ToNodeID = ToNode.GetID();
	MismatchedEdge.ToVertexID = ToVertex.VertexID;
	TypeMismatchEdge.Dependencies = {MoveTemp(FromClass), MoveTemp(ToClass)};
	FMetasoundFrontendGraph& EdgeGraph = TypeMismatchEdge.RootGraph.FindGraphChecked(
		Metasound::Frontend::DefaultPageID);
	EdgeGraph.Nodes = {MoveTemp(FromNode), MoveTemp(ToNode)};
	EdgeGraph.Edges.Add(MismatchedEdge);
	bMalformedComplete = false;
	TestFalse(TEXT("edge data-type mismatch fixture rejected"),
		FHyperAIStudioAudioContracts::ValidateMetaSoundDocumentForTest(
			TypeMismatchEdge, MalformedRevision, bMalformedComplete));
	TestTrue(TEXT("malformed edge fixture remains bounded"), bMalformedComplete);
	FMetasoundFrontendDocument AccessMismatchEdge = TypeMismatchEdge;
	AccessMismatchEdge.Dependencies[1].GetDefaultInterface().Inputs[0].TypeName = TEXT("Float");
	AccessMismatchEdge.Dependencies[1].GetDefaultInterface().Inputs[0].AccessType =
		EMetasoundFrontendVertexAccessType::Value;
	AccessMismatchEdge.RootGraph.FindGraphChecked(Metasound::Frontend::DefaultPageID)
		.Nodes[1].Interface.Inputs[0].TypeName = TEXT("Float");
	bMalformedComplete = false;
	TestFalse(TEXT("edge access mismatch fixture rejected"),
		FHyperAIStudioAudioContracts::ValidateMetaSoundDocumentForTest(
			AccessMismatchEdge, MalformedRevision, bMalformedComplete));
	TestTrue(TEXT("malformed edge access fixture remains bounded"), bMalformedComplete);

	FMetasoundFrontendDocument UnresolvedClass = Baseline;
	FMetasoundFrontendNode UnresolvedNode;
	UnresolvedNode.UpdateID(FGuid::NewGuid());
	UnresolvedNode.ClassID = FGuid::NewGuid();
	UnresolvedNode.Name = TEXT("UnresolvedNode");
	UnresolvedClass.RootGraph.FindGraphChecked(
		Metasound::Frontend::DefaultPageID).Nodes.Add(MoveTemp(UnresolvedNode));
	bMalformedComplete = false;
	TestFalse(TEXT("unresolved node class fixture rejected"),
		FHyperAIStudioAudioContracts::ValidateMetaSoundDocumentForTest(
			UnresolvedClass, MalformedRevision, bMalformedComplete));
	TestTrue(TEXT("unresolved class fixture remains bounded"), bMalformedComplete);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIAudioEvidenceTest,
	"HyperAIStudio.NativeTools.AudioMetaSound.OnDiskFamilyAndExactIndexEvidence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIAudioEvidenceTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Audio::Tests;
	const FString Path = TEXT("/Game/Audio/W_Evidence.W_Evidence");
	const FString Package = TEXT("/Game/Audio/W_Evidence");
	const FString Class = TEXT("/Script/Engine.SoundWave");
	const FString HashA = FString::ChrN(40, TEXT('a'));
	const FString HashB = FString::ChrN(40, TEXT('b'));
	TestTrue(TEXT("missing package data has no revision"),
		FHyperAIStudioAudioContracts::ComputeOnDiskEvidenceRevisionForTest(
			Path, Package, Class, 1024, HashA, false).IsEmpty());
	TestTrue(TEXT("zero saved hash has no revision"),
		FHyperAIStudioAudioContracts::ComputeOnDiskEvidenceRevisionForTest(
			Path, Package, Class, 1024, FString::ChrN(40, TEXT('0')), true).IsEmpty());
	TestTrue(TEXT("zero package size has no usable on-disk evidence"),
		FHyperAIStudioAudioContracts::ComputeOnDiskEvidenceRevisionForTest(
			Path, Package, Class, 0, HashA, true).IsEmpty());
	const FString RevisionA = FHyperAIStudioAudioContracts::ComputeOnDiskEvidenceRevisionForTest(
		Path, Package, Class, 1024, HashA, true);
	const FString RevisionB = FHyperAIStudioAudioContracts::ComputeOnDiskEvidenceRevisionForTest(
		Path, Package, Class, 1024, HashB, true);
	TestTrue(TEXT("usable saved hash yields revision"), RevisionA.StartsWith(TEXT("sha256:")));
	TestNotEqual(TEXT("same-size different saved content cannot alias"), RevisionA, RevisionB);

	TestEqual(TEXT("explicit SourceEffectPresetChain classified"),
		FHyperAIStudioAudioContracts::ClassifyClassEvidenceForTest(
			TEXT("/Script/Engine.SoundEffectSourcePresetChain"), {}), FString(TEXT("synthesis_effect")));
	TestEqual(TEXT("explicit SoundSourceBus classified through SoundWave family"),
		FHyperAIStudioAudioContracts::ClassifyClassEvidenceForTest(
			TEXT("/Script/Engine.SoundSourceBus"), {}), FString(TEXT("sound_wave")));
	TestTrue(TEXT("derived submix ancestry accepted"),
		FHyperAIStudioAudioContracts::IsClassEvidenceCompatibleForTest(TEXT("submix"),
			TEXT("/Script/MyGame.MyDerivedSubmix"), {TEXT("/Script/Engine.SoundSubmixBase")}));
	TestTrue(TEXT("near-name class rejected"),
		FHyperAIStudioAudioContracts::ClassifyClassEvidenceForTest(
			TEXT("/Script/MyGame.SoundWaveImpostor"), {}).IsEmpty());
	TestFalse(TEXT("near-name submix rejected"),
		FHyperAIStudioAudioContracts::IsClassEvidenceCompatibleForTest(TEXT("submix"),
			TEXT("/Script/MyGame.SoundSubmixBaseImpostor"), {}));

	const FString ExactPackageName = TEXT("/Game/__HyperAIStudioAutomation/AudioExact_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
	UPackage* ExactPackage = CreatePackage(*ExactPackageName);
	USoundCue* ExactCue = ExactPackage
		? NewObject<USoundCue>(ExactPackage, FName(TEXT("AudioExact")), RF_Public | RF_Standalone)
		: nullptr;
	if (ExactCue)
	{
		FHyperAIAudioInspectRequest Exact;
		Exact.ObjectPaths = {ExactCue->GetPathName()};
		const FHyperAIAudioInspectReport ExactReport =
			UHyperAIStudioAudioToolset::hyper_audio_inspect(Exact);
		TestTrue(TEXT("exact-path request succeeds without a broad scan"), ExactReport.bOk);
		TestTrue(TEXT("loaded family uses exact IsA hierarchy"),
			!ExactReport.Records.IsEmpty()
			&& ExactReport.Records[0].Variant == TEXT("sound_cue"));
		TestEqual(TEXT("exact-path loaded lookup scans exactly one requested path"),
			ExactReport.SourceObjectsScanned, 1);
		FHyperAIAudioInspectRequest ExactOnDisk;
		ExactOnDisk.Scope = TEXT("on_disk_index");
		ExactOnDisk.ObjectPaths = {TEXT("/Game/__HyperAIStudioAutomation/AbsentExact.AbsentExact")};
		const FHyperAIAudioInspectReport ExactOnDiskReport =
			UHyperAIStudioAudioToolset::hyper_audio_inspect(ExactOnDisk);
		TestFalse(TEXT("exact on-disk object classification requires an async typed index"),
			ExactOnDiskReport.bRevisionComplete);
		TestEqual(TEXT("exact on-disk request does not take the blocking object-index lock"),
			ExactOnDiskReport.SourceObjectsScanned, 0);
		TestTrue(TEXT("exact on-disk blocker is machine readable"),
			ExactOnDiskReport.Issues.ContainsByPredicate([](const FHyperAIAudioIssue& Issue)
			{
				return Issue.Code == TEXT("async_on_disk_class_index_required");
			}));
		FHyperAIAudioInspectRequest Broad;
		const FHyperAIAudioInspectReport BroadReport =
			UHyperAIStudioAudioToolset::hyper_audio_inspect(Broad);
		TestFalse(TEXT("broad scan hard-fails closed as incomplete without cached typed index"),
			BroadReport.bRevisionComplete);
		TestEqual(TEXT("broad request never enumerates a >4096 object universe synchronously"),
			BroadReport.SourceObjectsScanned, 0);
		FHyperAIAudioInspectRequest BroadOnDisk;
		BroadOnDisk.Scope = TEXT("on_disk_index");
		const FHyperAIAudioInspectReport BroadOnDiskReport =
			UHyperAIStudioAudioToolset::hyper_audio_inspect(BroadOnDisk);
		TestFalse(TEXT("broad on-disk request hard-fails closed without cached index"),
			BroadOnDiskReport.bRevisionComplete);
		TestEqual(TEXT("broad on-disk request never enumerates Asset Registry synchronously"),
			BroadOnDiskReport.SourceObjectsScanned, 0);
		ExactCue->MarkAsGarbage();
	}
	if (ExactPackage) ExactPackage->SetDirtyFlag(false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIAudioShadowReplayTest,
	"HyperAIStudio.NativeTools.AudioMetaSound.TypedShadowSemanticReplay",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIAudioShadowReplayTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Audio::Tests;
	const FString MetaTarget = UniqueAssetPath(TEXT("MS_Shadow"));
	FHyperAIAudioApplyPlanRequest Good;
	FHyperAIAudioPlanOperation Create;
	Create.Variant = TEXT("metasound.source.create");
	Create.TargetPath = MetaTarget;
	Good.Operations.Add(Create);
	FHyperAIAudioPlanOperation AddInput;
	AddInput.Variant = TEXT("metasound.add_input");
	AddInput.TargetPath = MetaTarget;
	AddInput.MemberName = TEXT("Gain");
	AddInput.DataType = TEXT("float");
	Good.Operations.Add(AddInput);
	const FHyperAIAudioApplyPlanReport GoodReport = FHyperAIStudioAudioContracts::BuildPlan(Good);
	TestTrue(TEXT("create-then-edit replays against one typed shadow"), GoodReport.bOk);
	FHyperAIAudioApplyPlanRequest DirectionLocalMember = Good;
	FHyperAIAudioPlanOperation SameNameOutput = AddInput;
	SameNameOutput.Variant = TEXT("metasound.add_output");
	DirectionLocalMember.Operations.Add(SameNameOutput);
	TestTrue(TEXT("root input/output names are unique per interface direction"),
		FHyperAIStudioAudioContracts::BuildPlan(DirectionLocalMember).bOk);

	FHyperAIAudioApplyPlanRequest DuplicateMember = Good;
	DuplicateMember.Operations.Add(AddInput);
	TestEqual(TEXT("duplicate member rejected by ordered replay"),
		FHyperAIStudioAudioContracts::BuildPlan(DuplicateMember).Status,
		FString(TEXT("semantic_replay_failed")));
	FHyperAIAudioApplyPlanRequest MissingMember;
	MissingMember.Operations.Add(Create);
	FHyperAIAudioPlanOperation RemoveMember;
	RemoveMember.Variant = TEXT("metasound.remove_member");
	RemoveMember.TargetPath = MetaTarget;
	RemoveMember.MemberName = TEXT("Missing");
	RemoveMember.DataType = TEXT("input");
	MissingMember.Operations.Add(RemoveMember);
	TestEqual(TEXT("missing member removal rejected"),
		FHyperAIStudioAudioContracts::BuildPlan(MissingMember).Status,
		FString(TEXT("semantic_replay_failed")));

	FHyperAIAudioApplyPlanRequest DuplicateNode;
	DuplicateNode.Operations.Add(Create);
	FHyperAIAudioPlanOperation AddNode;
	AddNode.Variant = TEXT("metasound.add_node");
	AddNode.TargetPath = MetaTarget;
	AddNode.NodeId = FGuid::NewGuid();
	AddNode.NodeKind = TEXT("gain");
	AddNode.NodeDescriptorVersion = 1;
	DuplicateNode.Operations.Add(AddNode);
	DuplicateNode.Operations.Add(AddNode);
	TestEqual(TEXT("duplicate node GUID rejected"),
		FHyperAIStudioAudioContracts::BuildPlan(DuplicateNode).Status,
		FString(TEXT("semantic_replay_failed")));

	FHyperAIAudioApplyPlanRequest SameBatchGraph;
	SameBatchGraph.Operations.Add(Create);
	FHyperAIAudioPlanOperation WaveNode = AddNode;
	WaveNode.NodeId = FGuid::NewGuid();
	WaveNode.NodeKind = TEXT("oscillator");
	FHyperAIAudioPlanOperation GainNode = AddNode;
	GainNode.NodeId = FGuid::NewGuid();
	SameBatchGraph.Operations.Add(WaveNode);
	SameBatchGraph.Operations.Add(GainNode);
	FHyperAIAudioPlanOperation ConfigureGain;
	ConfigureGain.Variant = TEXT("metasound.configure_node");
	ConfigureGain.TargetPath = MetaTarget;
	ConfigureGain.NodeId = GainNode.NodeId;
	ConfigureGain.NodeKind = TEXT("gain");
	ConfigureGain.NodeDescriptorVersion = 1;
	FHyperAIAudioParameterValue GainValue;
	GainValue.Name = TEXT("volume");
	GainValue.Type = TEXT("float");
	GainValue.FloatValue = 0.75;
	ConfigureGain.Parameters.Add(GainValue);
	SameBatchGraph.Operations.Add(ConfigureGain);
	FHyperAIAudioPlanOperation SetGainLiteral;
	SetGainLiteral.Variant = TEXT("metasound.set_literal");
	SetGainLiteral.TargetPath = MetaTarget;
	SetGainLiteral.NodeId = GainNode.NodeId;
	SetGainLiteral.VertexName = TEXT("AdditionalOperands");
	FHyperAIAudioParameterValue LiteralValue;
	LiteralValue.Name = TEXT("value");
	LiteralValue.Type = TEXT("float");
	LiteralValue.FloatValue = 0.5;
	SetGainLiteral.Parameters.Add(LiteralValue);
	SameBatchGraph.Operations.Add(SetGainLiteral);
	FHyperAIAudioPlanOperation ConnectByName;
	ConnectByName.Variant = TEXT("metasound.connect");
	ConnectByName.TargetPath = MetaTarget;
	ConnectByName.FromNodeId = WaveNode.NodeId;
	ConnectByName.FromPortName = TEXT("Audio");
	ConnectByName.ToNodeId = GainNode.NodeId;
	ConnectByName.ToPortName = TEXT("PrimaryOperand");
	SameBatchGraph.Operations.Add(ConnectByName);
	const FHyperAIAudioApplyPlanReport SameBatchReport =
		FHyperAIStudioAudioContracts::BuildPlan(SameBatchGraph);
	TestTrue(TEXT("add/configure/literal/connect replays in one typed batch"), SameBatchReport.bOk);
	FHyperAIAudioBackendOperation RegistryBackedGain;
	FString RegistryDescriptorError;
	TestTrue(TEXT("gain resolves through the native registry"),
		FHyperAIStudioAudioContracts::ValidateOperationShape(
			GainNode, RegistryBackedGain, RegistryDescriptorError));
	TestFalse(TEXT("registry key is sealed"), RegistryBackedGain.MetaSoundRegistryKey.IsEmpty());
	TestTrue(TEXT("registry interface fingerprint is sealed"),
		RegistryBackedGain.MetaSoundInterfaceFingerprint.StartsWith(TEXT("sha256:")));
	FHyperAIAudioPlanOperation SecondGainNode = GainNode;
	SecondGainNode.NodeId = FGuid::NewGuid();
	FHyperAIAudioBackendOperation SecondRegistryBackedGain;
	TestTrue(TEXT("second gain resolves through the same native registry class"),
		FHyperAIStudioAudioContracts::ValidateOperationShape(
			SecondGainNode, SecondRegistryBackedGain, RegistryDescriptorError));
	TestEqual(TEXT("registry interface seal is instance-independent"),
		SecondRegistryBackedGain.MetaSoundInterfaceFingerprint,
		RegistryBackedGain.MetaSoundInterfaceFingerprint);
	FMetasoundFrontendVersionNumber GainVersion;
	GainVersion.Major = 1;
	GainVersion.Minor = 1;
	const Metasound::Frontend::FNodeRegistryKey GainRegistryKey(
		EMetasoundFrontendClassType::External,
		FMetasoundFrontendClassName(TEXT("UE"), TEXT("Multiply"), TEXT("Audio by Float")),
		GainVersion);
	TestEqual(TEXT("gain seal uses exact UE registry key"),
		RegistryBackedGain.MetaSoundRegistryKey, GainRegistryKey.ToString());
	FMetasoundFrontendClass RegisteredGainClass;
	Metasound::Frontend::INodeClassRegistry* NodeRegistry =
		Metasound::Frontend::INodeClassRegistry::Get();
	TestTrue(TEXT("gain registry class can be copied"), NodeRegistry
		&& NodeRegistry->FindFrontendClassFromRegistered(GainRegistryKey, RegisteredGainClass));
	if (NodeRegistry)
	{
		for (const FMetasoundFrontendClassInput& RegistryInput :
			RegisteredGainClass.GetDefaultInterface().Inputs)
		{
			const FHyperAIAudioBackendMetaPort* BoundInput =
				RegistryBackedGain.DescriptorInputs.FindByPredicate(
					[&](const FHyperAIAudioBackendMetaPort& Port)
					{ return Port.Name == RegistryInput.Name.ToString(); });
			TestTrue(TEXT("every gain input comes from registry interface"),
				BoundInput && BoundInput->VertexId == RegistryInput.VertexID
					&& BoundInput->TypeName == RegistryInput.TypeName);
		}
		for (const FMetasoundFrontendClassOutput& RegistryOutput :
			RegisteredGainClass.GetDefaultInterface().Outputs)
		{
			const FHyperAIAudioBackendMetaPort* BoundOutput =
				RegistryBackedGain.DescriptorOutputs.FindByPredicate(
					[&](const FHyperAIAudioBackendMetaPort& Port)
					{ return Port.Name == RegistryOutput.Name.ToString(); });
			TestTrue(TEXT("every gain output comes from registry interface"),
				BoundOutput && BoundOutput->VertexId == RegistryOutput.VertexID
					&& BoundOutput->TypeName == RegistryOutput.TypeName);
		}
	}
	FHyperAIAudioPlanOperation UnprovenNode = AddNode;
	UnprovenNode.NodeId = FGuid::NewGuid();
	UnprovenNode.NodeKind = TEXT("wave_player");
	FHyperAIAudioBackendOperation RejectedUnproven;
	TestFalse(TEXT("unproven semantic node kind fails closed"),
		FHyperAIStudioAudioContracts::ValidateOperationShape(
			UnprovenNode, RejectedUnproven, RegistryDescriptorError));

	FHyperAIAudioApplyPlanRequest MissingEdge;
	MissingEdge.Operations.Add(Create);
	FHyperAIAudioPlanOperation Connect;
	Connect.Variant = TEXT("metasound.connect");
	Connect.TargetPath = MetaTarget;
	Connect.FromNodeId = FGuid::NewGuid();
	Connect.FromVertexId = FGuid::NewGuid();
	Connect.ToNodeId = FGuid::NewGuid();
	Connect.ToVertexId = FGuid::NewGuid();
	MissingEdge.Operations.Add(Connect);
	TestEqual(TEXT("missing edge endpoints rejected"),
		FHyperAIStudioAudioContracts::BuildPlan(MissingEdge).Status,
		FString(TEXT("semantic_replay_failed")));

	const FString CrossTarget = UniqueAssetPath(TEXT("CrossFamily"));
	FHyperAIAudioApplyPlanRequest CrossFamily;
	CrossFamily.Operations.Add(SoundCueCreate(CrossTarget));
	FHyperAIAudioPlanOperation WrongClass;
	WrongClass.Variant = TEXT("sound_class.configure");
	WrongClass.TargetPath = CrossTarget;
	CrossFamily.Operations.Add(WrongClass);
	TestEqual(TEXT("cross-family edit rejected"),
		FHyperAIStudioAudioContracts::BuildPlan(CrossFamily).Status,
		FString(TEXT("semantic_replay_failed")));

	const FString ClassA = UniqueAssetPath(TEXT("SC_CycleA"));
	const FString ClassB = UniqueAssetPath(TEXT("SC_CycleB"));
	FHyperAIAudioApplyPlanRequest Cycle;
	FHyperAIAudioPlanOperation CreateA;
	CreateA.Variant = TEXT("sound_class.create");
	CreateA.TargetPath = ClassA;
	FHyperAIAudioPlanOperation CreateB = CreateA;
	CreateB.TargetPath = ClassB;
	Cycle.Operations = {CreateA, CreateB};
	FHyperAIAudioPlanOperation ParentA;
	ParentA.Variant = TEXT("sound_class.set_parent");
	ParentA.TargetPath = ClassA;
	ParentA.ReferencePath = ClassB;
	FHyperAIAudioPlanOperation ParentB = ParentA;
	ParentB.TargetPath = ClassB;
	ParentB.ReferencePath = ClassA;
	Cycle.Operations.Add(ParentA);
	Cycle.Operations.Add(ParentB);
	TestEqual(TEXT("cross-target reference cycle rejected"),
		FHyperAIStudioAudioContracts::BuildPlan(Cycle).Status,
		FString(TEXT("semantic_replay_failed")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIAudioCoupledAndSparseRegressionTest,
	"HyperAIStudio.NativeTools.AudioMetaSound.CoupledFinalStateAndSparseBounds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIAudioCoupledAndSparseRegressionTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Audio::Tests;
	TestTrue(TEXT("create CAS admits exact DoesNotExist"),
		FHyperAIStudioAudioContracts::IsCreateExistenceStateAdmittedForTest(
			static_cast<int32>(UE::AssetRegistry::EExists::DoesNotExist), true));
	TestFalse(TEXT("create CAS rejects Exists even with registry available"),
		FHyperAIStudioAudioContracts::IsCreateExistenceStateAdmittedForTest(
			static_cast<int32>(UE::AssetRegistry::EExists::Exists), true));
	TestFalse(TEXT("create CAS rejects Unknown while registry is indexing"),
		FHyperAIStudioAudioContracts::IsCreateExistenceStateAdmittedForTest(
			static_cast<int32>(UE::AssetRegistry::EExists::Unknown), true));
	TestFalse(TEXT("create CAS rejects unavailable registry"),
		FHyperAIStudioAudioContracts::IsCreateExistenceStateAdmittedForTest(
			static_cast<int32>(UE::AssetRegistry::EExists::DoesNotExist), false));
	TestFalse(TEXT("create CAS rejects invalid synthetic state"),
		FHyperAIStudioAudioContracts::IsCreateExistenceStateAdmittedForTest(99, true));
	const FString LoadedPackageTarget = UniqueAssetPath(TEXT("LoadedPackageCreate"));
	UPackage* LoadedContainingPackage = CreatePackage(
		*FPackageName::ObjectPathToPackageName(LoadedPackageTarget));
	FHyperAIAudioApplyPlanRequest LoadedPackageCreate;
	LoadedPackageCreate.Operations.Add(SoundCueCreate(LoadedPackageTarget));
	TestEqual(TEXT("create CAS rejects a loaded containing package even when the exact object is absent"),
		FHyperAIStudioAudioContracts::BuildPlan(LoadedPackageCreate).Status,
		FString(TEXT("cas_failed")));
	if (LoadedContainingPackage)
	{
		LoadedContainingPackage->SetDirtyFlag(false);
		LoadedContainingPackage->MarkAsGarbage();
	}
	TestTrue(TEXT("ordinary sparse shape admitted"),
		FHyperAIStudioAudioContracts::IsSparseContainerShapeBoundedForTest(3, 16));
	TestFalse(TEXT("sparse internal-slot cap cannot be bypassed by small Num"),
		FHyperAIStudioAudioContracts::IsSparseContainerShapeBoundedForTest(1, 32769));
	TestFalse(TEXT("sparse shape cannot claim more entries than slots"),
		FHyperAIStudioAudioContracts::IsSparseContainerShapeBoundedForTest(4, 3));

	FString Error;
	TMap<FString, double> Modulation = {
		{TEXT("min_value"), -1.0}, {TEXT("default_value"), 0.0}, {TEXT("max_value"), 1.0}};
	TestTrue(TEXT("complete min/default/max tuple accepted"),
		FHyperAIStudioAudioContracts::ValidateCoupledTupleForTest(
			TEXT("modulation.parameter"), Modulation, {TEXT("default_value")}, Error));
	Modulation[TEXT("default_value")] = 2.0;
	TestFalse(TEXT("final min/default/max ordering enforced"),
		FHyperAIStudioAudioContracts::ValidateCoupledTupleForTest(
			TEXT("modulation.parameter"), Modulation, {TEXT("default_value")}, Error));
	TestFalse(TEXT("touched partial coupled tuple fails closed"),
		FHyperAIStudioAudioContracts::ValidateCoupledTupleForTest(
			TEXT("modulation.parameter"), {{TEXT("min_value"), 0.0}},
			{TEXT("min_value")}, Error));
	TestFalse(TEXT("untouched partial coupled tuple also fails final-state validation"),
		FHyperAIStudioAudioContracts::ValidateCoupledTupleForTest(
			TEXT("modulation.parameter"), {{TEXT("min_value"), 0.0}}, {}, Error));
	TestFalse(TEXT("idle RPM cannot exceed max RPM"),
		FHyperAIStudioAudioContracts::ValidateCoupledTupleForTest(
			TEXT("motor_sim"), {{TEXT("idle_rpm"), 8000.0}, {TEXT("max_rpm"), 7000.0}},
			{TEXT("idle_rpm"), TEXT("max_rpm")}, Error));

	auto FloatParameter = [](const FString& Name, const double Value)
	{
		FHyperAIAudioParameterValue Parameter;
		Parameter.Name = Name;
		Parameter.Type = TEXT("float");
		Parameter.FloatValue = Value;
		return Parameter;
	};
	const FString MixTarget = UniqueAssetPath(TEXT("Mix_Coupled"));
	FHyperAIAudioApplyPlanRequest ValidMix;
	FHyperAIAudioPlanOperation CreateMix;
	CreateMix.Variant = TEXT("sound_mix.create");
	CreateMix.TargetPath = MixTarget;
	ValidMix.Operations.Add(CreateMix);
	for (const TPair<FString, double>& Pair : TArray<TPair<FString, double>>{
		{TEXT("fade_in_time"), 1.25}, {TEXT("duration"), 2.0}, {TEXT("fade_out_time"), 0.75}})
	{
		FHyperAIAudioPlanOperation ConfigureMix;
		ConfigureMix.Variant = TEXT("sound_mix.configure");
		ConfigureMix.TargetPath = MixTarget;
		ConfigureMix.Parameters.Add(FloatParameter(Pair.Key, Pair.Value));
		ValidMix.Operations.Add(MoveTemp(ConfigureMix));
	}
	TestTrue(TEXT("coupled mix tuple is judged only after full ordered replay"),
		FHyperAIStudioAudioContracts::BuildPlan(ValidMix).bOk);
	FHyperAIAudioApplyPlanRequest InvalidMix = ValidMix;
	InvalidMix.Operations.Last().Parameters[0].FloatValue = 0.8;
	TestEqual(TEXT("invalid final mix tuple rejected after replay"),
		FHyperAIStudioAudioContracts::BuildPlan(InvalidMix).Status,
		FString(TEXT("coupled_invariant_failed")));

	FHyperAIAudioApplyPlanRequest InvalidAttenuation;
	FHyperAIAudioPlanOperation CreateAttenuation;
	CreateAttenuation.Variant = TEXT("sound_attenuation.create");
	CreateAttenuation.TargetPath = UniqueAssetPath(TEXT("Att_Coupled"));
	CreateAttenuation.Parameters = {
		FloatParameter(TEXT("inner_radius"), static_cast<double>(WORLD_MAX) * 0.75),
		FloatParameter(TEXT("falloff_distance"), static_cast<double>(WORLD_MAX) * 0.75)};
	InvalidAttenuation.Operations.Add(MoveTemp(CreateAttenuation));
	TestEqual(TEXT("oversized attenuation scalars fail the closed per-parameter bound first"),
		FHyperAIStudioAudioContracts::BuildPlan(InvalidAttenuation).Status,
		FString(TEXT("invalid_operation")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
