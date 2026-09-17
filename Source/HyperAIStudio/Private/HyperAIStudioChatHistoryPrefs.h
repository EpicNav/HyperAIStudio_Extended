// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "HyperAIStudioChatHistoryPrefs.generated.h"

/**
 * Per-user names and pins for agent chats, keyed by session id.
 *
 * Kept here rather than written into the agents' own session files: those belong to the CLI, and a
 * rename must never risk corrupting a conversation. EditorPerProjectUserSettings, so it stays local.
 */
UCLASS(Config = EditorPerProjectUserSettings)
class UHyperAIStudioChatHistoryPrefs : public UObject
{
	GENERATED_BODY()

public:
	static constexpr int32 MaxTitleChars = 90;
	static constexpr int32 MaxTrackedSessions = 200;

	UPROPERTY(Config)
	TMap<FString, FString> Titles;

	UPROPERTY(Config)
	TArray<FString> Pinned;

	static FString GetTitle(const FString& SessionId);
	static bool IsPinned(const FString& SessionId);
	/** An empty or blank name clears the rename and restores the agent's own title. */
	static void SetTitle(const FString& SessionId, const FString& Title);
	static void TogglePinned(const FString& SessionId);
};
