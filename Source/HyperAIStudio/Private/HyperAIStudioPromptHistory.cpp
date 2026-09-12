// Games by Hyper 2026.

#include "HyperAIStudioPromptHistory.h"

#include "HyperAIStudioSettings.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(HyperAIStudioPromptHistory)

bool UHyperAIStudioPromptHistory::Push(TArray<FHyperAIStudioPromptHistoryEntry>& InOut, const FString& Text, const FString& AgentName)
{
	if (Text.IsEmpty() || Text.Len() > MaxEntryChars)
	{
		return false;
	}

	InOut.RemoveAll([&Text](const FHyperAIStudioPromptHistoryEntry& Entry)
	{
		return Entry.Text.Equals(Text, ESearchCase::CaseSensitive);
	});

	FHyperAIStudioPromptHistoryEntry Entry;
	Entry.Text = Text;
	Entry.AgentName = AgentName;
	InOut.Insert(MoveTemp(Entry), 0);

	if (InOut.Num() > MaxEntries)
	{
		InOut.SetNum(MaxEntries);
	}
	return true;
}

void UHyperAIStudioPromptHistory::Record(const FString& Text, const FString& AgentName)
{
	UHyperAIStudioPromptHistory* History = GetMutableDefault<UHyperAIStudioPromptHistory>();
	if (Push(History->Entries, Text, AgentName) && GetDefault<UHyperAIStudioSettings>()->bPersistPromptHistory)
	{
		History->SaveConfig();
	}
}

void UHyperAIStudioPromptHistory::Clear()
{
	UHyperAIStudioPromptHistory* History = GetMutableDefault<UHyperAIStudioPromptHistory>();
	History->Entries.Reset();
	History->SaveConfig();
}
