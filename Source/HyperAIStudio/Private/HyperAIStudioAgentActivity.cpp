// Games by Hyper 2026.

#include "HyperAIStudioAgentActivity.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace HyperAIStudio::Activity::Private
{
	TArray<FHyperAIStudioActivityEntry>& Entries()
	{
		// Newest first, bounded: the panel shows a session, not an archive.
		static TArray<FHyperAIStudioActivityEntry> Storage;
		return Storage;
	}

	FHyperAIStudioAgentActivityLog::FAgentResolver& Resolver()
	{
		static FHyperAIStudioAgentActivityLog::FAgentResolver Value;
		return Value;
	}

	FString AgentLabel(const FString& Agent, const FString& Model)
	{
		return Model.IsEmpty() ? Agent : FString::Printf(TEXT("%s (%s)"), *Agent, *Model);
	}

	struct FScoreStore
	{
		bool bLoaded = false;
		FString PathOverride;
		TArray<FHyperAIStudioAgentScore> Scores;

		FString GetPath() const
		{
			return !PathOverride.IsEmpty() ? PathOverride
				: FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("HyperAIStudio/AgentScoreboard.json"));
		}

		void EnsureLoaded()
		{
			if (bLoaded) return;
			bLoaded = true;
			Scores.Reset();
			FString Json;
			TArray<TSharedPtr<FJsonValue>> Rows;
			if (!FFileHelper::LoadFileToString(Json, *GetPath())
				|| !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Rows))
			{
				return;
			}
			for (const TSharedPtr<FJsonValue>& Value : Rows)
			{
				const TSharedPtr<FJsonObject>* Object = nullptr;
				if (!Value.IsValid() || !Value->TryGetObject(Object)) continue;
				FHyperAIStudioAgentScore& Score = Scores.AddDefaulted_GetRef();
				(*Object)->TryGetStringField(TEXT("agent"), Score.Agent);
				(*Object)->TryGetStringField(TEXT("model"), Score.Model);
				(*Object)->TryGetStringField(TEXT("pack"), Score.PackId);
				(*Object)->TryGetNumberField(TEXT("completed"), Score.Completed);
				(*Object)->TryGetNumberField(TEXT("failed"), Score.Failed);
				(*Object)->TryGetNumberField(TEXT("rejected"), Score.Rejected);
			}
		}

		void Save() const
		{
			FString Json;
			const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
			Writer->WriteArrayStart();
			for (const FHyperAIStudioAgentScore& Score : Scores)
			{
				Writer->WriteObjectStart();
				Writer->WriteValue(TEXT("agent"), Score.Agent);
				Writer->WriteValue(TEXT("model"), Score.Model);
				Writer->WriteValue(TEXT("pack"), Score.PackId);
				Writer->WriteValue(TEXT("completed"), Score.Completed);
				Writer->WriteValue(TEXT("failed"), Score.Failed);
				Writer->WriteValue(TEXT("rejected"), Score.Rejected);
				Writer->WriteObjectEnd();
			}
			Writer->WriteArrayEnd();
			Writer->Close();
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(GetPath()), true);
			FFileHelper::SaveStringToFile(Json, *GetPath());
		}
	};

	FScoreStore& Store()
	{
		static FScoreStore Value;
		return Value;
	}
}

void FHyperAIStudioAgentActivityLog::Record(FHyperAIStudioActivityEntry Entry)
{
	using namespace HyperAIStudio::Activity::Private;
	if (!IsInGameThread())
	{
		return;
	}
	if (Entry.Utc == FDateTime::MinValue())
	{
		Entry.Utc = FDateTime::UtcNow();
	}
	TArray<FHyperAIStudioActivityEntry>& Storage = Entries();
	const FHyperAIStudioActivityEntry* First = nullptr;
	if (!Entry.OperationId.IsEmpty())
	{
		// Newest first, so the last match is the operation's first entry.
		for (const FHyperAIStudioActivityEntry& Existing : Storage)
		{
			if (Existing.OperationId == Entry.OperationId) First = &Existing;
		}
	}
	if (First)
	{
		if (Entry.Agent.IsEmpty())
		{
			Entry.Agent = First->Agent;
			Entry.Model = First->Model;
		}
		if (Entry.Target.IsEmpty()) Entry.Target = First->Target;
	}
	else if (Entry.Agent.IsEmpty() && Resolver())
	{
		FString Agent;
		FString Model;
		if (Resolver()(Agent, Model))
		{
			Entry.Agent = Agent;
			Entry.Model = Model;
		}
	}
	FHyperAIStudioAgentScoreboard::Tally(Entry);
	Storage.Insert(MoveTemp(Entry), 0);
	if (Storage.Num() > MaxEntries)
	{
		Storage.SetNum(MaxEntries);
	}
	OnChanged().Broadcast();
}

TArray<FHyperAIStudioActivityEntry> FHyperAIStudioAgentActivityLog::GetSnapshot()
{
	return HyperAIStudio::Activity::Private::Entries();
}

void FHyperAIStudioAgentActivityLog::Clear()
{
	HyperAIStudio::Activity::Private::Entries().Reset();
	OnChanged().Broadcast();
}

FSimpleMulticastDelegate& FHyperAIStudioAgentActivityLog::OnChanged()
{
	static FSimpleMulticastDelegate Delegate;
	return Delegate;
}

const TCHAR* FHyperAIStudioAgentActivityLog::LexToString(const EHyperAIStudioActivityKind Kind)
{
	switch (Kind)
	{
	case EHyperAIStudioActivityKind::ApprovalRequested: return TEXT("Waiting for approval");
	case EHyperAIStudioActivityKind::ApprovalApproved: return TEXT("Approved");
	case EHyperAIStudioActivityKind::ApprovalRejected: return TEXT("Rejected");
	case EHyperAIStudioActivityKind::Submitted: return TEXT("Running");
	case EHyperAIStudioActivityKind::JobStarted: return TEXT("Started");
	case EHyperAIStudioActivityKind::JobFinished: return TEXT("Finished");
	case EHyperAIStudioActivityKind::Completed: return TEXT("Completed");
	default: return TEXT("Failed");
	}
}

FHyperAIStudioAgentActivityLog::FAgentResolver FHyperAIStudioAgentActivityLog::SetAgentResolver(FAgentResolver Resolver)
{
	FAgentResolver Previous = MoveTemp(HyperAIStudio::Activity::Private::Resolver());
	HyperAIStudio::Activity::Private::Resolver() = MoveTemp(Resolver);
	return Previous;
}

FString FHyperAIStudioAgentActivityLog::DescribeLastChange(const FString& Target)
{
	using namespace HyperAIStudio::Activity::Private;
	if (Target.IsEmpty() || !IsInGameThread())
	{
		return FString();
	}
	for (const FHyperAIStudioActivityEntry& Entry : Entries())
	{
		if (Entry.Kind != EHyperAIStudioActivityKind::Completed || !Entry.Target.Equals(Target, ESearchCase::IgnoreCase))
		{
			continue;
		}
		const int32 Minutes = FMath::FloorToInt((FDateTime::UtcNow() - Entry.Utc).GetTotalMinutes());
		return FString::Printf(TEXT(" The last agent edit to it was by %s with %s (operation %s), %s. If that was another chat tab, check with it before editing the same asset."),
			Entry.Agent.IsEmpty() ? TEXT("an unidentified agent") : *AgentLabel(Entry.Agent, Entry.Model),
			*Entry.ToolName, *Entry.OperationId,
			Minutes < 1 ? TEXT("under a minute ago") : *FString::Printf(TEXT("%d min ago"), Minutes));
	}
	return FString();
}

TArray<FHyperAIStudioAgentScore> FHyperAIStudioAgentScoreboard::GetScores()
{
	HyperAIStudio::Activity::Private::FScoreStore& Store = HyperAIStudio::Activity::Private::Store();
	Store.EnsureLoaded();
	TArray<FHyperAIStudioAgentScore> Scores = Store.Scores;
	Scores.StableSort([](const FHyperAIStudioAgentScore& A, const FHyperAIStudioAgentScore& B) { return A.Total() > B.Total(); });
	return Scores;
}

void FHyperAIStudioAgentScoreboard::Tally(const FHyperAIStudioActivityEntry& Entry)
{
	const bool bCompleted = Entry.Kind == EHyperAIStudioActivityKind::Completed;
	// A failure counts only when it ends an operation; job-level failures carry no operation id.
	const bool bFailed = Entry.Kind == EHyperAIStudioActivityKind::Failed && !Entry.OperationId.IsEmpty();
	const bool bRejected = Entry.Kind == EHyperAIStudioActivityKind::ApprovalRejected;
	if (!IsInGameThread() || Entry.Agent.IsEmpty() || Entry.PackId.IsEmpty() || !(bCompleted || bFailed || bRejected))
	{
		return;
	}
	HyperAIStudio::Activity::Private::FScoreStore& Store = HyperAIStudio::Activity::Private::Store();
	Store.EnsureLoaded();
	FHyperAIStudioAgentScore* Score = Store.Scores.FindByPredicate([&Entry](const FHyperAIStudioAgentScore& Existing)
	{
		return Existing.Agent == Entry.Agent && Existing.Model == Entry.Model && Existing.PackId == Entry.PackId;
	});
	if (!Score)
	{
		Score = &Store.Scores.AddDefaulted_GetRef();
		Score->Agent = Entry.Agent;
		Score->Model = Entry.Model;
		Score->PackId = Entry.PackId;
	}
	Score->Completed += bCompleted ? 1 : 0;
	Score->Failed += bFailed ? 1 : 0;
	Score->Rejected += bRejected ? 1 : 0;
	// ponytail: rewrites the whole file per finished operation; a few hundred rows at most.
	Store.Save();
}

FHyperAIStudioAgentScore FHyperAIStudioAgentScoreboard::Summarize(const FString& Agent, const FString& Model, const TArray<FString>& PackIds)
{
	FHyperAIStudioAgentScore Sum;
	Sum.Agent = Agent;
	Sum.Model = Model;
	for (const FHyperAIStudioAgentScore& Score : GetScores())
	{
		if (Score.Agent == Agent && Score.Model == Model && (PackIds.IsEmpty() || PackIds.Contains(Score.PackId)))
		{
			Sum.Completed += Score.Completed;
			Sum.Failed += Score.Failed;
			Sum.Rejected += Score.Rejected;
		}
	}
	return Sum;
}

FHyperAIStudioRouteChoice FHyperAIStudioAgentScoreboard::ChooseAgent(const FString& FixedAgent, const FString& FixedModel,
	const TArray<FString>& PackIds, const TArray<FString>& UsableAgents, const FString& FallbackAgent)
{
	auto IsUsable = [&UsableAgents](const FString& Agent)
	{
		return !Agent.IsEmpty() && UsableAgents.ContainsByPredicate([&Agent](const FString& Usable) { return Usable.Equals(Agent, ESearchCase::IgnoreCase); });
	};
	FHyperAIStudioRouteChoice Choice;
	FString Unusable;
	if (IsUsable(FixedAgent))
	{
		Choice.Agent = FixedAgent;
		Choice.Model = FixedModel;
		Choice.Reason = TEXT("Chosen in the task routing settings.");
		return Choice;
	}
	if (!FixedAgent.IsEmpty())
	{
		Unusable = FString::Printf(TEXT("%s is not set up here, so "), *FixedAgent);
	}

	double BestRank = -1.0;
	FHyperAIStudioAgentScore Best;
	TSet<FString> Seen;
	for (const FHyperAIStudioAgentScore& Score : GetScores())
	{
		if (!IsUsable(Score.Agent) || Seen.Contains(Score.Agent + TEXT("\n") + Score.Model)) continue;
		Seen.Add(Score.Agent + TEXT("\n") + Score.Model);
		const FHyperAIStudioAgentScore Sum = Summarize(Score.Agent, Score.Model, PackIds);
		if (Sum.Total() < MinSamplesToPick) continue;
		const double Rank = (Sum.Completed + 1.0) / (Sum.Total() + 2.0);
		if (Rank > BestRank || (Rank == BestRank && Sum.Total() > Best.Total()))
		{
			BestRank = Rank;
			Best = Sum;
		}
	}
	if (BestRank >= 0.0)
	{
		Choice.Agent = Best.Agent;
		Choice.Model = Best.Model;
		Choice.Reason = FString::Printf(TEXT("%sbest record on these tasks: %d of %d completed."), *Unusable, Best.Completed, Best.Total());
		Choice.Reason[0] = FChar::ToUpper(Choice.Reason[0]);
		return Choice;
	}
	Choice.Agent = IsUsable(FallbackAgent) ? FallbackAgent : (UsableAgents.IsEmpty() ? FString() : UsableAgents[0]);
	Choice.Reason = FString::Printf(TEXT("%sno agent has %d operations on these tasks yet; using %s."),
		*Unusable, MinSamplesToPick, Choice.Agent.IsEmpty() ? TEXT("none") : *Choice.Agent);
	Choice.Reason[0] = FChar::ToUpper(Choice.Reason[0]);
	return Choice;
}

void FHyperAIStudioAgentScoreboard::Reset()
{
	HyperAIStudio::Activity::Private::FScoreStore& Store = HyperAIStudio::Activity::Private::Store();
	Store.Scores.Reset();
	Store.bLoaded = true;
	Store.Save();
}

void FHyperAIStudioAgentScoreboard::SetStoragePathForTests(const FString& Path)
{
	HyperAIStudio::Activity::Private::FScoreStore& Store = HyperAIStudio::Activity::Private::Store();
	Store.PathOverride = Path;
	Store.bLoaded = false;
}
