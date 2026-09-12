// Games by Hyper 2026.

#include "HyperAIStudioCinematicsToolset.h"

#include "HyperAIStudioExtensionRuntime.h"
#include "Misc/AutomationTest.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace HyperAIStudio::Cinematics::Tests
{
	FString Hash(const FString& Value)
	{
		return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Value);
	}

	void SealSequence(FHyperAIStudioCinematicsValueSnapshot& Value)
	{
		Value.Sequence.BindingCount = Value.Bindings.Num();
		Value.Sequence.TrackCount = Value.Tracks.Num();
		Value.Sequence.SectionCount = Value.Sections.Num();
		Value.Sequence.KeyCount = Value.Keys.Num();
		Value.Sequence.CameraCutCount = Value.Cameras.Num();
		Value.Sequence.EventCount = Value.Events.Num();
		Value.Sequence.PersistedRevision =
			FHyperAIStudioCinematicsContracts::ComputePersistedRevisionForEvidence(
				Value.Sequence.PackageName, Value.Sequence.DiskExistence,
				Value.Sequence.PackageSavedHash, Value.Sequence.DiskSize);
		Value.Sequence.bPersistedRevisionComplete = true;
		Value.Sequence.VolatileRevision =
			FHyperAIStudioCinematicsContracts::ComputeSequenceVolatileRevisionForValues(Value);
		Value.Sequence.bVolatileRevisionComplete = true;
	}

	FHyperAIStudioCinematicsValueSnapshot StructuralSnapshot()
	{
		FHyperAIStudioCinematicsValueSnapshot Value;
		Value.Sequence.TargetPath = TEXT("/Game/Cinematics/LS_Test.LS_Test");
		Value.Sequence.PackageName = TEXT("/Game/Cinematics/LS_Test");
		Value.Sequence.ClassName = TEXT("LevelSequence");
		Value.Sequence.bLoaded = true;
		Value.Sequence.bWasLoadedFromDisk = true;
		Value.Sequence.DiskExistence = TEXT("exists");
		Value.Sequence.PackageSavedHash =
			TEXT("0123456789012345678901234567890123456789");
		Value.Sequence.DiskSize = 4096;
		Value.Sequence.TickResolutionNumerator = 24000;
		Value.Sequence.TickResolutionDenominator = 1;
		Value.Sequence.DisplayRateNumerator = 24;
		Value.Sequence.DisplayRateDenominator = 1;
		Value.Sequence.PlaybackRange.bHasLowerBound = true;
		Value.Sequence.PlaybackRange.bLowerInclusive = true;
		Value.Sequence.PlaybackRange.LowerFrame = 0;
		Value.Sequence.PlaybackRange.bHasUpperBound = true;
		Value.Sequence.PlaybackRange.UpperFrame = 100;
		SealSequence(Value);
		Value.bComplete = true;
		Value.SnapshotFingerprint =
			FHyperAIStudioCinematicsContracts::ComputeSnapshotFingerprint(Value);
		return Value;
	}

	FHyperAIStudioCinematicsValueSnapshot RenderReadySnapshot()
	{
		FHyperAIStudioCinematicsValueSnapshot Value = StructuralSnapshot();
		FHyperAICinematicsTrackRecord Track;
		Track.TrackId = TEXT("track:0000");
		Track.ClassName = TEXT("MovieSceneCameraCutTrack");
		Track.bRootTrack = true;
		Track.bCameraCutTrack = true;
		Track.bDetailProjectionComplete = true;
		Track.DetailFingerprint = Hash(TEXT("camera-track-detail"));
		Track.SectionCount = 1;
		Value.Tracks.Add(Track);
		FHyperAICinematicsSectionRecord Section;
		Section.SectionId = TEXT("section:0000");
		Section.TrackId = Track.TrackId;
		Section.ClassName = TEXT("MovieSceneCameraCutSection");
		Section.Range.bHasLowerBound = true;
		Section.Range.bLowerInclusive = true;
		Section.Range.LowerFrame = 0;
		Section.Range.bHasUpperBound = true;
		Section.Range.UpperFrame = 100;
		Section.bActive = true;
		Section.bDetailProjectionComplete = true;
		Section.DetailFingerprint = Hash(TEXT("camera-section-detail"));
		Section.bChannelProjectionSupported = true;
		Section.ChannelFingerprint = Hash(TEXT("camera-section-binding"));
		Value.Sections.Add(Section);
		FHyperAICinematicsCameraRecord Camera;
		Camera.CameraCutId = TEXT("camera:0000");
		Camera.SectionId = Section.SectionId;
		Camera.CameraBindingId = TEXT("11111111-2222-3333-4444-555555555555");
		Camera.BindingFingerprint = Section.ChannelFingerprint;
		Camera.Range = Section.Range;
		Value.Cameras.Add(Camera);

		Value.RenderConfig.bPresent = true;
		Value.RenderConfig.TargetPath = TEXT("/Game/Cinematics/MRQ_Test.MRQ_Test");
		Value.RenderConfig.PackageName = TEXT("/Game/Cinematics/MRQ_Test");
		Value.RenderConfig.bWasLoadedFromDisk = true;
		Value.RenderConfig.DiskExistence = TEXT("exists");
		Value.RenderConfig.PackageSavedHash =
			TEXT("1123456789012345678901234567890123456789");
		Value.RenderConfig.DiskSize = 2048;
		Value.RenderConfig.SettingsSerialNumber = 7;
		Value.RenderConfig.SettingCount = 1;
		Value.RenderConfig.bOutputSettingPresent = true;
		Value.RenderConfig.OutputSettingFingerprint = Hash(TEXT("mrq-output-values"));
		Value.RenderConfig.OutputDirectory = TEXT("{project_dir}/Saved/MovieRenders");
		Value.RenderConfig.FileNameFormat = TEXT("{sequence_name}.{frame_number}");
		Value.RenderConfig.OutputWidth = 1920;
		Value.RenderConfig.OutputHeight = 1080;
		Value.RenderConfig.OutputRateNumerator = 24;
		Value.RenderConfig.OutputRateDenominator = 1;
		FHyperAICinematicsMRQSettingRecord Setting;
		Setting.SettingId = TEXT("mrq-setting:000");
		Setting.ClassName = TEXT("MoviePipelineOutputSetting");
		Setting.bEnabled = true;
		Setting.bDetailProjectionComplete = true;
		Setting.DetailFingerprint = Value.RenderConfig.OutputSettingFingerprint;
		Value.RenderSettings.Add(Setting);
		Value.RenderConfig.PersistedRevision =
			FHyperAIStudioCinematicsContracts::ComputePersistedRevisionForEvidence(
				Value.RenderConfig.PackageName, Value.RenderConfig.DiskExistence,
				Value.RenderConfig.PackageSavedHash, Value.RenderConfig.DiskSize);
		Value.RenderConfig.bPersistedRevisionComplete = true;
		SealSequence(Value);
		Value.RenderConfig.VolatileRevision =
			FHyperAIStudioCinematicsContracts::ComputeRenderConfigVolatileRevisionForValues(Value);
		Value.RenderConfig.bVolatileRevisionComplete = true;
		Value.bComplete = true;
		Value.SnapshotFingerprint =
			FHyperAIStudioCinematicsContracts::ComputeSnapshotFingerprint(Value);
		return Value;
	}

	FHyperAICinematicsValidateRequest ValidationRequest(
		const FHyperAIStudioCinematicsValueSnapshot& Value,
		const FString& Policy)
	{
		FHyperAICinematicsValidateRequest Request;
		Request.SequencePath = Value.Sequence.TargetPath;
		Request.RenderConfigPath = Value.RenderConfig.TargetPath;
		Request.ExpectedPersistedRevision = Value.Sequence.PersistedRevision;
		Request.ExpectedVolatileRevision = Value.Sequence.VolatileRevision;
		Request.ExpectedRenderConfigPersistedRevision =
			Value.RenderConfig.PersistedRevision;
		Request.ExpectedRenderConfigVolatileRevision =
			Value.RenderConfig.VolatileRevision;
		Request.Policy = Policy;
		Request.MaxIssues = FHyperAIStudioCinematicsContracts::MaxIssues;
		return Request;
	}

	TSharedRef<FHyperAIStudioCinematicsRenderPayload, ESPMode::ThreadSafe> RenderPayload()
	{
		const TSharedRef<FHyperAIStudioCinematicsRenderPayload, ESPMode::ThreadSafe> Value =
			MakeShared<FHyperAIStudioCinematicsRenderPayload, ESPMode::ThreadSafe>();
		Value->SequencePath = TEXT("/Game/Cinematics/LS_Test.LS_Test");
		Value->RenderConfigPath = TEXT("/Game/Cinematics/MRQ_Test.MRQ_Test");
		Value->BasePersistedRevision = Hash(TEXT("sequence-disk"));
		Value->BaseVolatileRevision = Hash(TEXT("sequence-loaded"));
		Value->BaseRenderConfigPersistedRevision = Hash(TEXT("mrq-disk"));
		Value->BaseRenderConfigVolatileRevision = Hash(TEXT("mrq-loaded"));
		Value->OutputSettingFingerprint = Hash(TEXT("mrq-output"));
		Value->Lifecycle = TEXT("render_validated_sequence");
		Value->ExpectedTerminalState = TEXT("render_outputs_verified");
		Value->AsyncDeadlineMs = 60000;
		Value->SemanticFingerprint =
			FHyperAIStudioCinematicsContracts::ComputeRenderSemanticFingerprint(*Value);
		return Value;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioCinematicsManifestAuthorityTest,
	"HyperAIStudio.NativeTools.Cinematics.ManifestAuthorityAndUnsupportedMatrix",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioCinematicsManifestAuthorityTest::RunTest(const FString& Parameters)
{
	const auto& Manifest = FHyperAIStudioCinematicsContracts::GetManifest();
	TestEqual(TEXT("pack id"), FString(FHyperAIStudioCinematicsContracts::PackId),
		FString(TEXT("cinematics")));
	TestEqual(TEXT("Editor module"), FString(FHyperAIStudioCinematicsContracts::RequiredModuleType),
		FString(TEXT("Editor")));
	TestEqual(TEXT("LoadingPhase None"), FString(FHyperAIStudioCinematicsContracts::RequiredLoadingPhase),
		FString(TEXT("None")));
	TestEqual(TEXT("exact cohort"), FString(FHyperAIStudioCinematicsContracts::AtomicCohortId),
		FString(TEXT("cohort.source.hyperaistudiocinematicstoolset.v1")));
	TestEqual(TEXT("exact qualifier"), FHyperAIStudioCinematicsContracts::GetQualifiedToolsetName(),
		FString(TEXT("HyperAIStudioCinematics.HyperAIStudioCinematicsToolset")));
	TestEqual(TEXT("exact three callables"), Manifest.Num(), 3);
	TestEqual(TEXT("inspect"), Manifest[0].Name, FString(TEXT("hyper_cinematics_inspect")));
	TestEqual(TEXT("apply"), Manifest[1].Name, FString(TEXT("hyper_cinematics_apply_plan")));
	TestEqual(TEXT("validate"), Manifest[2].Name, FString(TEXT("hyper_cinematics_validate")));
	for (const FHyperAIStudioCinematicsManifestEntry& Entry : Manifest)
	{
		TestEqual(TEXT("one atomic owner"), Entry.QualifiedToolset,
			FHyperAIStudioCinematicsContracts::GetQualifiedToolsetName());
	}
	const auto& Authority = FHyperAIStudioCinematicsContracts::GetDelegationAuthority();
	TestEqual(TEXT("exact Epic delegation plus UE 5.8 API/source matrix"), Authority.Num(), 59);
	int32 DelegatedCallables = 0;
	int32 NativePublicSources = 0;
	int32 NativeImplementations = 0;
	int32 SequencerDelegationSources = 0;
	TSet<FString> Groups;
	for (const FHyperAICinematicsDelegationAuthority& Row : Authority)
	{
		TestFalse(TEXT("source group unique"), Groups.Contains(Row.SourceGroup));
		Groups.Add(Row.SourceGroup);
		TestTrue(TEXT("reviewed source has canonical hash"),
			FHyperAIStudioCinematicsContracts::IsCanonicalSha256(Row.ReviewedFingerprint));
		if (Row.AuthorityKind == TEXT("epic_python_group"))
		{
			DelegatedCallables += Row.CallableCount;
			TestEqual(TEXT("Epic authoring stays delegated"), Row.Disposition,
				FString(TEXT("epic_delegate")));
		}
		else if (Row.AuthorityKind == TEXT("ue58_public_api"))
		{
			++NativePublicSources;
			TestEqual(TEXT("native surface is observation-only"), Row.Disposition,
				FString(TEXT("native_observation_authority")));
		}
		else if (Row.AuthorityKind == TEXT("ue58_source_implementation"))
		{
			++NativeImplementations;
			TestEqual(TEXT("native implementation grounds observation only"),
				Row.Disposition, FString(TEXT("native_observation_authority")));
		}
		else
		{
			++SequencerDelegationSources;
			TestTrue(TEXT("only exact Sequencer delegation source kinds are admitted"),
				Row.AuthorityKind == TEXT("ue58_delegation_api")
				|| Row.AuthorityKind == TEXT("ue58_delegation_implementation"));
			TestEqual(TEXT("Sequencer editor authoring remains Epic-delegated"),
				Row.Disposition, FString(TEXT("epic_delegate")));
		}
	}
	TestEqual(TEXT("all reviewed Sequencer/AnimMixer callables delegated"),
		DelegatedCallables, 296);
	TestEqual(TEXT("all reviewed UE 5.8 observation headers frozen"), NativePublicSources, 25);
	TestEqual(TEXT("all reviewed UE 5.8 observation implementations frozen"),
		NativeImplementations, 24);
	TestEqual(TEXT("Sequencer interface and implementation delegation frozen"),
		SequencerDelegationSources, 2);
	const TArray<int32> ExactEpicCounts = {9, 72, 8, 6, 22, 18, 140, 21};
	const TArray<FString> ExactEpicFingerprints = {
		TEXT("sha256:f178ed9f5124751af8ff4eb8353bab65f5c3c009845181a86fcb11d1370dc816"),
		TEXT("sha256:fd3520b8e8ce7b7dcf81fd4ec757da8fb21ab5c35295388e79673f10e8d50521"),
		TEXT("sha256:7f902c1205d67ccbf122fa366e1b377f28d05065ea3a6e1570352241574be6ae"),
		TEXT("sha256:31642ee2536690fcd933fbf77cd62b9581cb7875b80349d0a9ce9552f12563cd"),
		TEXT("sha256:74fc571ed14c4f86f96b7227ef57eff78fce44c0b4e0d64089f067989c34b3af"),
		TEXT("sha256:51366ff2db474133d8fa4ae0f62dff2f41930afb7d8b746c76ec07dd4fefb37d"),
		TEXT("sha256:75b33a624997e75e58c0374bc7093cff74351805dc1f8a383b84879f4d986c85"),
		TEXT("sha256:75a43e108740754ddc6eda42fe90938d4bfd6386e2d28da543dda6802e19ab03")};
	for (int32 Index = 0; Index < ExactEpicCounts.Num(); ++Index)
	{
		TestEqual(TEXT("exact Epic group callable count"),
			Authority[Index].CallableCount, ExactEpicCounts[Index]);
		TestEqual(TEXT("exact Epic group reviewed fingerprint"),
			Authority[Index].ReviewedFingerprint, ExactEpicFingerprints[Index]);
	}
	TestEqual(TEXT("LevelSequence source hash frozen"), Authority[8].ReviewedFingerprint,
		FString(TEXT("sha256:dfe3cdf6777029f40819fc70cb50fa9194d99c0429e5f4c3e34164c4ed37242b")));
	TestEqual(TEXT("MRQ OutputSetting public source hash frozen"), Authority[27].ReviewedFingerprint,
		FString(TEXT("sha256:f7f9990b89efdad2c2f6554807dd91fa82cf9116a35daff6a2f330984aca1fa1")));
	TestEqual(TEXT("MRQ setting implementation source hash frozen"),
		Authority.Last().ReviewedFingerprint,
		FString(TEXT("sha256:d9358716b8e21ce952c41ea3e82553c688faa9f019cf8bc2e038765322116321")));
	const auto Matrix = FHyperAIStudioCinematicsContracts::GetCapabilityMatrix();
	TestEqual(TEXT("one truthful capability row"), Matrix.Num(), 1);
	TestTrue(TEXT("MovieRenderPipeline is hard linked"), Matrix[0].bMovieRenderPipelineHardLinked);
	TestFalse(TEXT("mutation unavailable"), Matrix[0].bMutationExecutionImplemented);
	TestFalse(TEXT("render submission unavailable"), Matrix[0].bRenderSubmissionImplemented);
	TestEqual(TEXT("compact report freezes all authority rows"),
		Matrix[0].DelegationAuthorityCount, 59);
	TestEqual(TEXT("compact report freezes delegated callable count"),
		Matrix[0].DelegatedEpicCallableCount, 296);
	TestTrue(TEXT("compact authority seal canonical"),
		FHyperAIStudioCinematicsContracts::IsCanonicalSha256(
			Matrix[0].DelegationAuthorityFingerprint));
	TestTrue(TEXT("full authority matrix is not repeated in bounded reports"),
		Matrix[0].DelegationAuthority.IsEmpty());
	TestEqual(TEXT("stable execution blocker"), Matrix[0].State,
		FString(TEXT("source_candidate_async_continuation_host_required")));
	const FHyperAIStudioDomainAdapterDescriptor& Descriptor =
		FHyperAIStudioCinematicsContracts::GetAdapterDescriptor();
	TestEqual(TEXT("exact three adapter variants"), Descriptor.Variants.Num(), 3);
	TestEqual(TEXT("apply remains external effect"),
		static_cast<uint8>(Descriptor.Variants[1].Safety),
		static_cast<uint8>(EHyperAIStudioDomainSafety::ExternalEffect));
	TestTrue(TEXT("hard requirements are not nonblocking groups"),
		Descriptor.ApplicableNonBlockingRequirementGroupIds.IsEmpty());
	TestFalse(TEXT("production source cohort is catalog fail-closed"),
		FHyperAIStudioCinematicsContracts::IsRegistrationAllowed(false));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioCinematicsBoundsNoLoadTest,
	"HyperAIStudio.NativeTools.Cinematics.BoundsTriStateAndNoLoad",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioCinematicsBoundsNoLoadTest::RunTest(const FString& Parameters)
{
	FString Error;
	TestTrue(TEXT("closed maxima admitted"),
		FHyperAIStudioCinematicsContracts::AdmitCountsBeforeProjection(
			64, 256, 1024, 2048, 4096, 512, 512, 64, Error));
	TestFalse(TEXT("binding overflow rejected before copy"),
		FHyperAIStudioCinematicsContracts::AdmitCountsBeforeProjection(
			65, 0, 0, 0, 0, 0, 0, 0, Error));
	TestEqual(TEXT("stable overflow reason"), Error, FString(TEXT("binding_count_exceeded")));
	TestFalse(TEXT("negative key count rejected"),
		FHyperAIStudioCinematicsContracts::AdmitCountsBeforeProjection(
			0, 0, 0, 0, -1, 0, 0, 0, Error));
	TestEqual(TEXT("unknown is never absence"),
		FHyperAIStudioCinematicsContracts::ClassifyAssetRegistryExistence(
			UE::AssetRegistry::EExists::Unknown), FString(TEXT("unknown")));
	TestEqual(TEXT("absence stays explicit"),
		FHyperAIStudioCinematicsContracts::ClassifyAssetRegistryExistence(
			UE::AssetRegistry::EExists::DoesNotExist), FString(TEXT("does_not_exist")));
	TestTrue(TEXT("canonical top-level path"),
		FHyperAIStudioCinematicsContracts::IsCanonicalPrimaryAssetPath(
			TEXT("/Game/Cinematics/LS_Test.LS_Test")));
	TestFalse(TEXT("wildcard search is not an exact path"),
		FHyperAIStudioCinematicsContracts::IsCanonicalPrimaryAssetPath(
			TEXT("/Game/Cinematics/*.LevelSequence")));
	TestFalse(TEXT("subobject is not a primary object"),
		FHyperAIStudioCinematicsContracts::IsCanonicalPrimaryAssetPath(
			TEXT("/Game/Cinematics/LS_Test.LS_Test:MovieScene")));
	TestNull(TEXT("ResolveObject never loads missing sequence"),
		FSoftObjectPath(TEXT("/Game/Cinematics/DefinitelyNotLoaded_CineAudit.DefinitelyNotLoaded_CineAudit"))
			.ResolveObject());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioCinematicsDetachedValidatorTest,
	"HyperAIStudio.NativeTools.Cinematics.SeparateCASAndIndependentDetachedValidator",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioCinematicsDetachedValidatorTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Cinematics::Tests;
	FHyperAIStudioCinematicsValueSnapshot Structural = StructuralSnapshot();
	FHyperAICinematicsValidateRequest StructuralRequest =
		ValidationRequest(Structural, TEXT("structural"));
	FHyperAICinematicsValidateReport Report =
		FHyperAIStudioCinematicsContracts::ValidateSnapshot(Structural, StructuralRequest);
	TestTrue(TEXT("structural fixture validates"), Report.bValid);
	TestTrue(TEXT("detached validator completes"), Report.bComplete);
	TestTrue(TEXT("validator result sealed"),
		FHyperAIStudioCinematicsContracts::IsCanonicalSha256(Report.ValidatorFingerprint));
	TestEqual(TEXT("no structural errors"), Report.ErrorCount, 0);

	FHyperAIStudioCinematicsValueSnapshot Render = RenderReadySnapshot();
	FHyperAICinematicsValidateRequest RenderRequest =
		ValidationRequest(Render, TEXT("render_ready"));
	Report = FHyperAIStudioCinematicsContracts::ValidateSnapshot(Render, RenderRequest);
	TestTrue(TEXT("closed MRQ fixture is render-ready"), Report.bValid);
	TestNotEqual(TEXT("sequence disk and loaded CAS are independent"),
		Render.Sequence.PersistedRevision, Render.Sequence.VolatileRevision);
	TestNotEqual(TEXT("MRQ disk and loaded CAS are independent"),
		Render.RenderConfig.PersistedRevision, Render.RenderConfig.VolatileRevision);
	FHyperAIStudioCinematicsValueSnapshot KeyValueA = Render;
	FHyperAICinematicsKeyRecord Key;
	Key.KeyId = TEXT("key:000000");
	Key.SectionId = KeyValueA.Sections[0].SectionId;
	Key.ChannelType = TEXT("MovieSceneDoubleChannel");
	Key.Frame = 12;
	Key.ValueFingerprint = Hash(TEXT("double-value-a"));
	KeyValueA.Keys.Add(Key);
	KeyValueA.Sections[0].ChannelCount = 1;
	KeyValueA.Sections[0].KeyCount = 1;
	KeyValueA.Sections[0].ChannelFingerprint = Hash(TEXT("channel-value-a"));
	KeyValueA.Sequence.ChannelCount = 1;
	SealSequence(KeyValueA);
	const FString KeyValueARevision = KeyValueA.Sequence.VolatileRevision;
	FHyperAIStudioCinematicsValueSnapshot KeyValueB = KeyValueA;
	KeyValueB.Keys[0].ValueFingerprint = Hash(TEXT("double-value-b"));
	KeyValueB.Sections[0].ChannelFingerprint = Hash(TEXT("channel-value-b"));
	SealSequence(KeyValueB);
	TestNotEqual(TEXT("same key time with changed value changes loaded CAS"),
		KeyValueARevision, KeyValueB.Sequence.VolatileRevision);

	FHyperAICinematicsValidateRequest Stale = RenderRequest;
	Stale.ExpectedVolatileRevision = Hash(TEXT("stale-loaded-sequence"));
	Report = FHyperAIStudioCinematicsContracts::ValidateSnapshot(Render, Stale);
	TestFalse(TEXT("stale loaded sequence CAS rejected"), Report.bValid);
	TestTrue(TEXT("stale loaded CAS has exact issue"), Report.Issues.ContainsByPredicate(
		[](const FHyperAICinematicsIssue& Issue)
		{
			return Issue.Code == TEXT("stale_sequence_volatile_revision");
		}));
	FHyperAIStudioCinematicsValueSnapshot Drift = Render;
	Drift.Sequence.DisplayRateNumerator = 25;
	Report = FHyperAIStudioCinematicsContracts::ValidateSnapshot(Drift, RenderRequest);
	TestFalse(TEXT("same counts with value drift rejected"), Report.bValid);
	TestTrue(TEXT("volatile seal is recomputed from values"), Report.Issues.ContainsByPredicate(
		[](const FHyperAICinematicsIssue& Issue)
		{
			return Issue.Code == TEXT("sequence_volatile_revision_incomplete");
		}));
	FHyperAIStudioCinematicsValueSnapshot OutputDrift = Render;
	OutputDrift.RenderConfig.OutputWidth = 1280;
	Report = FHyperAIStudioCinematicsContracts::ValidateSnapshot(OutputDrift, RenderRequest);
	TestFalse(TEXT("MRQ output summary drift is rejected"), Report.bValid);
	TestTrue(TEXT("MRQ output values are part of volatile CAS"),
		Report.Issues.ContainsByPredicate([](const FHyperAICinematicsIssue& Issue)
		{
			return Issue.Code == TEXT("render_config_volatile_revision_incomplete");
		}));
	FHyperAIStudioCinematicsValueSnapshot Unknown = Structural;
	Unknown.Sequence.DiskExistence = TEXT("unknown");
	Unknown.Sequence.bPersistedRevisionComplete = false;
	Unknown.bComplete = false;
	Unknown.SnapshotFingerprint =
		FHyperAIStudioCinematicsContracts::ComputeSnapshotFingerprint(Unknown);
	Report = FHyperAIStudioCinematicsContracts::ValidateSnapshot(
		Unknown, ValidationRequest(Unknown, TEXT("structural")));
	TestFalse(TEXT("unknown package evidence never passes"), Report.bValid);
	TestFalse(TEXT("unknown evidence remains incomplete"), Report.bComplete);
	FHyperAICinematicsValidateRequest TinyIssueEnvelope = RenderRequest;
	TinyIssueEnvelope.MaxIssues = 1;
	FHyperAIStudioCinematicsValueSnapshot ManyErrors = Render;
	ManyErrors.Sequence.TickResolutionNumerator = 0;
	ManyErrors.Sequence.DisplayRateNumerator = 0;
	ManyErrors.Sequence.PlaybackRange.LowerFrame = 200;
	Report = FHyperAIStudioCinematicsContracts::ValidateSnapshot(
		ManyErrors, TinyIssueEnvelope);
	TestTrue(TEXT("issue overflow is explicit"), Report.bTruncated);
	TestFalse(TEXT("issue overflow never passes"), Report.bComplete);
	FHyperAIStudioCinematicsValueSnapshot Oversized = Structural;
	Oversized.Bindings.SetNum(FHyperAIStudioCinematicsContracts::MaxBindings + 1);
	Report = FHyperAIStudioCinematicsContracts::ValidateSnapshot(
		Oversized, StructuralRequest);
	TestEqual(TEXT("detached count envelope rejects before record traversal"), Report.Status,
		FString(TEXT("validation_envelope_rejected")));
	TestTrue(TEXT("detached count envelope has one stable issue"),
		Report.Issues.Num() == 1
		&& Report.Issues[0].Code == TEXT("detached_snapshot_envelope_exceeded"));
	FHyperAIStudioCinematicsValueSnapshot OversizedText = Structural;
	OversizedText.Sequence.ClassName = FString::ChrN(
		FHyperAIStudioCinematicsContracts::MaxNameCharacters + 1, TEXT('x'));
	Report = FHyperAIStudioCinematicsContracts::ValidateSnapshot(
		OversizedText, StructuralRequest);
	TestEqual(TEXT("detached text envelope rejects before hashing"), Report.Status,
		FString(TEXT("validation_envelope_rejected")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioCinematicsCursorCloneAdapterTest,
	"HyperAIStudio.NativeTools.Cinematics.CursorClonePurePrepareAndZeroEffectAdapter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioCinematicsCursorCloneAdapterTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Cinematics::Tests;
	const FString SequencePath = TEXT("/Game/Cinematics/LS_Test.LS_Test");
	const FString ConfigPath = TEXT("/Game/Cinematics/MRQ_Test.MRQ_Test");
	const FString SnapshotSeal = Hash(TEXT("snapshot"));
	const FString Cursor = FHyperAIStudioCinematicsContracts::BuildCursor(
		SequencePath, ConfigPath, SnapshotSeal, 32, 64);
	int32 Offset = 0;
	TestTrue(TEXT("sealed cursor parses"), FHyperAIStudioCinematicsContracts::ParseCursor(
		Cursor, SequencePath, ConfigPath, SnapshotSeal, 32, Offset));
	TestEqual(TEXT("cursor offset"), Offset, 64);
	TestFalse(TEXT("loaded-value drift invalidates cursor"),
		FHyperAIStudioCinematicsContracts::ParseCursor(Cursor, SequencePath, ConfigPath,
			Hash(TEXT("changed snapshot")), 32, Offset));
	TestTrue(TEXT("cursor beyond total bound is never minted"),
		FHyperAIStudioCinematicsContracts::BuildCursor(SequencePath, ConfigPath,
			SnapshotSeal, 32,
			FHyperAIStudioCinematicsContracts::MaxSnapshotItems + 1).IsEmpty());

	const auto Payload = RenderPayload();
	const TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> Clone =
		Payload->CloneImmutable();
	TestTrue(TEXT("clone is detached"), &Clone.Get() != &Payload.Get());
	TestEqual(TEXT("clone semantic seal"), Clone->GetSemanticFingerprint(),
		Payload->GetSemanticFingerprint());
	Payload->SequencePath = TEXT("/Game/Cinematics/LS_Changed.LS_Changed");
	TestNotEqual(TEXT("clone does not alias source fields"),
		static_cast<const FHyperAIStudioCinematicsRenderPayload&>(Clone.Get()).SequencePath,
		Payload->SequencePath);
	const auto PurePayload = RenderPayload();
	const FHyperAIStudioDomainAdapterDescriptor& Descriptor =
		FHyperAIStudioCinematicsContracts::GetAdapterDescriptor();
	FHyperAIStudioDomainBinding Binding;
	Binding.PackId = FHyperAIStudioCinematicsContracts::PackId;
	Binding.ToolName = TEXT("hyper_cinematics_apply_plan");
	Binding.VariantId = FHyperAIStudioCinematicsContracts::MutationVariantId;
	Binding.ExpectedSafety = EHyperAIStudioDomainSafety::ExternalEffect;
	Binding.CanonicalProjectId = TEXT("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
	Binding.ExpectedAdapterFingerprint = Descriptor.AdapterFingerprint;
	Binding.ExpectedAdapterGeneration = 1;
	Binding.ExpectedRegistryEpoch = 1;
	Binding.Prerequisites.PackId = Binding.PackId;
	Binding.Prerequisites.bPackEnabled = true;
	Binding.Prerequisites.Revision = 1;
	Binding.Prerequisites.Observations = {
		{TEXT("module.Sequencer"), EHyperAIStudioDomainPrerequisiteState::Available},
		{TEXT("plugin.MovieRenderPipeline"), EHyperAIStudioDomainPrerequisiteState::Available},
		{FHyperAIStudioCinematicsContracts::LiveProbeId,
			EHyperAIStudioDomainPrerequisiteState::Available}};
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
	Contract.EffectTarget = TEXT("cinematics:test-snapshot");
	Contract.DeadlineMs = 1000;
	Contract.MaxNativeOperations = 8;
	Contract.MaxGameThreadMs = 200;
	Contract.MaxOutputBytes = 16384;
	Contract.MaxResultBytes = 256;
	Contract.StageLifetimeMs = FHyperAIStudioCinematicsContracts::StageLifetimeMs;
	Contract.bCompileOnce = false;
	Contract.bSaveOnce = false;
	Contract.bValidateOnce = true;
	Contract.bVerifyFreshOnce = true;
	FHyperAIStudioPreparedTypedArtifact Prepared;
	FString Error;
	TestTrue(TEXT("pure typed-artifact preparation seals dry-run intent"),
		FHyperAIStudioTypedArtifactExecutor::Prepare(Contract, Prepared, Error));
	TestTrue(TEXT("pure plan hash is canonical"),
		FHyperAIStudioCinematicsContracts::IsCanonicalSha256(Prepared.PlanHash));
	FHyperAIStudioDomainDispatchContext Context;
	Context.Binding = Binding;
	Context.Safety = EHyperAIStudioDomainSafety::ExternalEffect;
	Context.ActionKind = EHyperAIStudioDomainExecutionActionKind::Apply;
	FHyperAIStudioCinematicsDomainAdapter Adapter;
	const FHyperAIStudioDomainAdapterResult Result = Adapter.Execute(Context, *PurePayload);
	TestEqual(TEXT("adapter rejects before effect"), static_cast<uint8>(Result.Outcome),
		static_cast<uint8>(EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect));
	TestEqual(TEXT("stable continuation blocker"), Result.StatusCode,
		FString(FHyperAIStudioCinematicsContracts::NonDryCallableState));
	TestFalse(TEXT("zero-effect rejection has no result payload"), Result.Payload.IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioCinematicsReflectionTest,
	"HyperAIStudio.NativeTools.Cinematics.ExactAICallableReflectionContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioCinematicsReflectionTest::RunTest(const FString& Parameters)
{
	TSet<FString> Names;
	for (TFieldIterator<UFunction> It(UHyperAIStudioCinematicsToolset::StaticClass(),
		EFieldIteratorFlags::ExcludeSuper); It; ++It)
	{
		if (It->HasMetaData(TEXT("AICallable"))) Names.Add(It->GetName());
	}
	TestEqual(TEXT("exactly three reflected AICallables"), Names.Num(), 3);
	TestTrue(TEXT("inspect reflected"), Names.Contains(TEXT("hyper_cinematics_inspect")));
	TestTrue(TEXT("apply reflected"), Names.Contains(TEXT("hyper_cinematics_apply_plan")));
	TestTrue(TEXT("validate reflected"), Names.Contains(TEXT("hyper_cinematics_validate")));
	TestNull(TEXT("no raw script field"),
		FHyperAICinematicsApplyPlanRequest::StaticStruct()->FindPropertyByName(TEXT("Script")));
	TestNull(TEXT("no raw JSON field"),
		FHyperAICinematicsApplyPlanRequest::StaticStruct()->FindPropertyByName(TEXT("Json")));
	TestNull(TEXT("no queue job field"),
		FHyperAICinematicsApplyPlanRequest::StaticStruct()->FindPropertyByName(TEXT("Job")));
	TestNull(TEXT("no client authorization token"),
		FHyperAICinematicsApplyPlanRequest::StaticStruct()->FindPropertyByName(
			TEXT("AuthorizationToken")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
