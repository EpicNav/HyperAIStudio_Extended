// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "Tests/HyperAIStudioBenchmarkInstrumentation.h"

#include "Containers/Set.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "HAL/PlatformTime.h"
#include "Logging/LogMacros.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Templates/SharedPointer.h"
#include "Templates/ValueOrError.h"
#include "ToolsetRegistry/Toolset.h"
#include "ToolsetRegistry/ToolsetRegistry.h"
#include "ToolsetRegistry/ToolsetRegistrySubsystem.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIBenchmarkInstrumentation, Log, All);

namespace HyperAIStudio::BenchmarkInstrumentation::Private
{
	using UE::ToolsetRegistry::FToolset;
	using UE::ToolsetRegistry::FToolsetRegistry;

	constexpr TCHAR CandidateToolsetName[] = TEXT("HyperAIStudio.HyperAIStudioContextSnapshotToolset");
	constexpr TCHAR CandidateToolName[] = TEXT("hyper_context_snapshot");
	constexpr TCHAR EpicToolsetName[] = TEXT("EditorToolset.EditorAppToolset");
	constexpr TCHAR ActorsToolName[] = TEXT("GetSelectedActors");
	constexpr TCHAR CameraToolName[] = TEXT("GetCameraTransform");
	constexpr TCHAR AssetsToolName[] = TEXT("GetSelectedAssets");
	constexpr TCHAR InstrumentationField[] = TEXT("benchmarkInstrumentation");

	TValueOrError<FString, FString> InjectInstrumentation(
		FString&& ResultJson,
		const FString& QualifiedToolName,
		const uint64 StartCycles,
		const uint64 FinishCycles)
	{
		if (FinishCycles <= StartCycles)
		{
			return MakeError(FString::Printf(
				TEXT("Benchmark clock did not advance while executing %s."),
				*QualifiedToolName));
		}

		TSharedPtr<FJsonObject> ResultObject;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResultJson);
		if (!FJsonSerializer::Deserialize(Reader, ResultObject) || !ResultObject.IsValid())
		{
			return MakeError(FString::Printf(
				TEXT("Benchmark proxy could not parse the object result from %s."),
				*QualifiedToolName));
		}
		if (ResultObject->HasField(InstrumentationField))
		{
			return MakeError(FString::Printf(
				TEXT("Benchmark result field collision for %s."),
				*QualifiedToolName));
		}

		const double GameThreadMilliseconds = FPlatformTime::ToMilliseconds64(FinishCycles - StartCycles);
		if (!FMath::IsFinite(GameThreadMilliseconds) || GameThreadMilliseconds <= 0.0)
		{
			return MakeError(FString::Printf(
				TEXT("Benchmark duration was not positive and finite for %s."),
				*QualifiedToolName));
		}

		TSharedRef<FJsonObject> Instrumentation = MakeShared<FJsonObject>();
		Instrumentation->SetStringField(TEXT("schema"), TEXT("hyperai.toolset-registry-game-thread-sample.v1"));
		Instrumentation->SetStringField(TEXT("qualified_tool"), QualifiedToolName);
		Instrumentation->SetStringField(TEXT("clock"), TEXT("FPlatformTime::Cycles64"));
		Instrumentation->SetStringField(TEXT("measurement_scope"), TEXT("ToolsetRegistry.ExecuteTool on game thread"));
		Instrumentation->SetNumberField(TEXT("native_operations"), 1);
		Instrumentation->SetNumberField(TEXT("game_thread_ms"), GameThreadMilliseconds);
		Instrumentation->SetNumberField(TEXT("longest_game_thread_block_ms"), GameThreadMilliseconds);
		ResultObject->SetObjectField(InstrumentationField, Instrumentation);

		FString InstrumentedJson;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&InstrumentedJson);
		if (!FJsonSerializer::Serialize(ResultObject.ToSharedRef(), Writer))
		{
			return MakeError(FString::Printf(
				TEXT("Benchmark proxy could not serialize the instrumented result from %s."),
				*QualifiedToolName));
		}
		return MakeValue(MoveTemp(InstrumentedJson));
	}

	class FInstrumentedToolset final : public FToolset
	{
	public:
		FInstrumentedToolset(TSharedRef<FToolset> InInner, TSet<FString> InInstrumentedTools)
			: Inner(MoveTemp(InInner))
			, InstrumentedTools(MoveTemp(InInstrumentedTools))
		{
		}

		virtual FString GetToolsetName() const override
		{
			return Inner->GetToolsetName();
		}

		virtual FString GetToolsetVersion() const override
		{
			return Inner->GetToolsetVersion();
		}

		virtual FString GetToolsetDescription() const override
		{
			return Inner->GetToolsetDescription();
		}

		virtual UClass* GetToolsetClass() const override
		{
			return Inner->GetToolsetClass();
		}

	protected:
		virtual TFuture<TValueOrError<FString, FString>> ExecuteToolInternal(
			const FString& ToolName,
			const FString& JsonInput) override
		{
			if (!InstrumentedTools.Contains(ToolName))
			{
				return Inner->ExecuteTool(ToolName, JsonInput);
			}
			if (!IsInGameThread())
			{
				return MakeFulfilledPromise<TValueOrError<FString, FString>>(
					MakeError(FString(TEXT("Benchmark proxy execution must start on the game thread.")))).GetFuture();
			}

			const FString QualifiedToolName = GetToolsetName() + TEXT(".") + ToolName;
			const uint64 StartCycles = FPlatformTime::Cycles64();
			return Inner->ExecuteTool(ToolName, JsonInput).Next(
				[QualifiedToolName, StartCycles](TValueOrError<FString, FString>&& Result)
					-> TValueOrError<FString, FString>
				{
					if (!IsInGameThread())
					{
						return MakeError(FString::Printf(
							TEXT("Benchmark proxy completion for %s left the game thread."),
							*QualifiedToolName));
					}
					const uint64 FinishCycles = FPlatformTime::Cycles64();
					if (Result.HasError())
					{
						return MakeError(Result.StealError());
					}
					return InjectInstrumentation(
						Result.StealValue(), QualifiedToolName, StartCycles, FinishCycles);
				});
		}

		virtual FString GetJsonSchemaInternal() const override
		{
			return Inner->GetJsonSchema();
		}

	private:
		TSharedRef<FToolset> Inner;
		TSet<FString> InstrumentedTools;
	};

	struct FProxyPair
	{
		TSharedPtr<FToolset> CandidateOriginal;
		TSharedPtr<FToolset> EpicOriginal;
		TSharedPtr<FToolset> CandidateProxy;
		TSharedPtr<FToolset> EpicProxy;

		bool IsInstalled() const
		{
			return CandidateProxy.IsValid() && EpicProxy.IsValid();
		}

		void Reset()
		{
			CandidateProxy.Reset();
			EpicProxy.Reset();
			CandidateOriginal.Reset();
			EpicOriginal.Reset();
		}
	};

	bool RestoreIfCurrent(
		FToolsetRegistry& Registry,
		const TSharedPtr<FToolset>& Proxy,
		const TSharedPtr<FToolset>& Original)
	{
		if (!Proxy.IsValid() || !Original.IsValid())
		{
			return false;
		}
		const FString Name = Proxy->GetToolsetName();
		if (Registry.Find(Name) != Proxy)
		{
			return false;
		}
		return Registry.UnregisterToolset(Proxy) && Registry.RegisterToolset(Original);
	}

	bool InstallPair(FToolsetRegistry& Registry, FProxyPair& OutPair)
	{
		if (OutPair.IsInstalled())
		{
			return true;
		}

		TSharedPtr<FToolset> CandidateOriginal = Registry.Find(CandidateToolsetName);
		TSharedPtr<FToolset> EpicOriginal = Registry.Find(EpicToolsetName);
		if (!CandidateOriginal.IsValid() || !EpicOriginal.IsValid())
		{
			return false;
		}

		TSharedPtr<FToolset> CandidateProxy = MakeShared<FInstrumentedToolset>(
			CandidateOriginal.ToSharedRef(),
			TSet<FString>{CandidateToolName});
		TSharedPtr<FToolset> EpicProxy = MakeShared<FInstrumentedToolset>(
			EpicOriginal.ToSharedRef(),
			TSet<FString>{ActorsToolName, CameraToolName, AssetsToolName});

		if (!Registry.UnregisterToolset(CandidateOriginal))
		{
			return false;
		}
		if (!Registry.UnregisterToolset(EpicOriginal))
		{
			Registry.RegisterToolset(CandidateOriginal);
			return false;
		}
		if (!Registry.RegisterToolset(CandidateProxy))
		{
			Registry.RegisterToolset(EpicOriginal);
			Registry.RegisterToolset(CandidateOriginal);
			return false;
		}
		if (!Registry.RegisterToolset(EpicProxy))
		{
			Registry.UnregisterToolset(CandidateProxy);
			Registry.RegisterToolset(EpicOriginal);
			Registry.RegisterToolset(CandidateOriginal);
			return false;
		}

		OutPair.CandidateOriginal = MoveTemp(CandidateOriginal);
		OutPair.EpicOriginal = MoveTemp(EpicOriginal);
		OutPair.CandidateProxy = MoveTemp(CandidateProxy);
		OutPair.EpicProxy = MoveTemp(EpicProxy);
		return true;
	}

	void RestorePair(FToolsetRegistry& Registry, FProxyPair& Pair)
	{
		// Restore independently and only while our exact proxy is current. This never replaces a
		// handler installed by somebody else after benchmark startup.
		RestoreIfCurrent(Registry, Pair.EpicProxy, Pair.EpicOriginal);
		RestoreIfCurrent(Registry, Pair.CandidateProxy, Pair.CandidateOriginal);
		Pair.Reset();
	}

	struct FRuntimeState
	{
		bool bStarted = false;
		FTSTicker::FDelegateHandle RetryTicker;
		FProxyPair Pair;
	};

	FRuntimeState& RuntimeState()
	{
		static FRuntimeState State;
		return State;
	}

	bool TryInstallRuntime()
	{
		if (!IsInGameThread())
		{
			return false;
		}
		auto Subsystem = UToolsetRegistrySubsystem::Get();
		if (Subsystem.HasError())
		{
			return false;
		}
		FRuntimeState& State = RuntimeState();
		if (!InstallPair(Subsystem.GetValue()->ToolsetRegistry, State.Pair))
		{
			return false;
		}
		UE_LOG(LogHyperAIBenchmarkInstrumentation, Display,
			TEXT("Installed dev-only game-thread benchmark proxies for %s and %s."),
			CandidateToolsetName,
			EpicToolsetName);
		return true;
	}
}

void HyperAIStudio::BenchmarkInstrumentation::Startup()
{
	using namespace Private;
	FRuntimeState& State = RuntimeState();
	if (State.bStarted
		|| !FParse::Param(FCommandLine::Get(), TEXT("HyperAIBenchmarkInstrumentation")))
	{
		return;
	}
	State.bStarted = true;
	if (TryInstallRuntime())
	{
		return;
	}
	State.RetryTicker = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([](float)
		{
			const bool bInstalled = Private::TryInstallRuntime();
			if (bInstalled)
			{
				Private::RuntimeState().RetryTicker.Reset();
			}
			return !bInstalled;
		}),
		0.1f);
}

void HyperAIStudio::BenchmarkInstrumentation::Shutdown()
{
	using namespace Private;
	FRuntimeState& State = RuntimeState();
	if (State.RetryTicker.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(State.RetryTicker);
		State.RetryTicker.Reset();
	}
	if (State.Pair.IsInstalled() && IsInGameThread())
	{
		auto Subsystem = UToolsetRegistrySubsystem::Get();
		if (Subsystem.HasValue())
		{
			RestorePair(Subsystem.GetValue()->ToolsetRegistry, State.Pair);
			UE_LOG(LogHyperAIBenchmarkInstrumentation, Display,
				TEXT("Restored benchmarked ToolsetRegistry handlers."));
		}
	}
	State.Pair.Reset();
	State.bStarted = false;
}

namespace HyperAIStudio::BenchmarkInstrumentation::Private
{
	class FFixtureToolset final : public FToolset
	{
	public:
		explicit FFixtureToolset(FString InName)
			: Name(MoveTemp(InName))
		{
		}

		virtual FString GetToolsetName() const override { return Name; }
		virtual FString GetToolsetVersion() const override { return TEXT("1"); }
		virtual FString GetToolsetDescription() const override { return TEXT("Benchmark fixture"); }

	protected:
		virtual TFuture<TValueOrError<FString, FString>> ExecuteToolInternal(
			const FString& ToolName,
			const FString&) override
		{
			if (ToolName != TEXT("Read"))
			{
				return MakeFulfilledPromise<TValueOrError<FString, FString>>(
					MakeError(FString(TEXT("Unknown fixture tool")))).GetFuture();
			}
			return MakeFulfilledPromise<TValueOrError<FString, FString>>(
				MakeValue(FString(TEXT("{\"returnValue\":{\"ok\":true}}")))).GetFuture();
		}

		virtual FString GetJsonSchemaInternal() const override
		{
			return FString::Printf(
				TEXT("{\"name\":\"%s\",\"version\":\"1\",\"description\":\"fixture\",\"tools\":[{\"name\":\"Read\",\"description\":\"read\",\"inputSchema\":{\"type\":\"object\"}}]}"),
				*Name);
		}

	private:
		FString Name;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIBenchmarkInstrumentationWrapTest,
	"HyperAIStudio.BenchmarkInstrumentation.WrapAndInject",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIBenchmarkInstrumentationWrapTest::RunTest(const FString&)
{
	using namespace HyperAIStudio::BenchmarkInstrumentation::Private;
	TSharedRef<FToolset> Original = MakeShared<FFixtureToolset>(TEXT("Fixture.Benchmark"));
	TSharedRef<FToolset> Proxy = MakeShared<FInstrumentedToolset>(
		Original,
		TSet<FString>{TEXT("Read")});
	TValueOrError<FString, FString> Result = Proxy->ExecuteTool(TEXT("Read"), TEXT("{}")).Get();
	TestTrue(TEXT("Instrumented fixture call succeeds"), Result.HasValue());
	if (!Result.HasValue())
	{
		return false;
	}
	TSharedPtr<FJsonObject> Object;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Result.GetValue());
	TestTrue(TEXT("Instrumented result remains JSON"), FJsonSerializer::Deserialize(Reader, Object));
	const TSharedPtr<FJsonObject>* Metrics = nullptr;
	TestTrue(TEXT("Instrumentation is a result sibling"),
		Object.IsValid() && Object->TryGetObjectField(InstrumentationField, Metrics));
	TestTrue(TEXT("Original returnValue is preserved"),
		Object.IsValid() && Object->HasField(TEXT("returnValue")));
	TestEqual(TEXT("One proxied native operation"),
		Metrics && Metrics->IsValid() ? (*Metrics)->GetIntegerField(TEXT("native_operations")) : 0,
		1);
	TestTrue(TEXT("Measured game-thread duration is positive"),
		Metrics && Metrics->IsValid() && (*Metrics)->GetNumberField(TEXT("game_thread_ms")) > 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIBenchmarkInstrumentationRestoreTest,
	"HyperAIStudio.BenchmarkInstrumentation.Restore",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIBenchmarkInstrumentationRestoreTest::RunTest(const FString&)
{
	using namespace HyperAIStudio::BenchmarkInstrumentation::Private;
	FToolsetRegistry Registry;
	TSharedPtr<FToolset> Original = MakeShared<FFixtureToolset>(TEXT("Fixture.Restore"));
	TSharedPtr<FToolset> Proxy = MakeShared<FInstrumentedToolset>(
		Original.ToSharedRef(),
		TSet<FString>{TEXT("Read")});
	TestTrue(TEXT("Original registers"), Registry.RegisterToolset(Original));
	TestTrue(TEXT("Original unregisters for proxy swap"), Registry.UnregisterToolset(Original));
	TestTrue(TEXT("Proxy registers"), Registry.RegisterToolset(Proxy));
	TestTrue(TEXT("Exact proxy restores original"), RestoreIfCurrent(Registry, Proxy, Original));
	TestTrue(TEXT("Original identity is restored"), Registry.Find(TEXT("Fixture.Restore")) == Original);
	Registry.UnregisterToolset(Original);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
