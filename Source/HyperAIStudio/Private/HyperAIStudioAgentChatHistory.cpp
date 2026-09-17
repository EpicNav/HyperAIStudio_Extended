// Games by Hyper 2026.

#include "HyperAIStudioAgentChatHistory.h"

#include "Algo/AllOf.h"
#include "Dom/JsonObject.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace HyperAIStudio::ChatHistory
{
namespace
{
	constexpr int64 HeadBytes = 512 * 1024;
	constexpr int64 TailBytes = 256 * 1024;
	constexpr int32 MaxTitleChars = 90;

	FString NormalizeRoot(const FString& Path)
	{
		FString Result = FPaths::ConvertRelativePathToFull(Path);
		FPaths::NormalizeDirectoryName(Result);
		return Result;
	}

	FString HomeDirectory()
	{
		FString Home = FPlatformMisc::GetEnvironmentVariable(TEXT("USERPROFILE"));
		return Home.IsEmpty() ? FPlatformMisc::GetEnvironmentVariable(TEXT("HOME")) : Home;
	}

	FString ConfigDirectory(const TCHAR* OverrideVariable, const TCHAR* DefaultName)
	{
		const FString Override = FPlatformMisc::GetEnvironmentVariable(OverrideVariable);
		return Override.IsEmpty() ? HomeDirectory() / DefaultName : Override;
	}

	/** Agents append to these files while running, so open with shared write access. */
	FString ReadRange(const FString& Path, const int64 Offset, const int64 Bytes)
	{
		TUniquePtr<IFileHandle> Handle(FPlatformFileManager::Get().GetPlatformFile().OpenRead(*Path, /*bAllowWrite=*/true));
		if (!Handle || Bytes <= 0 || !Handle->Seek(Offset))
		{
			return FString();
		}
		TArray<uint8> Buffer;
		Buffer.SetNumUninitialized(static_cast<int32>(Bytes));
		if (!Handle->Read(Buffer.GetData(), Bytes))
		{
			return FString();
		}
		const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Buffer.GetData()), Buffer.Num());
		return FString(Converted.Length(), Converted.Get());
	}

	/** The start of a session plus its end, where renames land. Lines cut at the seams simply fail to parse. */
	FString ReadHeadAndTail(const FString& Path)
	{
		const int64 Size = IFileManager::Get().FileSize(*Path);
		if (Size <= 0)
		{
			return FString();
		}
		if (Size <= HeadBytes + TailBytes)
		{
			return ReadRange(Path, 0, Size);
		}
		return ReadRange(Path, 0, HeadBytes) + TEXT("\n") + ReadRange(Path, Size - TailBytes, TailBytes);
	}

	TSharedPtr<FJsonObject> ParseLine(const FString& Line)
	{
		TSharedPtr<FJsonObject> Object;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Line);
		return FJsonSerializer::Deserialize(Reader, Object) ? Object : nullptr;
	}

	FString OneLine(FString Text)
	{
		Text.TrimStartAndEndInline();
		int32 Break = INDEX_NONE;
		if (Text.FindChar(TEXT('\n'), Break))
		{
			Text.LeftInline(Break);
		}
		Text.ReplaceInline(TEXT("\r"), TEXT(""));
		Text.TrimEndInline();
		return Text.Len() > MaxTitleChars ? Text.Left(MaxTitleChars - 3) + TEXT("...") : Text;
	}

	/** Text of a message content field that is either a string or an array of {type, text} parts. */
	FString ContentText(const TSharedPtr<FJsonObject>& Owner, const TCHAR* TextPartType)
	{
		FString Text;
		if (Owner->TryGetStringField(TEXT("content"), Text))
		{
			return Text;
		}
		const TArray<TSharedPtr<FJsonValue>>* Parts = nullptr;
		if (Owner->TryGetArrayField(TEXT("content"), Parts))
		{
			for (const TSharedPtr<FJsonValue>& Part : *Parts)
			{
				const TSharedPtr<FJsonObject> PartObject = Part.IsValid() ? Part->AsObject() : nullptr;
				FString Type;
				if (PartObject && PartObject->TryGetStringField(TEXT("type"), Type) && Type == TextPartType
					&& PartObject->TryGetStringField(TEXT("text"), Text))
				{
					return Text;
				}
			}
		}
		return FString();
	}

	bool IsTypedPrompt(const FString& Text)
	{
		const FString Trimmed = Text.TrimStart();
		// Injected instructions, environment blocks, command caveats and slash commands are not what the user asked.
		return !Trimmed.IsEmpty() && !Trimmed.StartsWith(TEXT("<")) && !Trimmed.StartsWith(TEXT("#"))
			&& !Trimmed.StartsWith(TEXT("/"));
	}

	TArray<FString> FilesNewestFirst(TArray<FString> Files)
	{
		TMap<FString, FDateTime> Stamps;
		for (const FString& File : Files)
		{
			Stamps.Add(File, IFileManager::Get().GetTimeStamp(*File));
		}
		Files.Sort([&Stamps](const FString& A, const FString& B) { return Stamps[A] > Stamps[B]; });
		return Files;
	}

	void AddClaudeSessions(const FString& Root, const int32 MaxSessions, TArray<FHyperAIStudioChatSession>& Out)
	{
		const FString Directory = ConfigDirectory(TEXT("CLAUDE_CONFIG_DIR"), TEXT(".claude")) / TEXT("projects")
			/ ClaudeProjectDirectoryName(Root);
		TArray<FString> Names;
		IFileManager::Get().FindFiles(Names, *(Directory / TEXT("*.jsonl")), /*Files=*/true, /*Directories=*/false);
		TArray<FString> Paths;
		for (const FString& Name : Names)
		{
			Paths.Add(Directory / Name);
		}
		int32 Found = 0;
		for (const FString& Path : FilesNewestFirst(MoveTemp(Paths)))
		{
			const FString SessionId = FPaths::GetBaseFilename(Path);
			const FString Text = IsSafeSessionId(SessionId) ? ReadHeadAndTail(Path) : FString();
			const FString Title = ExtractClaudeTitle(Text);
			// No title means no typed prompt: an aborted launch, not a conversation worth resuming.
			if (Title.IsEmpty())
			{
				continue;
			}
			Out.Add({TEXT("Claude Code"), SessionId, Title, IFileManager::Get().GetTimeStamp(*Path),
				MentionsUnrealMcpCall(Text)});
			if (++Found >= MaxSessions)
			{
				return;
			}
		}
	}

	void AddCodexSessions(const FString& Root, const int32 MaxSessions, TArray<FHyperAIStudioChatSession>& Out)
	{
		const FString CodexHome = ConfigDirectory(TEXT("CODEX_HOME"), TEXT(".codex"));
		TMap<FString, FString> ThreadNames;
		FString Index;
		if (FFileHelper::LoadFileToString(Index, *(CodexHome / TEXT("session_index.jsonl"))))
		{
			TArray<FString> Lines;
			Index.ParseIntoArrayLines(Lines);
			for (const FString& Line : Lines)
			{
				FString Id;
				FString Name;
				const TSharedPtr<FJsonObject> Object = ParseLine(Line);
				if (Object && Object->TryGetStringField(TEXT("id"), Id) && Object->TryGetStringField(TEXT("thread_name"), Name))
				{
					ThreadNames.Add(Id, Name);
				}
			}
		}

		TArray<FString> Paths;
		IFileManager::Get().FindFilesRecursive(Paths, *(CodexHome / TEXT("sessions")), TEXT("rollout-*.jsonl"), true, false);
		int32 Found = 0;
		for (const FString& Path : FilesNewestFirst(MoveTemp(Paths)))
		{
			const FString Head = ReadRange(Path, 0, FMath::Min(HeadBytes, IFileManager::Get().FileSize(*Path)));
			int32 Break = INDEX_NONE;
			const TSharedPtr<FJsonObject> Meta = Head.FindChar(TEXT('\n'), Break) ? ParseLine(Head.Left(Break)) : nullptr;
			const TSharedPtr<FJsonObject>* Payload = nullptr;
			FString Id;
			FString Cwd;
			if (!Meta || !Meta->TryGetObjectField(TEXT("payload"), Payload)
				|| !(*Payload)->TryGetStringField(TEXT("id"), Id) || !(*Payload)->TryGetStringField(TEXT("cwd"), Cwd)
				|| !IsSafeSessionId(Id) || !NormalizeRoot(Cwd).Equals(Root, ESearchCase::IgnoreCase))
			{
				continue;
			}
			const FString* ThreadName = ThreadNames.Find(Id);
			const FString Title = OneLine(ThreadName ? *ThreadName : ExtractCodexFirstPrompt(Head));
			if (Title.IsEmpty())
			{
				continue;
			}
			Out.Add({TEXT("Codex"), Id, Title, IFileManager::Get().GetTimeStamp(*Path), MentionsUnrealMcpCall(Head)});
			if (++Found >= MaxSessions)
			{
				return;
			}
		}
	}
}

TArray<FHyperAIStudioChatSession> ListSessions(const FString& ProjectRoot, const int32 MaxSessions)
{
	TArray<FHyperAIStudioChatSession> Sessions;
	if (ProjectRoot.IsEmpty() || MaxSessions <= 0)
	{
		return Sessions;
	}
	const FString Root = NormalizeRoot(ProjectRoot);
	AddClaudeSessions(Root, MaxSessions, Sessions);
	AddCodexSessions(Root, MaxSessions, Sessions);
	Sessions.Sort([](const FHyperAIStudioChatSession& A, const FHyperAIStudioChatSession& B) { return A.LastActiveUtc > B.LastActiveUtc; });
	if (Sessions.Num() > MaxSessions)
	{
		Sessions.SetNum(MaxSessions);
	}
	return Sessions;
}

FString BuildResumeArguments(const FString& AgentName, const FString& SessionId)
{
	if (!IsSafeSessionId(SessionId))
	{
		return FString();
	}
	if (AgentName == TEXT("Claude Code"))
	{
		return TEXT("--resume ") + SessionId;
	}
	// Codex applies the root -c overrides to its resume subcommand.
	return AgentName == TEXT("Codex") ? TEXT("resume ") + SessionId : FString();
}

bool IsSafeSessionId(const FString& SessionId)
{
	// The id lands in a cmd.exe command line: hex digits and dashes only.
	return SessionId.Len() >= 8 && SessionId.Len() <= 64
		&& Algo::AllOf(SessionId, [](const TCHAR C) { return FChar::IsHexDigit(C) || C == TEXT('-'); });
}

bool MentionsUnrealMcpCall(const FString& JsonlText)
{
	// A called tool is a JSON name field. The catalogue an agent is shown lists the same names as escaped
	// text inside a prompt string, which this deliberately does not match.
	return JsonlText.Contains(TEXT("\"name\":\"mcp__unreal")) || JsonlText.Contains(TEXT("\"name\":\"mcp__hyper"));
}

FString ClaudeProjectDirectoryName(const FString& ProjectRoot)
{
	FString Name = NormalizeRoot(ProjectRoot);
	for (TCHAR& Character : Name)
	{
		if (!FChar::IsAlnum(Character))
		{
			Character = TEXT('-');
		}
	}
	return Name;
}

FString ExtractClaudeTitle(const FString& JsonlText)
{
	FString CustomTitle;
	FString AiTitle;
	FString FirstPrompt;
	TArray<FString> Lines;
	JsonlText.ParseIntoArrayLines(Lines);
	for (const FString& Line : Lines)
	{
		const bool bCustom = Line.Contains(TEXT("\"custom-title\""));
		const bool bAi = !bCustom && Line.Contains(TEXT("\"ai-title\""));
		const bool bUser = !bCustom && !bAi && FirstPrompt.IsEmpty() && Line.Contains(TEXT("\"type\":\"user\""));
		const TSharedPtr<FJsonObject> Object = bCustom || bAi || bUser ? ParseLine(Line) : nullptr;
		if (!Object)
		{
			continue;
		}
		if (bCustom)
		{
			Object->TryGetStringField(TEXT("customTitle"), CustomTitle);
		}
		else if (bAi)
		{
			Object->TryGetStringField(TEXT("aiTitle"), AiTitle);
		}
		else
		{
			const TSharedPtr<FJsonObject>* Message = nullptr;
			bool bMeta = false;
			Object->TryGetBoolField(TEXT("isMeta"), bMeta);
			if (!bMeta && Object->TryGetObjectField(TEXT("message"), Message))
			{
				const FString Prompt = ContentText(*Message, TEXT("text"));
				if (IsTypedPrompt(Prompt))
				{
					FirstPrompt = Prompt;
				}
			}
		}
	}
	return OneLine(!CustomTitle.IsEmpty() ? CustomTitle : !AiTitle.IsEmpty() ? AiTitle : FirstPrompt);
}

FString ExtractCodexFirstPrompt(const FString& JsonlText)
{
	TArray<FString> Lines;
	JsonlText.ParseIntoArrayLines(Lines);
	for (const FString& Line : Lines)
	{
		if (!Line.Contains(TEXT("\"response_item\"")) || !Line.Contains(TEXT("\"role\":\"user\"")))
		{
			continue;
		}
		const TSharedPtr<FJsonObject> Object = ParseLine(Line);
		const TSharedPtr<FJsonObject>* Payload = nullptr;
		if (Object && Object->TryGetObjectField(TEXT("payload"), Payload))
		{
			const FString Prompt = ContentText(*Payload, TEXT("input_text"));
			if (IsTypedPrompt(Prompt))
			{
				return OneLine(Prompt);
			}
		}
	}
	return FString();
}
}
