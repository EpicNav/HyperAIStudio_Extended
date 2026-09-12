// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "HyperAIStudioPromptHistory.generated.h"

USTRUCT()
struct FHyperAIStudioPromptHistoryEntry
{
	GENERATED_BODY()

	UPROPERTY(Config)
	FString Text;

	UPROPERTY(Config)
	FString AgentName;
};

/**
 * Prompts sent from the chat composer, newest first. The CDO is the live list for the editor session.
 *
 * Deliberately not a property of UHyperAIStudioSettings: that class is DefaultConfig, so any SaveConfig on it
 * writes the project's shared Config/Default*.ini, where prompt text would end up in source control. This class
 * saves to the per-user Saved/ ini, and only when UHyperAIStudioSettings::bPersistPromptHistory is on.
 */
UCLASS(Config = EditorPerProjectUserSettings)
class UHyperAIStudioPromptHistory : public UObject
{
	GENERATED_BODY()

public:
	static constexpr int32 MaxEntries = 100;
	static constexpr int32 MaxEntryChars = 32000;

	UPROPERTY(Config)
	TArray<FHyperAIStudioPromptHistoryEntry> Entries;

	/** Most-recent-first push. An exact-text duplicate moves to the front. Rejects empty or over-long text. */
	static bool Push(TArray<FHyperAIStudioPromptHistoryEntry>& InOut, const FString& Text, const FString& AgentName);

	/** Adds a sent prompt to the session list, saving it only if the user opted in. */
	static void Record(const FString& Text, const FString& AgentName);

	/** Empties the list and the saved copy, including entries saved before persistence was turned off. */
	static void Clear();
};
