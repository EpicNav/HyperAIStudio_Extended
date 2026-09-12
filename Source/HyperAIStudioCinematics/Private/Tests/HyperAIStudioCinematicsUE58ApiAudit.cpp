// Games by Hyper 2026.

#include "HyperAIStudioCinematicsToolset.h"

#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioCinematicsUE58StaticAuditTest,
	"HyperAIStudio.NativeTools.Cinematics.UE58StaticNoLoadAndZeroEffectAudit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioCinematicsUE58StaticAuditTest::RunTest(const FString& Parameters)
{
	FString SourcePath = FPaths::Combine(
		FPaths::GetPath(FString(__FILE__)), TEXT(".."),
		TEXT("HyperAIStudioCinematicsToolset.cpp"));
	FPaths::CollapseRelativeDirectories(SourcePath);
	FString Source;
	if (!FFileHelper::LoadFileToString(Source, *SourcePath))
	{
		AddWarning(FString::Printf(
			TEXT("Cinematics source audit skipped because source is not installed: %s"),
			*SourcePath));
		return true;
	}
	const TArray<FString> RequiredObservationHooks = {
		TEXT("FSoftObjectPath(SequencePath).ResolveObject()"),
		TEXT("TryGetAssetPackageData"),
		TEXT("bFailIfLockHeld=*/true"),
		TEXT("GetBindings()"),
		TEXT("GetAllSections()"),
		TEXT("GetChannelProxy()"),
		TEXT("GetKeys(TRange<FFrameNumber>::All()"),
		TEXT("UMovieSceneCameraCutSection::StaticClass()"),
		TEXT("UMovieSceneEventTriggerSection::StaticClass()"),
		TEXT("FScriptArrayHelper"),
		TEXT("TEXT(\"OutputSetting\")"),
		TEXT("PrimaryOutputSetting"),
		TEXT("HashFloatValue("),
		TEXT("HashDoubleValue("),
		TEXT("UMoviePipelineOutputSetting::StaticClass()")};
	for (const FString& Hook : RequiredObservationHooks)
	{
		TestTrue(*FString::Printf(TEXT("reviewed observation hook present: %s"), *Hook),
			Source.Contains(Hook));
	}
	const TArray<FString> ForbiddenCalls = {
		TEXT("LoadObject<"),
		TEXT("StaticLoadObject("),
		TEXT("TryLoad("),
		TEXT("LoadSynchronous("),
		TEXT("GetAssetByObjectPath("),
		TEXT("GetAssetsByPath("),
		TEXT("ScanPathsSynchronous("),
		TEXT("LoadModuleChecked("),
		TEXT("FindOrAddSettingByClass("),
		TEXT("GetUserSettings("),
		TEXT("AllocateNewJob("),
		TEXT("RenderQueueWithExecutor("),
		TEXT("RenderQueueWithExecutorInstance("),
		TEXT("SavePackage("),
		TEXT("StageExact("),
		TEXT("SubmitExact("),
		TEXT("FHyperAIStudioTypedArtifactExecutor::Execute("),
		TEXT("FHyperAIStudioTypedArtifactStore"),
		TEXT("ClaimExact("),
		TEXT("ExecuteExact("),
		TEXT("TypedArtifactExecutionService"),
		TEXT("PrepareDryRun("),
		TEXT("QueryStatus(")};
	for (const FString& Call : ForbiddenCalls)
	{
		TestFalse(*FString::Printf(TEXT("forbidden load/effect call absent: %s"), *Call),
			Source.Contains(Call));
	}
	TestTrue(TEXT("non-dry blocker is source-frozen"),
		Source.Contains(TEXT("return Reject(NonDryCallableState")));
	TestTrue(TEXT("typed artifact use is pure Prepare only"),
		Source.Contains(TEXT("FHyperAIStudioTypedArtifactExecutor::Prepare(")));
	TestTrue(TEXT("disk and loaded sequence revisions are both sealed"),
		Source.Contains(TEXT("BasePersistedRevision"))
		&& Source.Contains(TEXT("BaseVolatileRevision")));
	TestTrue(TEXT("disk and loaded MRQ revisions are both sealed"),
		Source.Contains(TEXT("BaseRenderConfigPersistedRevision"))
		&& Source.Contains(TEXT("BaseRenderConfigVolatileRevision")));
	FString ModulePath = FPaths::Combine(
		FPaths::GetPath(FString(__FILE__)), TEXT(".."),
		TEXT("HyperAIStudioCinematicsModule.cpp"));
	FPaths::CollapseRelativeDirectories(ModulePath);
	FString ModuleSource;
	TestTrue(TEXT("isolated module source is present"),
		FFileHelper::LoadFileToString(ModuleSource, *ModulePath));
	if (!ModuleSource.IsEmpty())
	{
		TestTrue(TEXT("dynamic reload is disabled"),
			ModuleSource.Contains(TEXT("SupportsDynamicReloading() override { return false; }")));
		TestTrue(TEXT("automatic shutdown is disabled"),
			ModuleSource.Contains(TEXT("SupportsAutomaticShutdown() override { return false; }")));
		TestTrue(TEXT("optional toolset publication is runtime-index owned"),
			ModuleSource.Contains(TEXT("PublishOptionalToolset("))
			&& ModuleSource.Contains(TEXT("WithdrawOptionalToolset(")));
	}
	FString BuildPath = FPaths::Combine(
		FPaths::GetPath(FString(__FILE__)), TEXT(".."), TEXT(".."),
		TEXT("HyperAIStudioCinematics.Build.cs"));
	FPaths::CollapseRelativeDirectories(BuildPath);
	FString BuildSource;
	TestTrue(TEXT("isolated Build.cs is present"),
		FFileHelper::LoadFileToString(BuildSource, *BuildPath));
	if (!BuildSource.IsEmpty())
	{
		for (const FString& Dependency : {
			FString(TEXT("\"LevelSequence\"")), FString(TEXT("\"MovieScene\"")),
			FString(TEXT("\"MovieSceneTracks\"")), FString(TEXT("\"Sequencer\"")),
			FString(TEXT("\"MovieRenderPipelineCore\""))})
		{
			TestTrue(*FString::Printf(TEXT("hard dependency present: %s"), *Dependency),
				BuildSource.Contains(Dependency));
		}
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
