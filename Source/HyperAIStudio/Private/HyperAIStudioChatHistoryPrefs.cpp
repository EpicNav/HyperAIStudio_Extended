// Games by Hyper 2026.

#include "HyperAIStudioChatHistoryPrefs.h"

namespace HyperAIStudio::ChatPrefs
{
	bool IsSaneTitle(const FString& Title)
	{
		if (Title.Len() > UHyperAIStudioChatHistoryPrefs::MaxTitleChars)
		{
			return false;
		}
		for (const TCHAR Character : Title)
		{
			if (Character < 0x20 || Character == 0x7F)
			{
				return false;
			}
		}
		return true;
	}
}

FString UHyperAIStudioChatHistoryPrefs::GetTitle(const FString& SessionId)
{
	const UHyperAIStudioChatHistoryPrefs* Prefs = GetDefault<UHyperAIStudioChatHistoryPrefs>();
	const FString* Title = Prefs ? Prefs->Titles.Find(SessionId) : nullptr;
	return Title ? *Title : FString();
}

bool UHyperAIStudioChatHistoryPrefs::IsPinned(const FString& SessionId)
{
	const UHyperAIStudioChatHistoryPrefs* Prefs = GetDefault<UHyperAIStudioChatHistoryPrefs>();
	return Prefs && Prefs->Pinned.Contains(SessionId);
}

void UHyperAIStudioChatHistoryPrefs::SetTitle(const FString& SessionId, const FString& Title)
{
	UHyperAIStudioChatHistoryPrefs* Prefs = GetMutableDefault<UHyperAIStudioChatHistoryPrefs>();
	const FString Trimmed = Title.TrimStartAndEnd();
	if (!Prefs || SessionId.IsEmpty() || !HyperAIStudio::ChatPrefs::IsSaneTitle(Trimmed))
	{
		return;
	}
	if (Trimmed.IsEmpty())
	{
		Prefs->Titles.Remove(SessionId);
	}
	else
	{
		// Bounded: a long-lived project must not grow this file without limit.
		if (!Prefs->Titles.Contains(SessionId) && Prefs->Titles.Num() >= MaxTrackedSessions)
		{
			return;
		}
		Prefs->Titles.Add(SessionId, Trimmed);
	}
	Prefs->SaveConfig();
}

void UHyperAIStudioChatHistoryPrefs::TogglePinned(const FString& SessionId)
{
	UHyperAIStudioChatHistoryPrefs* Prefs = GetMutableDefault<UHyperAIStudioChatHistoryPrefs>();
	if (!Prefs || SessionId.IsEmpty())
	{
		return;
	}
	if (Prefs->Pinned.Remove(SessionId) == 0 && Prefs->Pinned.Num() < MaxTrackedSessions)
	{
		Prefs->Pinned.Add(SessionId);
	}
	Prefs->SaveConfig();
}
