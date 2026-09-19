// Games by Hyper 2026.

#include "HyperAIStudioService.h"
#include "HyperAIStudioCapabilityInventoryClient.h"
#include "HyperAIStudioCapabilityPackRegistry.h"
#include "HyperAIStudioCapabilityPreset.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioLegacyMigration.h"
#include "HyperAIStudioManagedConfigMigration.h"
#include "HyperAIStudioMcpSerialQueue.h"
#include "HyperAIStudioOperationJournal.h"

#include "CoreGlobals.h"
#include "AssetRegistry/AssetData.h"
#include "ContentBrowserModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphNode_Comment.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/Selection.h"
#include "Engine/World.h"
#include "Framework/Docking/TabManager.h"
#include "GameFramework/Actor.h"
#include "GraphEditor.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/ThreadSafeCounter.h"
#include "HttpModule.h"
#include "IModelContextProtocolModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Interfaces/IPluginManager.h"
#include "Interfaces/IProjectManager.h"
#include "IContentBrowserSingleton.h"
#include "JsonObjectConverter.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "ModelContextProtocol.h"
#include "ModelContextProtocolServer.h"
#include "ModelContextProtocolSettings.h"
#include "Modules/ModuleManager.h"
#include "ProjectDescriptor.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/Archive.h"
#include "HAL/PlatformProcess.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "Widgets/Docking/SDockTab.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

namespace HyperAIStudio::Private
{
	static const TCHAR* ManagedMarkdownBegin = TEXT("<!-- BEGIN HYPERAISTUDIO MANAGED BLOCK -->");
	static const TCHAR* ManagedMarkdownEnd = TEXT("<!-- END HYPERAISTUDIO MANAGED BLOCK -->");
	static const TCHAR* ManagedWorkspaceGuideBegin = TEXT("<!-- BEGIN HYPERAISTUDIO WORKSPACE GUIDE -->");
	static const TCHAR* ManagedWorkspaceGuideEnd = TEXT("<!-- END HYPERAISTUDIO WORKSPACE GUIDE -->");
	static const TCHAR* ManagedWorkspaceIndexBegin = TEXT("<!-- BEGIN HYPERAISTUDIO WORKSPACE INDEX GUIDE -->");
	static const TCHAR* ManagedWorkspaceIndexEnd = TEXT("<!-- END HYPERAISTUDIO WORKSPACE INDEX GUIDE -->");
	static const TCHAR* ManagedGitIgnoreBegin = TEXT("# BEGIN HYPERAISTUDIO MANAGED RUNTIME IGNORE");
	static const TCHAR* ManagedGitIgnoreEnd = TEXT("# END HYPERAISTUDIO MANAGED RUNTIME IGNORE");
	static const TCHAR* ManagedTomlBegin = TEXT("# BEGIN HYPERAISTUDIO MANAGED MCP");
	static const TCHAR* ManagedTomlEnd = TEXT("# END HYPERAISTUDIO MANAGED MCP");

	void ApplyCapabilityInventoryResult(
		FHyperAIStudioStatus& Status,
		const FHyperAIStudioCapabilityInventoryResult& Inventory)
	{
		Status.bCapabilityInventoryInProgress = false;
		Status.bCapabilityInventoryAvailable = Inventory.bSuccess;
		Status.bToolSearchMode = Inventory.Snapshot.DiscoveryMode == EHyperAIStudioToolDiscoveryMode::ToolSearch;
		Status.bCapabilityInventoryTruncated = Inventory.Snapshot.bTruncated;
		Status.DiscoverableToolsetCount = Inventory.Snapshot.DiscoverableToolsetCount;
		Status.DescribedToolsetCount = Inventory.Snapshot.DescribedToolsets.Num();
		Status.InventoryToolCount = Inventory.Snapshot.DescribedToolCount;
		Status.CapabilityInventoryMessage = Inventory.Message;
		Status.CapabilityInventoryFingerprint = Inventory.Snapshot.InventoryFingerprint;
	}

	FString CleanUrlPath(FString UrlPath)
	{
		UModelContextProtocolSettings::EnforceValidServerUrlPath(UrlPath);
		return UrlPath;
	}

	FString EscapeJsonString(const FString& Value)
	{
		FString Out = Value;
		Out.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
		Out.ReplaceInline(TEXT("\""), TEXT("\\\""));
		Out.ReplaceInline(TEXT("\r"), TEXT("\\r"));
		Out.ReplaceInline(TEXT("\n"), TEXT("\\n"));
		return Out;
	}

	void AddExecutableSearchDirectory(TArray<FString>& Directories, FString Directory)
	{
		Directory.TrimStartAndEndInline();
		Directory = Directory.TrimQuotes();
		if (Directory.IsEmpty())
		{
			return;
		}

		Directories.AddUnique(FPaths::ConvertRelativePathToFull(Directory));
	}

	FString GetEnvDirectory(const TCHAR* VariableName)
	{
		FString Value = FPlatformMisc::GetEnvironmentVariable(VariableName);
		Value.TrimStartAndEndInline();
		return Value.TrimQuotes();
	}

	void AddExecutableSearchDirectoriesMatching(TArray<FString>& Directories, const FString& RootDirectory, const FString& Wildcard, const FString& RelativeSuffix = FString())
	{
		if (RootDirectory.IsEmpty() || Wildcard.IsEmpty())
		{
			return;
		}

		TArray<FString> Matches;
		IFileManager::Get().FindFiles(Matches, *FPaths::Combine(RootDirectory, Wildcard), false, true);
		for (const FString& Match : Matches)
		{
			AddExecutableSearchDirectory(Directories, RelativeSuffix.IsEmpty()
				? FPaths::Combine(RootDirectory, Match)
				: FPaths::Combine(RootDirectory, Match, RelativeSuffix));
		}
	}

	void AppendKnownWindowsExecutableDirectories(TArray<FString>& Directories)
	{
#if PLATFORM_WINDOWS
		const FString UserProfile = GetEnvDirectory(TEXT("USERPROFILE"));
		const FString AppData = GetEnvDirectory(TEXT("APPDATA"));
		const FString LocalAppData = GetEnvDirectory(TEXT("LOCALAPPDATA"));
		const FString ProgramData = GetEnvDirectory(TEXT("ProgramData"));
		const FString ProgramFiles = GetEnvDirectory(TEXT("ProgramFiles"));
		const FString ProgramFilesX86 = GetEnvDirectory(TEXT("ProgramFiles(x86)"));

		if (!AppData.IsEmpty())
		{
			AddExecutableSearchDirectory(Directories, FPaths::Combine(AppData, TEXT("npm")));
		}
		if (!LocalAppData.IsEmpty())
		{
			AddExecutableSearchDirectory(Directories, FPaths::Combine(LocalAppData, TEXT("npm")));
			AddExecutableSearchDirectory(Directories, FPaths::Combine(LocalAppData, TEXT("Microsoft"), TEXT("WindowsApps")));
			AddExecutableSearchDirectory(Directories, FPaths::Combine(LocalAppData, TEXT("OpenAI"), TEXT("Codex"), TEXT("bin")));
			AddExecutableSearchDirectoriesMatching(Directories, FPaths::Combine(LocalAppData, TEXT("OpenAI"), TEXT("Codex"), TEXT("bin")), TEXT("*"));
			AddExecutableSearchDirectory(Directories, FPaths::Combine(LocalAppData, TEXT("Programs"), TEXT("Microsoft VS Code"), TEXT("bin")));
			AddExecutableSearchDirectory(Directories, FPaths::Combine(LocalAppData, TEXT("Programs"), TEXT("Cursor"), TEXT("resources"), TEXT("app"), TEXT("bin")));
			AddExecutableSearchDirectory(Directories, FPaths::Combine(LocalAppData, TEXT("Programs"), TEXT("cursor"), TEXT("resources"), TEXT("app"), TEXT("bin")));
			AddExecutableSearchDirectory(Directories, FPaths::Combine(LocalAppData, TEXT("Programs"), TEXT("Claude")));
			AddExecutableSearchDirectory(Directories, FPaths::Combine(LocalAppData, TEXT("Programs"), TEXT("Claude"), TEXT("bin")));
			AddExecutableSearchDirectory(Directories, FPaths::Combine(LocalAppData, TEXT("Programs"), TEXT("Claude"), TEXT("resources"), TEXT("app"), TEXT("bin")));
			AddExecutableSearchDirectory(Directories, FPaths::Combine(LocalAppData, TEXT("Programs"), TEXT("nodejs")));
			AddExecutableSearchDirectory(Directories, FPaths::Combine(LocalAppData, TEXT("Volta"), TEXT("bin")));
			AddExecutableSearchDirectory(Directories, FPaths::Combine(LocalAppData, TEXT("fnm")));
			AddExecutableSearchDirectoriesMatching(Directories, FPaths::Combine(LocalAppData, TEXT("fnm_multishells")), TEXT("*"));
		}
		if (!UserProfile.IsEmpty())
		{
			AddExecutableSearchDirectory(Directories, FPaths::Combine(UserProfile, TEXT(".local"), TEXT("bin")));
			AddExecutableSearchDirectory(Directories, FPaths::Combine(UserProfile, TEXT(".claude"), TEXT("local")));
			AddExecutableSearchDirectory(Directories, FPaths::Combine(UserProfile, TEXT(".codex"), TEXT("bin")));
			AddExecutableSearchDirectory(Directories, FPaths::Combine(UserProfile, TEXT(".volta"), TEXT("bin")));
			AddExecutableSearchDirectory(Directories, FPaths::Combine(UserProfile, TEXT(".bun"), TEXT("bin")));
			AddExecutableSearchDirectory(Directories, FPaths::Combine(UserProfile, TEXT(".proto"), TEXT("shims")));
		}
		if (!ProgramData.IsEmpty())
		{
			AddExecutableSearchDirectory(Directories, FPaths::Combine(ProgramData, TEXT("chocolatey"), TEXT("bin")));
		}
		if (!ProgramFiles.IsEmpty())
		{
			AddExecutableSearchDirectory(Directories, FPaths::Combine(ProgramFiles, TEXT("Microsoft VS Code"), TEXT("bin")));
			AddExecutableSearchDirectory(Directories, FPaths::Combine(ProgramFiles, TEXT("Cursor"), TEXT("resources"), TEXT("app"), TEXT("bin")));
			AddExecutableSearchDirectory(Directories, FPaths::Combine(ProgramFiles, TEXT("nodejs")));
			AddExecutableSearchDirectory(Directories, FPaths::Combine(ProgramFiles, TEXT("Git"), TEXT("cmd")));
			AddExecutableSearchDirectory(Directories, FPaths::Combine(ProgramFiles, TEXT("Git"), TEXT("bin")));
			AddExecutableSearchDirectoriesMatching(Directories, FPaths::Combine(ProgramFiles, TEXT("WindowsApps")), TEXT("OpenAI.Codex_*"), FPaths::Combine(TEXT("app"), TEXT("resources")));
		}
		if (!ProgramFilesX86.IsEmpty())
		{
			AddExecutableSearchDirectory(Directories, FPaths::Combine(ProgramFilesX86, TEXT("Microsoft VS Code"), TEXT("bin")));
			AddExecutableSearchDirectory(Directories, FPaths::Combine(ProgramFilesX86, TEXT("nodejs")));
			AddExecutableSearchDirectory(Directories, FPaths::Combine(ProgramFilesX86, TEXT("Git"), TEXT("cmd")));
			AddExecutableSearchDirectory(Directories, FPaths::Combine(ProgramFilesX86, TEXT("Git"), TEXT("bin")));
		}
#endif
	}

	FString EscapeTomlString(const FString& Value)
	{
		FString Out = Value;
		Out.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
		Out.ReplaceInline(TEXT("\""), TEXT("\\\""));
		Out.ReplaceInline(TEXT("\r"), TEXT("\\r"));
		Out.ReplaceInline(TEXT("\n"), TEXT("\\n"));
		return Out;
	}

	FString NormalizeServerId(const FString& RawId, const FString& Fallback)
	{
		FString Result;
		const FString Source = RawId.IsEmpty() ? Fallback : RawId;
		for (TCHAR Ch : Source.ToLower())
		{
			if ((Ch >= TEXT('a') && Ch <= TEXT('z')) || (Ch >= TEXT('0') && Ch <= TEXT('9')) || Ch == TEXT('-') || Ch == TEXT('_'))
			{
				Result.AppendChar(Ch);
			}
			else if (Ch == TEXT(' ') || Ch == TEXT('.') || Ch == TEXT('/'))
			{
				Result.AppendChar(TEXT('-'));
			}
		}
		return Result.IsEmpty() ? TEXT("custom-mcp") : Result;
	}

	FHyperAIStudioLegacyProfileMatch RecognizeLegacySettingsEntry(
		const FHyperAIStudioMCPServerEntry& Entry)
	{
		FHyperAIStudioLegacyProfileInput Input;
		Input.ServerId = Entry.Id.IsEmpty() ? Entry.DisplayName : Entry.Id;
		Input.Transport = Entry.Transport == EHyperAIStudioMCPTransport::Command
			? TEXT("command")
			: TEXT("http");
		Input.Command = Entry.Command;
		Input.Arguments = Entry.Arguments;
		Input.Url = Entry.Url;
		return FHyperAIStudioLegacyProfileRecognizer::Recognize(Input);
	}

	bool IsRetiredExactOwnedLegacy(const FHyperAIStudioMCPServerEntry& Entry)
	{
		const FHyperAIStudioLegacyProfileMatch Match = RecognizeLegacySettingsEntry(Entry);
		return FHyperAIStudioLegacyProfileRecognizer::IsExactOwnedRetiredProfile(Match);
	}

	bool IsSupportedTargetClient(const FString& ClientId)
	{
		return ClientId == TEXT("codex")
			|| ClientId == TEXT("claude")
			|| ClientId == TEXT("cursor")
			|| ClientId == TEXT("vscode")
			|| ClientId == TEXT("gemini");
	}

	bool TargetsClient(
		const FHyperAIStudioMCPServerEntry& Entry,
		const FString& ClientId,
		TArray<FString>* Warnings = nullptr)
	{
		if (Entry.TargetClients.IsEmpty())
		{
			return true;
		}

		bool bMatches = false;
		for (FString Target : Entry.TargetClients)
		{
			Target.TrimStartAndEndInline();
			Target = Target.ToLower();
			if (!IsSupportedTargetClient(Target))
			{
				if (Warnings)
				{
					Warnings->AddUnique(FString::Printf(
						TEXT("Unknown TargetClients id '%s' was preserved and ignored."),
						*Target));
				}
				continue;
			}
			bMatches |= Target == ClientId;
		}
		return bMatches;
	}

	bool EnsureDirectoryForFile(const FString& FilePath)
	{
		const FString Directory = FPaths::GetPath(FilePath);
		return IFileManager::Get().MakeDirectory(*Directory, true);
	}

	bool WriteFileIfChanged(const FString& FilePath, const FString& Content)
	{
		EnsureDirectoryForFile(FilePath);

		FString Existing;
		if (FFileHelper::LoadFileToString(Existing, *FilePath) && Existing == Content)
		{
			return true;
		}

		return FFileHelper::SaveStringToFile(Content, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	enum class EManagedTextEncoding : uint8
	{
		Utf8,
		Utf8Bom,
		Utf16LittleEndian
	};

	bool ReadFileBytes(const FString& FilePath, TArray<uint8>& OutBytes)
	{
		OutBytes.Reset();
		return !FPaths::FileExists(FilePath)
			|| FFileHelper::LoadFileToArray(OutBytes, *FilePath, FILEREAD_Silent);
	}

	bool EncodeManagedTextFragment(
		const FString& Text,
		const EManagedTextEncoding Encoding,
		TArray<uint8>& OutBytes)
	{
		OutBytes.Reset();
		if (Encoding == EManagedTextEncoding::Utf16LittleEndian)
		{
			OutBytes.Reserve(Text.Len() * 2);
			for (const TCHAR Character : Text)
			{
				const uint16 CodeUnit = static_cast<uint16>(Character);
				OutBytes.Add(static_cast<uint8>(CodeUnit & 0xff));
				OutBytes.Add(static_cast<uint8>(CodeUnit >> 8));
			}
			return true;
		}

		const FTCHARToUTF8 Converted(*Text);
		OutBytes.Append(reinterpret_cast<const uint8*>(Converted.Get()), Converted.Length());
		return true;
	}

	bool DecodeManagedText(
		const TArray<uint8>& Bytes,
		FString& OutText,
		EManagedTextEncoding& OutEncoding,
		FString& OutReasonCode)
	{
		OutText.Reset();
		OutReasonCode.Reset();
		if (Bytes.Num() > FHyperAIStudioManagedBlockParser::MaxTextCharacters * 4 + 3)
		{
			OutReasonCode = TEXT("managed_text_input_too_large");
			return false;
		}
		int32 Offset = 0;
		if (Bytes.Num() >= 3 && Bytes[0] == 0xef && Bytes[1] == 0xbb && Bytes[2] == 0xbf)
		{
			OutEncoding = EManagedTextEncoding::Utf8Bom;
			Offset = 3;
		}
		else if (Bytes.Num() >= 2 && Bytes[0] == 0xff && Bytes[1] == 0xfe)
		{
			OutEncoding = EManagedTextEncoding::Utf16LittleEndian;
			Offset = 2;
		}
		else if (Bytes.Num() >= 2 && Bytes[0] == 0xfe && Bytes[1] == 0xff)
		{
			OutReasonCode = TEXT("managed_text_utf16_be_unsupported");
			return false;
		}
		else
		{
			OutEncoding = EManagedTextEncoding::Utf8;
		}

		if (OutEncoding == EManagedTextEncoding::Utf16LittleEndian)
		{
			if ((Bytes.Num() - Offset) % 2 != 0)
			{
				OutReasonCode = TEXT("managed_text_invalid_utf16_length");
				return false;
			}
			OutText.Reserve((Bytes.Num() - Offset) / 2);
			for (int32 Index = Offset; Index < Bytes.Num(); Index += 2)
			{
				const uint16 CodeUnit = static_cast<uint16>(Bytes[Index])
					| (static_cast<uint16>(Bytes[Index + 1]) << 8);
				if (CodeUnit == 0)
				{
					OutReasonCode = TEXT("managed_text_embedded_null");
					return false;
				}
				OutText.AppendChar(static_cast<TCHAR>(CodeUnit));
			}
			return true;
		}

		const int32 Utf8Length = Bytes.Num() - Offset;
		if (Utf8Length == 0)
		{
			return true;
		}
		const FUTF8ToTCHAR Converted(
			reinterpret_cast<const ANSICHAR*>(Bytes.GetData() + Offset),
			Utf8Length);
		OutText = FString(Converted.Length(), Converted.Get());
		for (const TCHAR Character : OutText)
		{
			if (Character == 0)
			{
				OutReasonCode = TEXT("managed_text_embedded_null");
				return false;
			}
		}

		TArray<uint8> RoundTrip;
		EncodeManagedTextFragment(OutText, EManagedTextEncoding::Utf8, RoundTrip);
		if (RoundTrip.Num() != Utf8Length
			|| (Utf8Length > 0
				&& FMemory::Memcmp(RoundTrip.GetData(), Bytes.GetData() + Offset, Utf8Length) != 0))
		{
			OutReasonCode = TEXT("managed_text_invalid_utf8");
			return false;
		}
		return true;
	}

	void FindByteOccurrences(
		const TArray<uint8>& Bytes,
		const TArray<uint8>& Needle,
		TArray<int32>& OutOffsets)
	{
		OutOffsets.Reset();
		if (Needle.IsEmpty() || Needle.Num() > Bytes.Num())
		{
			return;
		}
		for (int32 Offset = 0; Offset <= Bytes.Num() - Needle.Num();)
		{
			if (FMemory::Memcmp(Bytes.GetData() + Offset, Needle.GetData(), Needle.Num()) == 0)
			{
				OutOffsets.Add(Offset);
				Offset += Needle.Num();
			}
			else
			{
				++Offset;
			}
		}
	}

	bool BuildManagedBlockBytes(
		const FString& FilePath,
		const FString& BlockContent,
		const FString& BeginMarker,
		const FString& EndMarker,
		const bool bDesired,
		TArray<uint8>& OutOriginalBytes,
		TArray<uint8>& OutDesiredBytes,
		bool& bOutChanged,
		FString& OutReasonCode)
	{
		const bool bFileExists = FPaths::FileExists(FilePath);
		if (!ReadFileBytes(FilePath, OutOriginalBytes))
		{
			OutReasonCode = TEXT("managed_text_read_failed");
			return false;
		}

		FString ExistingText;
		EManagedTextEncoding Encoding = EManagedTextEncoding::Utf8;
		if (!DecodeManagedText(OutOriginalBytes, ExistingText, Encoding, OutReasonCode))
		{
			return false;
		}

		const FHyperAIStudioManagedBlockParseResult Parsed =
			FHyperAIStudioManagedBlockParser::Parse(ExistingText, BeginMarker, EndMarker);
		if (Parsed.State != EHyperAIStudioManagedBlockState::Missing
			&& Parsed.State != EHyperAIStudioManagedBlockState::ValidSinglePair)
		{
			OutReasonCode = Parsed.ReasonCode;
			return false;
		}

		OutDesiredBytes = OutOriginalBytes;
		const FString ManagedBlock = BeginMarker + LINE_TERMINATOR
			+ BlockContent + LINE_TERMINATOR + EndMarker;
		TArray<uint8> ManagedBlockBytes;
		EncodeManagedTextFragment(ManagedBlock, Encoding, ManagedBlockBytes);

		if (Parsed.State == EHyperAIStudioManagedBlockState::Missing)
		{
			if (!bDesired)
			{
				bOutChanged = false;
				OutReasonCode = TEXT("managed_text_disabled_missing_noop");
				return true;
			}

			FString Separator;
			if (!ExistingText.IsEmpty())
			{
				Separator = ExistingText.EndsWith(TEXT("\n")) || ExistingText.EndsWith(TEXT("\r"))
					? FString(LINE_TERMINATOR)
					: FString(LINE_TERMINATOR LINE_TERMINATOR);
			}
			TArray<uint8> SuffixBytes;
			EncodeManagedTextFragment(Separator + ManagedBlock + LINE_TERMINATOR, Encoding, SuffixBytes);
			OutDesiredBytes.Append(SuffixBytes);
		}
		else
		{
			TArray<uint8> BeginBytes;
			TArray<uint8> EndBytes;
			EncodeManagedTextFragment(BeginMarker, Encoding, BeginBytes);
			EncodeManagedTextFragment(EndMarker, Encoding, EndBytes);
			TArray<int32> BeginOffsets;
			TArray<int32> EndOffsets;
			FindByteOccurrences(OutOriginalBytes, BeginBytes, BeginOffsets);
			FindByteOccurrences(OutOriginalBytes, EndBytes, EndOffsets);
			if (BeginOffsets.Num() != 1 || EndOffsets.Num() != 1 || EndOffsets[0] < BeginOffsets[0])
			{
				OutReasonCode = TEXT("managed_text_byte_marker_mismatch");
				return false;
			}

			const int32 ReplaceBegin = BeginOffsets[0];
			const int32 ReplaceEnd = EndOffsets[0] + EndBytes.Num();
			OutDesiredBytes.Reset(OutOriginalBytes.Num() + ManagedBlockBytes.Num());
			OutDesiredBytes.Append(OutOriginalBytes.GetData(), ReplaceBegin);
			if (bDesired)
			{
				OutDesiredBytes.Append(ManagedBlockBytes);
			}
			OutDesiredBytes.Append(
				OutOriginalBytes.GetData() + ReplaceEnd,
				OutOriginalBytes.Num() - ReplaceEnd);
		}

		bOutChanged = !bFileExists
			|| OutDesiredBytes.Num() != OutOriginalBytes.Num()
			|| (OutDesiredBytes.Num() > 0
				&& FMemory::Memcmp(OutDesiredBytes.GetData(), OutOriginalBytes.GetData(), OutDesiredBytes.Num()) != 0);
		OutReasonCode = bOutChanged ? TEXT("managed_text_change_planned") : TEXT("managed_text_unchanged");
		return true;
	}

	bool WriteBytesAtomically(
		const FString& FilePath,
		const TArray<uint8>& Bytes,
		const TArray<uint8>* ExpectedOriginalBytes = nullptr,
		const bool bExpectedOriginalExists = false)
	{
		if (!EnsureDirectoryForFile(FilePath))
		{
			return false;
		}
		const FString TempPath = FilePath + TEXT(".hyperai.tmp.")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits);

#if PLATFORM_WINDOWS
		HANDLE TempHandle = ::CreateFileW(
			*TempPath,
			GENERIC_WRITE,
			0,
			nullptr,
			CREATE_NEW,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
			nullptr);
		if (TempHandle == INVALID_HANDLE_VALUE)
		{
			return false;
		}

		bool bWriteOk = true;
		int32 Offset = 0;
		while (Offset < Bytes.Num())
		{
			const DWORD Requested = static_cast<DWORD>(FMath::Min<int64>(
				static_cast<int64>(Bytes.Num() - Offset),
				static_cast<int64>(MAXDWORD)));
			DWORD Written = 0;
			if (!::WriteFile(TempHandle, Bytes.GetData() + Offset, Requested, &Written, nullptr)
				|| Written != Requested)
			{
				bWriteOk = false;
				break;
			}
			Offset += static_cast<int32>(Written);
		}
		bWriteOk = bWriteOk && ::FlushFileBuffers(TempHandle) != 0;
		bWriteOk = ::CloseHandle(TempHandle) != 0 && bWriteOk;
		if (!bWriteOk)
		{
			IFileManager::Get().Delete(*TempPath, false, true, true);
			return false;
		}

		const bool bTargetExists = FPaths::FileExists(FilePath);
		if (ExpectedOriginalBytes)
		{
			TArray<uint8> CurrentBytes;
			if (bTargetExists != bExpectedOriginalExists
				|| !ReadFileBytes(FilePath, CurrentBytes)
				|| CurrentBytes != *ExpectedOriginalBytes)
			{
				IFileManager::Get().Delete(*TempPath, false, true, true);
				return false;
			}
		}
		const bool bReplaceOk = bTargetExists
			? ::ReplaceFileW(
				*FilePath,
				*TempPath,
				nullptr,
				REPLACEFILE_WRITE_THROUGH,
				nullptr,
				nullptr) != 0
			: ::MoveFileExW(*TempPath, *FilePath, MOVEFILE_WRITE_THROUGH) != 0;
		if (!bReplaceOk)
		{
			IFileManager::Get().Delete(*TempPath, false, true, true);
			return false;
		}
		return true;
#else
		TUniquePtr<FArchive> Writer(IFileManager::Get().CreateFileWriter(*TempPath));
		if (!Writer)
		{
			return false;
		}
		if (!Bytes.IsEmpty())
		{
			Writer->Serialize(const_cast<uint8*>(Bytes.GetData()), Bytes.Num());
		}
		Writer->Flush();
		const bool bWriteOk = !Writer->IsError() && Writer->Close();
		Writer.Reset();
		const bool bTargetExists = FPaths::FileExists(FilePath);
		if (ExpectedOriginalBytes)
		{
			TArray<uint8> CurrentBytes;
			if (bTargetExists != bExpectedOriginalExists
				|| !ReadFileBytes(FilePath, CurrentBytes)
				|| CurrentBytes != *ExpectedOriginalBytes)
			{
				IFileManager::Get().Delete(*TempPath, false, true, true);
				return false;
			}
		}
		if (!bWriteOk || !IFileManager::Get().Move(*FilePath, *TempPath, true, true, false, true))
		{
			IFileManager::Get().Delete(*TempPath, false, true, true);
			return false;
		}
		return true;
	#endif
	}

	bool MigrateLegacyManagedFile(
		const FString& LegacyPath,
		const FString& OrganizedPath,
		FString& OutReason)
	{
		OutReason.Reset();
		if (!FPaths::FileExists(LegacyPath))
		{
			return true;
		}

		TArray<uint8> LegacyBytes;
		if (!ReadFileBytes(LegacyPath, LegacyBytes))
		{
			OutReason = TEXT("legacy_workspace_file_read_failed");
			return false;
		}

		if (FPaths::FileExists(OrganizedPath))
		{
			TArray<uint8> OrganizedBytes;
			if (!ReadFileBytes(OrganizedPath, OrganizedBytes))
			{
				OutReason = TEXT("organized_workspace_file_read_failed");
				return false;
			}
			if (OrganizedBytes != LegacyBytes)
			{
				OutReason = TEXT("workspace_layout_migration_conflict");
				return false;
			}
		}
		else
		{
			const TArray<uint8> MissingBytes;
			if (!WriteBytesAtomically(OrganizedPath, LegacyBytes, &MissingBytes, false))
			{
				OutReason = TEXT("organized_workspace_file_write_failed");
				return false;
			}
		}

		TArray<uint8> VerifiedBytes;
		if (!ReadFileBytes(OrganizedPath, VerifiedBytes) || VerifiedBytes != LegacyBytes)
		{
			OutReason = TEXT("workspace_layout_migration_verification_failed");
			return false;
		}
		if (!IFileManager::Get().Delete(*LegacyPath, false, true, true))
		{
			OutReason = TEXT("legacy_workspace_file_delete_failed");
			return false;
		}
		return true;
	}

	bool SerializeSettingsEvidenceBytes(
		const FHyperAIStudioManagedConfigSettingsTransaction& Evidence,
		TArray<uint8>& OutBytes,
		FString& OutReasonCode)
	{
		FString Json;
		if (!FHyperAIStudioManagedConfigSettingsCodec::Serialize(Evidence, Json, OutReasonCode))
		{
			return false;
		}
		Json += LINE_TERMINATOR;
		return EncodeManagedTextFragment(Json, EManagedTextEncoding::Utf8, OutBytes);
	}

	bool LoadSettingsEvidence(
		const FString& EvidencePath,
		FHyperAIStudioManagedConfigSettingsTransaction& OutEvidence,
		TArray<uint8>& OutBytes,
		bool& bOutExists,
		FString& OutReasonCode)
	{
		OutEvidence = {};
		OutBytes.Reset();
		bOutExists = FPaths::FileExists(EvidencePath);
		if (!ReadFileBytes(EvidencePath, OutBytes))
		{
			OutReasonCode = TEXT("settings_evidence_read_failed");
			return false;
		}
		if (bOutExists != FPaths::FileExists(EvidencePath))
		{
			OutReasonCode = TEXT("settings_evidence_existence_changed");
			return false;
		}
		if (!bOutExists)
		{
			OutReasonCode = TEXT("settings_evidence_missing_idle");
			return true;
		}
		FString Text;
		EManagedTextEncoding Encoding = EManagedTextEncoding::Utf8;
		if (!DecodeManagedText(OutBytes, Text, Encoding, OutReasonCode)
			|| !FHyperAIStudioManagedConfigSettingsCodec::Parse(Text, OutEvidence, OutReasonCode))
		{
			return false;
		}
		return true;
	}

	bool ReloadSettingsFromDisk(
		UHyperAIStudioSettings* Settings,
		const FString& ConfigPath)
	{
		if (!Settings || !GConfig || ConfigPath.IsEmpty())
		{
			return false;
		}
		GConfig->UnloadFile(ConfigPath);
		GConfig->LoadFile(ConfigPath);
		Settings->ReloadConfig(Settings->GetClass(), *ConfigPath);
		return true;
	}

	bool ClearSettingsEvidence(
		const FString& EvidencePath,
		const TArray<uint8>& ExpectedEvidenceBytes,
		const bool bExpectedEvidenceExists,
		FString& OutReasonCode)
	{
		FHyperAIStudioManagedConfigSettingsTransaction Idle;
		TArray<uint8> IdleBytes;
		if (!SerializeSettingsEvidenceBytes(Idle, IdleBytes, OutReasonCode)
			|| !WriteBytesAtomically(
				EvidencePath,
				IdleBytes,
				&ExpectedEvidenceBytes,
				bExpectedEvidenceExists))
		{
			OutReasonCode = TEXT("settings_evidence_clear_failed");
			return false;
		}
		return true;
	}

	bool ReconcileSettingsEvidenceAtEntry(
		UHyperAIStudioSettings* Settings,
		const FString& ConfigPath,
		const FString& EvidencePath,
		FString& OutReasonCode)
	{
		FHyperAIStudioManagedConfigSettingsTransaction Evidence;
		TArray<uint8> EvidenceBytes;
		bool bEvidenceExists = false;
		if (!LoadSettingsEvidence(
			EvidencePath,
			Evidence,
			EvidenceBytes,
			bEvidenceExists,
			OutReasonCode))
		{
			return false;
		}
		if (!bEvidenceExists || Evidence.IsIdle())
		{
			return true;
		}

		const bool bCurrentExists = FPaths::FileExists(ConfigPath);
		TArray<uint8> CurrentBytes;
		if (!ReadFileBytes(ConfigPath, CurrentBytes)
			|| bCurrentExists != FPaths::FileExists(ConfigPath))
		{
			OutReasonCode = TEXT("settings_evidence_current_read_failed");
			return false;
		}
		const FString CurrentHash = bCurrentExists
			? FHyperAIStudioManagedConfigPendingCodec::HashBytes(CurrentBytes)
			: FString();
		const EHyperAIStudioTransactionEvidenceMatch Match =
			FHyperAIStudioManagedConfigSettingsCodec::Classify(
				Evidence,
				bCurrentExists,
				CurrentHash);
		if (Match == EHyperAIStudioTransactionEvidenceMatch::Conflict)
		{
			ReloadSettingsFromDisk(Settings, ConfigPath);
			OutReasonCode = TEXT("settings_evidence_current_state_conflict");
			return false;
		}
		if (!ClearSettingsEvidence(
			EvidencePath,
			EvidenceBytes,
			true,
			OutReasonCode))
		{
			ReloadSettingsFromDisk(Settings, ConfigPath);
			return false;
		}
		if (!ReloadSettingsFromDisk(Settings, ConfigPath))
		{
			OutReasonCode = TEXT("settings_evidence_reload_failed");
			return false;
		}
		OutReasonCode = Match == EHyperAIStudioTransactionEvidenceMatch::ExactDesired
			? TEXT("settings_evidence_completed_staged_commit")
			: TEXT("settings_evidence_confirmed_original_retry");
		return true;
	}

	bool SaveSettingsConfigAtomically(
		UHyperAIStudioSettings* Settings,
		const FString& ConfigPath,
		const FString& EvidencePath,
		const TArray<uint8>& ExpectedOriginalBytes,
		const bool bExpectedOriginalExists,
		TArray<uint8>& OutCommittedBytes,
		FString& OutReasonCode,
		bool& bOutCommitOutcomeUnknown)
	{
		OutCommittedBytes.Reset();
		OutReasonCode.Reset();
		bOutCommitOutcomeUnknown = false;
		if (!Settings || ConfigPath.IsEmpty())
		{
			OutReasonCode = TEXT("settings_config_invalid_target");
			return false;
		}
		if (FParse::Param(FCommandLine::Get(), TEXT("nowrite"))
			|| (FParse::Param(FCommandLine::Get(), TEXT("Multiprocess"))
				&& !FParse::Param(FCommandLine::Get(), TEXT("MultiprocessSaveConfig"))))
		{
			// UE's config writer deliberately reports success without touching disk in
			// these modes. Fail explicitly instead of misreporting a readback mismatch.
			OutReasonCode = TEXT("settings_config_writes_disabled_by_command_line");
			return false;
		}

		TArray<uint8> OriginalBytes;
		const bool bOriginalExisted = FPaths::FileExists(ConfigPath);
		if (bOriginalExisted != bExpectedOriginalExists)
		{
			OutReasonCode = TEXT("settings_config_existence_changed_after_preflight");
			return false;
		}
		if (!ReadFileBytes(ConfigPath, OriginalBytes))
		{
			OutReasonCode = TEXT("settings_config_read_failed");
			return false;
		}
		if (OriginalBytes != ExpectedOriginalBytes)
		{
			OutReasonCode = TEXT("settings_config_changed_after_preflight");
			return false;
		}
		FHyperAIStudioManagedConfigSettingsTransaction ExistingEvidence;
		TArray<uint8> ExistingEvidenceBytes;
		bool bExistingEvidenceExists = false;
		if (!LoadSettingsEvidence(
			EvidencePath,
			ExistingEvidence,
			ExistingEvidenceBytes,
			bExistingEvidenceExists,
			OutReasonCode)
			|| (bExistingEvidenceExists && !ExistingEvidence.IsIdle()))
		{
			if (OutReasonCode.IsEmpty())
			{
				OutReasonCode = TEXT("settings_evidence_not_idle_before_replace");
			}
			return false;
		}

		// FConfigCacheIni resolves arbitrary path-backed branches through OtherFiles
		// only when the filename ends in .ini.
		const FString StagingPath = ConfigPath + TEXT(".hyperai.settings.")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".ini");
		auto CleanupStaging = [&StagingPath]()
		{
			IFileManager::Get().Delete(*StagingPath, false, true, true);
		};
		if (!WriteBytesAtomically(StagingPath, OriginalBytes))
		{
			OutReasonCode = TEXT("settings_config_stage_copy_failed");
			CleanupStaging();
			return false;
		}

		// Serialize through an isolated config cache. UObject::TryUpdateDefaultConfigFile
		// flushes and reloads the class' live config even when given a staging path,
		// which would mutate GEditorPerProjectIni before our durable commit barrier.
		FConfigCacheIni StagingConfig(EConfigCacheType::DiskBacked);
		FConfigFile& StagingFile = StagingConfig.Add(StagingPath, FConfigFile());
		StagingFile.Read(StagingPath);
		const FString SectionName = Settings->GetClass()->GetPathName();
		Settings->SaveConfig(CPF_Config, *StagingPath, &StagingConfig, false);
		// This is the transaction's commit token. Write it explicitly after the
		// full object serialization so the staged version is unambiguous.
		StagingConfig.SetInt(
			*SectionName,
			TEXT("NativeToolArchitectureMigrationVersion"),
			Settings->NativeToolArchitectureMigrationVersion,
			StagingPath);
		if (!StagingConfig.Flush(true, *StagingPath))
		{
			OutReasonCode = TEXT("settings_config_stage_flush_failed");
			CleanupStaging();
			return false;
		}

		FConfigFile VerificationFile;
		VerificationFile.Read(StagingPath);
		const FConfigSection* VerificationSection = VerificationFile.FindSection(SectionName);
		int32 StagedMigrationVersion = 0;
		const bool bHasStagedMigrationVersion = VerificationSection
			&& VerificationSection->GetInt(
				TEXT("NativeToolArchitectureMigrationVersion"),
				StagedMigrationVersion);
		if (!VerificationSection)
		{
			OutReasonCode = TEXT("settings_config_stage_section_missing");
			CleanupStaging();
			return false;
		}
		if (!bHasStagedMigrationVersion
			&& Settings->NativeToolArchitectureMigrationVersion != 0)
		{
			OutReasonCode = TEXT("settings_config_stage_migration_version_missing_or_invalid");
			CleanupStaging();
			return false;
		}
		if (bHasStagedMigrationVersion
			&& StagedMigrationVersion != Settings->NativeToolArchitectureMigrationVersion)
		{
			OutReasonCode = TEXT("settings_config_stage_migration_version_mismatch");
			CleanupStaging();
			return false;
		}
		auto ValidateStagedBool = [VerificationSection, &OutReasonCode](
			const TCHAR* Key,
			const bool Expected,
			const TCHAR* ReasonCode)
		{
			bool Actual = false;
			if (!VerificationSection->GetBool(Key, Actual) || Actual != Expected)
			{
				OutReasonCode = ReasonCode;
				return false;
			}
			return true;
		};
		if (!ValidateStagedBool(
				TEXT("bGenerateCodexConfig"),
				Settings->bGenerateCodexConfig,
				TEXT("settings_config_stage_generate_codex_mismatch"))
			|| !ValidateStagedBool(
				TEXT("bGenerateClaudeConfig"),
				Settings->bGenerateClaudeConfig,
				TEXT("settings_config_stage_generate_claude_mismatch"))
			|| !ValidateStagedBool(
				TEXT("bGenerateClaudeMarkdown"),
				Settings->bGenerateClaudeMarkdown,
				TEXT("settings_config_stage_generate_claude_markdown_mismatch"))
			|| !ValidateStagedBool(
				TEXT("bGenerateCursorConfig"),
				Settings->bGenerateCursorConfig,
				TEXT("settings_config_stage_generate_cursor_mismatch"))
			|| !ValidateStagedBool(
				TEXT("bGenerateVSCodeConfig"),
				Settings->bGenerateVSCodeConfig,
				TEXT("settings_config_stage_generate_vscode_mismatch"))
			|| !ValidateStagedBool(
				TEXT("bGenerateGeminiConfig"),
				Settings->bGenerateGeminiConfig,
				TEXT("settings_config_stage_generate_gemini_mismatch")))
		{
			CleanupStaging();
			return false;
		}

		const FArrayProperty* ExtraServersProperty = FindFProperty<FArrayProperty>(
			Settings->GetClass(),
			GET_MEMBER_NAME_CHECKED(UHyperAIStudioSettings, ExtraServers));
		if (!ExtraServersProperty)
		{
			OutReasonCode = TEXT("settings_config_extra_servers_property_missing");
			CleanupStaging();
			return false;
		}
		TArray<FString> ExpectedExtraServers;
		FScriptArrayHelper_InContainer ExtraServersHelper(ExtraServersProperty, Settings);
		ExpectedExtraServers.Reserve(ExtraServersHelper.Num());
		for (int32 Index = 0; Index < ExtraServersHelper.Num(); ++Index)
		{
			FString Exported;
			ExtraServersProperty->Inner->ExportTextItem_Direct(
				Exported,
				ExtraServersHelper.GetRawPtr(Index),
				ExtraServersHelper.GetRawPtr(Index),
				Settings,
				PPF_SerializedAsImportText);
			ExpectedExtraServers.Add(MoveTemp(Exported));
		}
		TArray<FString> StagedExtraServers;
		VerificationSection->GetArray(TEXT("ExtraServers"), StagedExtraServers);
		if (StagedExtraServers != ExpectedExtraServers)
		{
			OutReasonCode = TEXT("settings_config_stage_extra_servers_mismatch");
			CleanupStaging();
			return false;
		}

		TArray<uint8> StagedBytes;
		if (!ReadFileBytes(StagingPath, StagedBytes) || StagedBytes.IsEmpty())
		{
			OutReasonCode = TEXT("settings_config_staged_bytes_unavailable");
			CleanupStaging();
			return false;
		}
		if (bOriginalExisted && StagedBytes == OriginalBytes)
		{
			CleanupStaging();
			OutCommittedBytes = OriginalBytes;
			OutReasonCode = TEXT("settings_config_already_exact");
			return true;
		}

		FHyperAIStudioManagedConfigSettingsTransaction PreparedEvidence;
		PreparedEvidence.TransactionId = FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
		PreparedEvidence.Phase = TEXT("settings_replace_prepared");
		PreparedEvidence.bOriginalExists = bOriginalExisted;
		PreparedEvidence.OriginalFileHash = bOriginalExisted
			? FHyperAIStudioManagedConfigPendingCodec::HashBytes(OriginalBytes)
			: FString();
		PreparedEvidence.StagedFileHash = FHyperAIStudioManagedConfigPendingCodec::HashBytes(StagedBytes);
		TArray<uint8> PreparedEvidenceBytes;
		if (!SerializeSettingsEvidenceBytes(PreparedEvidence, PreparedEvidenceBytes, OutReasonCode)
			|| !WriteBytesAtomically(
				EvidencePath,
				PreparedEvidenceBytes,
				&ExistingEvidenceBytes,
				bExistingEvidenceExists))
		{
			OutReasonCode = TEXT("settings_evidence_prepare_failed");
			CleanupStaging();
			return false;
		}

		if (!WriteBytesAtomically(ConfigPath, StagedBytes, &OriginalBytes, bOriginalExisted))
		{
			const bool bCurrentExists = FPaths::FileExists(ConfigPath);
			TArray<uint8> CurrentBytes;
			const bool bCurrentRead = ReadFileBytes(ConfigPath, CurrentBytes)
				&& bCurrentExists == FPaths::FileExists(ConfigPath);
			const FString CurrentHash = bCurrentExists && bCurrentRead
				? FHyperAIStudioManagedConfigPendingCodec::HashBytes(CurrentBytes)
				: FString();
			const EHyperAIStudioTransactionEvidenceMatch Match = bCurrentRead
				? FHyperAIStudioManagedConfigSettingsCodec::Classify(
					PreparedEvidence,
					bCurrentExists,
					CurrentHash)
				: EHyperAIStudioTransactionEvidenceMatch::Conflict;
			if (Match == EHyperAIStudioTransactionEvidenceMatch::ExactOriginal)
			{
				if (!ClearSettingsEvidence(
					EvidencePath,
					PreparedEvidenceBytes,
					true,
					OutReasonCode))
				{
					bOutCommitOutcomeUnknown = true;
				}
				else
				{
					OutReasonCode = TEXT("settings_config_durable_replace_failed");
				}
			}
			else
			{
				bOutCommitOutcomeUnknown = true;
				OutReasonCode = Match == EHyperAIStudioTransactionEvidenceMatch::ExactDesired
					? TEXT("settings_config_replace_reported_failure_but_staged_state_exists")
					: TEXT("settings_config_replace_failure_state_conflict");
			}
			CleanupStaging();
			ReloadSettingsFromDisk(Settings, ConfigPath);
			return false;
		}
		TArray<uint8> CommittedBytes;
		if (!FPaths::FileExists(ConfigPath)
			|| !ReadFileBytes(ConfigPath, CommittedBytes)
			|| CommittedBytes != StagedBytes)
		{
			// ReplaceFileW/MoveFileExW already reported success. Do not claim the old
			// value or flush a stale GConfig branch over an externally changed result.
			bOutCommitOutcomeUnknown = true;
			OutReasonCode = TEXT("settings_config_readback_mismatch");
			CleanupStaging();
			ReloadSettingsFromDisk(Settings, ConfigPath);
			return false;
		}
		if (!ClearSettingsEvidence(
			EvidencePath,
			PreparedEvidenceBytes,
			true,
			OutReasonCode))
		{
			bOutCommitOutcomeUnknown = true;
			CleanupStaging();
			ReloadSettingsFromDisk(Settings, ConfigPath);
			return false;
		}

		CleanupStaging();
		if (!ReloadSettingsFromDisk(Settings, ConfigPath))
		{
			bOutCommitOutcomeUnknown = true;
			OutReasonCode = TEXT("settings_config_reload_failed_after_commit");
			return false;
		}
		OutCommittedBytes = MoveTemp(CommittedBytes);
		OutReasonCode = TEXT("settings_config_committed");
		return true;
	}

	bool UpsertManagedBlock(const FString& FilePath, const FString& BlockContent, const FString& BeginMarker, const FString& EndMarker)
	{
		TArray<uint8> OriginalBytes;
		TArray<uint8> DesiredBytes;
		bool bChanged = false;
		FString ReasonCode;
		return BuildManagedBlockBytes(
			FilePath,
			BlockContent,
			BeginMarker,
			EndMarker,
			true,
			OriginalBytes,
			DesiredBytes,
			bChanged,
			ReasonCode)
			&& (!bChanged || WriteBytesAtomically(FilePath, DesiredBytes));
	}

	bool RemoveManagedBlock(
		const FString& FilePath,
		const FString& BeginMarker,
		const FString& EndMarker)
	{
		TArray<uint8> OriginalBytes;
		TArray<uint8> DesiredBytes;
		bool bChanged = false;
		FString ReasonCode;
		return BuildManagedBlockBytes(
			FilePath,
			FString(),
			BeginMarker,
			EndMarker,
			false,
			OriginalBytes,
			DesiredBytes,
			bChanged,
			ReasonCode)
			&& (!bChanged || WriteBytesAtomically(FilePath, DesiredBytes));
	}

	bool SerializeJsonToFile(const FString& FilePath, const TSharedRef<FJsonObject>& RootObject)
	{
		FString Output;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
		if (!FJsonSerializer::Serialize(RootObject, Writer))
		{
			return false;
		}
		Output += LINE_TERMINATOR;
		return WriteFileIfChanged(FilePath, Output);
	}

	FString SerializeJsonObjectToString(const TSharedRef<FJsonObject>& Object)
	{
		FString Output;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
		FJsonSerializer::Serialize(Object, Writer);
		return Output;
	}

	TArray<FString> ReadStringArrayField(const TSharedRef<FJsonObject>& Object, const FString& FieldName)
	{
		TArray<FString> Result;
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Object->TryGetArrayField(FieldName, Values) || !Values)
		{
			return Result;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			FString StringValue;
			if (Value.IsValid() && Value->TryGetString(StringValue) && !StringValue.IsEmpty())
			{
				Result.Add(StringValue);
			}
		}
		return Result;
	}

	FString JoinReadable(const TArray<FString>& Values, const FString& EmptyText)
	{
		return Values.Num() == 0 ? EmptyText : FString::Join(Values, TEXT(", "));
	}

	TSharedRef<FJsonObject> MakeHttpServerObject(const FString& Url, bool bGemini)
	{
		TSharedRef<FJsonObject> ServerObject = MakeShared<FJsonObject>();
		if (bGemini)
		{
			ServerObject->SetStringField(TEXT("httpUrl"), Url);
		}
		else
		{
			ServerObject->SetStringField(TEXT("type"), TEXT("http"));
			ServerObject->SetStringField(TEXT("url"), Url);
		}
		return ServerObject;
	}

	TSharedRef<FJsonObject> MakeCommandServerObject(const FHyperAIStudioMCPServerEntry& Entry)
	{
		TSharedRef<FJsonObject> ServerObject = MakeShared<FJsonObject>();
		ServerObject->SetStringField(TEXT("command"), Entry.Command);

		TArray<TSharedPtr<FJsonValue>> Args;
		for (const FString& Argument : Entry.Arguments)
		{
			Args.Add(MakeShared<FJsonValueString>(Argument));
		}
		ServerObject->SetArrayField(TEXT("args"), Args);
		return ServerObject;
	}

	FString BuildCodexTomlBlock(const FString& Endpoint, const UHyperAIStudioSettings* Settings)
	{
		FString TomlBlock;
		TomlBlock += TEXT("# Generated by HyperAIStudio. Edit through Tools > HyperAIStudio or Editor Preferences > HyperAIStudio.") LINE_TERMINATOR;
		TomlBlock += TEXT("[mcp_servers.\"unreal-mcp\"]") LINE_TERMINATOR;
		TomlBlock += FString::Printf(TEXT("url = \"%s\"%s"), *EscapeTomlString(Endpoint), LINE_TERMINATOR);
		TomlBlock += TEXT("startup_timeout_sec = 30") LINE_TERMINATOR;
		TomlBlock += TEXT("tool_timeout_sec = 300") LINE_TERMINATOR;

		if (!Settings)
		{
			return TomlBlock;
		}

		TArray<FHyperAIStudioMCPServerEntry> SortedServers = Settings->ExtraServers;
		SortedServers.Sort([](const FHyperAIStudioMCPServerEntry& A, const FHyperAIStudioMCPServerEntry& B)
		{
			return A.Priority < B.Priority;
		});

		for (const FHyperAIStudioMCPServerEntry& Entry : SortedServers)
		{
			if (!Entry.bEnabled
				|| IsRetiredExactOwnedLegacy(Entry)
				|| !TargetsClient(Entry, TEXT("codex")))
			{
				continue;
			}

			const FString ServerId = NormalizeServerId(Entry.Id, Entry.DisplayName);
			TomlBlock += LINE_TERMINATOR;
			TomlBlock += FString::Printf(TEXT("[mcp_servers.\"%s\"]%s"), *EscapeTomlString(ServerId), LINE_TERMINATOR);
			if (Entry.Transport == EHyperAIStudioMCPTransport::StreamableHttp)
			{
				TomlBlock += FString::Printf(TEXT("url = \"%s\"%s"), *EscapeTomlString(Entry.Url), LINE_TERMINATOR);
			}
			else
			{
				TomlBlock += FString::Printf(TEXT("command = \"%s\"%s"), *EscapeTomlString(Entry.Command), LINE_TERMINATOR);
				if (Entry.Arguments.Num() > 0)
				{
					TomlBlock += TEXT("args = [");
					for (int32 Index = 0; Index < Entry.Arguments.Num(); ++Index)
					{
						if (Index > 0)
						{
							TomlBlock += TEXT(", ");
						}
						TomlBlock += FString::Printf(TEXT("\"%s\""), *EscapeTomlString(Entry.Arguments[Index]));
					}
					TomlBlock += TEXT("]") LINE_TERMINATOR;
				}
			}
		}

		return TomlBlock;
	}

	FString BuildManagedCodexTomlPatch(const FString& Endpoint, const UHyperAIStudioSettings* Settings)
	{
		return FString::Printf(
			TEXT("%s%s%s%s%s%s"),
			ManagedTomlBegin,
			LINE_TERMINATOR,
			*BuildCodexTomlBlock(Endpoint, Settings),
			LINE_TERMINATOR,
			ManagedTomlEnd,
			LINE_TERMINATOR);
	}

	FString ExtractJsonPayloadFromResponse(const FString& Content)
	{
		const FString Trimmed = Content.TrimStartAndEnd();
		if (Trimmed.StartsWith(TEXT("{")) || Trimmed.StartsWith(TEXT("[")))
		{
			return Trimmed;
		}

		TArray<FString> Lines;
		Trimmed.ParseIntoArrayLines(Lines);
		TArray<FString> DataLines;
		for (const FString& Line : Lines)
		{
			const FString CleanLine = Line.TrimStartAndEnd();
			if (CleanLine.StartsWith(TEXT("data:")))
			{
				const FString Data = CleanLine.RightChop(5).TrimStartAndEnd();
				if (!Data.IsEmpty() && Data != TEXT("[DONE]"))
				{
					DataLines.Add(Data);
				}
			}
		}

		return DataLines.IsEmpty() ? Trimmed : FString::Join(DataLines, LINE_TERMINATOR).TrimStartAndEnd();
	}

	bool DeserializeJsonObjectFromResponse(const FString& Content, TSharedPtr<FJsonObject>& OutObject)
	{
		const FString JsonPayload = ExtractJsonPayloadFromResponse(Content);
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonPayload);
		return FJsonSerializer::Deserialize(Reader, OutObject) && OutObject.IsValid();
	}

	bool ExtractToolNamesFromToolsList(const FString& Content, TArray<FString>& OutToolNames, FString& OutMessage)
	{
		TSharedPtr<FJsonObject> RootObject;
		if (!DeserializeJsonObjectFromResponse(Content, RootObject))
		{
			OutMessage = TEXT("tools/list returned non-JSON content.");
			return false;
		}

		const TSharedPtr<FJsonObject>* ErrorObject = nullptr;
		if (RootObject->TryGetObjectField(TEXT("error"), ErrorObject) && ErrorObject && ErrorObject->IsValid())
		{
			FString ErrorMessage;
			(*ErrorObject)->TryGetStringField(TEXT("message"), ErrorMessage);
			OutMessage = ErrorMessage.IsEmpty() ? TEXT("tools/list returned a JSON-RPC error.") : ErrorMessage;
			return false;
		}

		const TSharedPtr<FJsonObject>* ResultObject = nullptr;
		if (!RootObject->TryGetObjectField(TEXT("result"), ResultObject) || !ResultObject || !ResultObject->IsValid())
		{
			OutMessage = TEXT("tools/list response did not contain a result object.");
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* ToolsArray = nullptr;
		if (!(*ResultObject)->TryGetArrayField(TEXT("tools"), ToolsArray))
		{
			OutMessage = TEXT("tools/list response did not contain a tools array.");
			return false;
		}

		for (const TSharedPtr<FJsonValue>& ToolValue : *ToolsArray)
		{
			const TSharedPtr<FJsonObject> ToolObject = ToolValue.IsValid() ? ToolValue->AsObject() : nullptr;
			FString ToolName;
			if (ToolObject.IsValid() && ToolObject->TryGetStringField(TEXT("name"), ToolName) && !ToolName.IsEmpty())
			{
				OutToolNames.Add(ToolName);
			}
		}

		OutMessage = FString::Printf(TEXT("tools/list returned %d advertised tools."), OutToolNames.Num());
		return true;
	}

	FString LimitToolDialogText(const FString& Text)
	{
		constexpr int32 MaxChars = 60000;
		if (Text.Len() <= MaxChars)
		{
			return Text;
		}
		return Text.Left(MaxChars) + LINE_TERMINATOR + TEXT("... truncated for the UI. Use the external MCP client for the full schema.");
	}

	FString CapabilitySafetyText(const TArray<EHyperAIStudioCapabilitySafetyClass>& SafetyClasses)
	{
		TArray<FString> Labels;
		Labels.Reserve(SafetyClasses.Num());
		for (const EHyperAIStudioCapabilitySafetyClass Safety : SafetyClasses)
		{
			switch (Safety)
			{
			case EHyperAIStudioCapabilitySafetyClass::Read: Labels.Add(TEXT("read")); break;
			case EHyperAIStudioCapabilitySafetyClass::Edit: Labels.Add(TEXT("edit")); break;
			case EHyperAIStudioCapabilitySafetyClass::Destructive: Labels.Add(TEXT("destructive")); break;
			case EHyperAIStudioCapabilitySafetyClass::ExternalEffect: Labels.Add(TEXT("external-effect")); break;
			default: Labels.Add(TEXT("unknown")); break;
			}
		}
		return Labels.IsEmpty() ? TEXT("unknown") : FString::Join(Labels, TEXT("/"));
	}

	FString HumanizeToolIdentifier(FString Value)
	{
		Value.ReplaceInline(TEXT("_"), TEXT(" "));
		Value.ReplaceInline(TEXT("."), TEXT(" "));
		Value.ReplaceInline(TEXT("-"), TEXT(" "));
		Value.TrimStartAndEndInline();
		if (!Value.IsEmpty())
		{
			Value[0] = FChar::ToUpper(Value[0]);
		}
		return Value;
	}

	FString HyperAIToolCapabilityLabel(const FString& ToolName)
	{
		static const TSet<FString> LimitedDirectFastEditTools = {
			TEXT("hyper_data_apply_plan"),
			TEXT("hyper_input_apply_plan"),
			TEXT("hyper_paper2d_apply_plan")};
		static const TSet<FString> PlanOnlyTools = {
			TEXT("hyper_actor_modifier_apply_plan"),
			TEXT("hyper_animation_apply_plan"),
			TEXT("hyper_audio_apply_plan"),
			TEXT("hyper_character_apply_plan"),
			TEXT("hyper_cinematics_apply_plan"),
			TEXT("hyper_dynamic_material_apply_plan"),
			TEXT("hyper_game_framework_apply_plan"),
			TEXT("hyper_gameplay_ai_apply_plan"),
			TEXT("hyper_gameplay_systems_apply_plan"),
			TEXT("hyper_gas_apply_plan"),
			TEXT("hyper_geometry_apply_plan"),
			TEXT("hyper_interchange_apply_plan"),
			TEXT("hyper_live_production_apply_plan"),
			TEXT("hyper_material_apply_plan"),
			TEXT("hyper_navigation_apply_plan"),
			TEXT("hyper_network_apply_plan"),
			TEXT("hyper_niagara_apply_plan"),
			TEXT("hyper_pcg_apply_plan"),
			TEXT("hyper_physics_apply_plan"),
			TEXT("hyper_property_animation_apply_plan"),
			TEXT("hyper_scene_apply_plan"),
			TEXT("hyper_ui_apply_plan"),
			TEXT("hyper_worldbuilding_apply_plan")};

		if (LimitedDirectFastEditTools.Contains(ToolName))
		{
			return TEXT("Limited direct Fast edit");
		}
		if (PlanOnlyTools.Contains(ToolName))
		{
			return TEXT("Plan only — use Epic Unreal MCP to apply");
		}
		if (ToolName.Contains(TEXT("validate"), ESearchCase::IgnoreCase))
		{
			return TEXT("Validate");
		}
		if (ToolName.Contains(TEXT("inspect"), ESearchCase::IgnoreCase)
			|| ToolName.Contains(TEXT("read"), ESearchCase::IgnoreCase)
			|| ToolName.Contains(TEXT("status"), ESearchCase::IgnoreCase))
		{
			return TEXT("Read");
		}
		return TEXT("Action");
	}

	FString HyperAIToolFallbackDescription(
		const FHyperAIStudioCapabilityToolDefinition& Tool,
		const FString& CapabilityLabel)
	{
		FString PackName = Tool.PackId;
		int32 LastSeparator = INDEX_NONE;
		if (PackName.FindLastChar(TEXT('.'), LastSeparator))
		{
			PackName.RightChopInline(LastSeparator + 1);
		}
		PackName = HumanizeToolIdentifier(PackName);
		if (CapabilityLabel == TEXT("Read"))
		{
			return FString::Printf(TEXT("Read %s information without changing the project."), *PackName);
		}
		if (CapabilityLabel == TEXT("Validate"))
		{
			return FString::Printf(TEXT("Validate %s state or a prepared plan without applying changes."), *PackName);
		}
		if (CapabilityLabel.StartsWith(TEXT("Plan only")))
		{
			return FString::Printf(TEXT("Prepare and validate a %s change plan; use Epic Unreal MCP to apply it."), *PackName);
		}
		if (CapabilityLabel == TEXT("Limited direct Fast edit"))
		{
			return FString::Printf(TEXT("Prepare %s edits; supported reversible variants can run directly in Fast mode."), *PackName);
		}
		return FString::Printf(TEXT("Run %s."), *HumanizeToolIdentifier(Tool.Name));
	}

	FString BuildHyperAIToolDialogDetails()
	{
		const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
		const bool bExtended = !Settings
			|| Settings->NativeToolChannel == EHyperAIStudioNativeToolChannel::Preview;
		const bool bFast = !Settings
			|| Settings->NativeExecutionMode == EHyperAIStudioNativeExecutionMode::Fast;
		const FHyperAIStudioCapabilityCatalog& Catalog =
			FHyperAIStudioCapabilityPackRegistry::GetCatalog();

		TArray<FHyperAIStudioRuntimeToolBinding> RuntimeBindings;
		TArray<FString> RuntimeDiagnostics;
		const bool bRuntimeIndexValid = FHyperAIStudioCapabilityRuntimeIndex::BuildSnapshot(
			RuntimeBindings,
			RuntimeDiagnostics);
		TMap<FString, const FHyperAIStudioRuntimeToolBinding*> RuntimeByName;
		for (const FHyperAIStudioRuntimeToolBinding& Binding : RuntimeBindings)
		{
			RuntimeByName.Add(Binding.ToolName, &Binding);
		}

		int32 LoadedCount = 0;
		int32 RegisteredCount = 0;
		int32 CallableCount = 0;
		FString ToolLines;
		ToolLines.Reserve(FMath::Min(52000, Catalog.Tools.Num() * 360));
		TArray<FHyperAIStudioCapabilityToolDefinition> SortedTools = Catalog.Tools;
		SortedTools.Sort([](
			const FHyperAIStudioCapabilityToolDefinition& A,
			const FHyperAIStudioCapabilityToolDefinition& B)
		{
			return A.Name < B.Name;
		});
		for (const FHyperAIStudioCapabilityToolDefinition& Tool : SortedTools)
		{
			const FHyperAIStudioRuntimeToolBinding* Binding = RuntimeByName.FindRef(Tool.Name);
			LoadedCount += Binding && Binding->bImplementationLoaded ? 1 : 0;
			RegisteredCount += Binding && Binding->bToolsetRegistered ? 1 : 0;
			const bool bToolSetAllows = bExtended
				&& (Tool.AdmissionState == EHyperAIStudioCapabilityAdmissionState::Admitted
					|| Tool.AdmissionState == EHyperAIStudioCapabilityAdmissionState::SourceCandidate);
			const bool bCallable = Binding && Binding->bToolEnabled && bToolSetAllows;
			CallableCount += bCallable ? 1 : 0;

			const TCHAR* RuntimeState = bCallable
				? TEXT("included")
				: (!Binding || !Binding->bImplementationLoaded
					? TEXT("module-unloaded")
					: (!Binding->bToolsetRegistered
						? TEXT("not-registered")
						: (!Binding->bToolEnabled
							? TEXT("filtered-or-disabled")
							: TEXT("extended-tools-disabled"))));
			const FString CapabilityLabel = HyperAIToolCapabilityLabel(Tool.Name);
			FString Description = Binding ? Binding->Description : FString();
			Description.ReplaceInline(TEXT("\r"), TEXT(" "));
			Description.ReplaceInline(TEXT("\n"), TEXT(" "));
			if (Description.TrimStartAndEnd().IsEmpty())
			{
				Description = HyperAIToolFallbackDescription(Tool, CapabilityLabel);
			}
			ToolLines += FString::Printf(
				TEXT("- [%s] %s — %s; pack=%s; safety=%s; toolset=%s — %s"),
				RuntimeState,
				*Tool.Name,
				*CapabilityLabel,
				*Tool.PackId,
				*CapabilitySafetyText(Tool.AllowedSafetyClasses),
				Binding ? *Binding->QualifiedToolset : TEXT("not loaded"),
				*Description.Left(180));
			ToolLines += LINE_TERMINATOR;
		}

		FString Details = FString::Printf(
			TEXT("Extended Hyper Tools%sActive tool set: %s%sExecution: %s%sIncluded tools: %d%sLoaded implementations: %d%sRegistered: %d%sIncluded now: %d%sRuntime index: %s%sLegend: Included means offered through MCP; it does not mean every edit form executes directly. Plan-only tools use Epic Unreal MCP to apply changes.%sRisk policy: destructive and external-effect tools always use strict safety.%s%s"),
			LINE_TERMINATOR,
			bExtended ? TEXT("Unreal MCP + Extended Hyper Tools") : TEXT("Unreal MCP Only"), LINE_TERMINATOR,
			bFast ? TEXT("Fast") : TEXT("Strict Safety"), LINE_TERMINATOR,
			Catalog.Tools.Num(), LINE_TERMINATOR,
			LoadedCount, LINE_TERMINATOR,
			RegisteredCount, LINE_TERMINATOR,
			CallableCount, LINE_TERMINATOR,
			bRuntimeIndexValid ? TEXT("valid") : TEXT("diagnostics present"), LINE_TERMINATOR,
			LINE_TERMINATOR,
			LINE_TERMINATOR,
			LINE_TERMINATOR);
		if (!RuntimeDiagnostics.IsEmpty())
		{
			Details += TEXT("Runtime diagnostics: ")
				+ FString::Join(RuntimeDiagnostics, TEXT(", "))
				+ LINE_TERMINATOR + LINE_TERMINATOR;
		}
		return Details + ToolLines;
	}

	FString BuildToolsListDetails(const FString& Content, TArray<FString>& OutToolNames)
	{
		FString Message;
		if (!ExtractToolNamesFromToolsList(Content, OutToolNames, Message))
		{
			return FString::Printf(TEXT("tools/list failed: %s"), *Message);
		}

		FString Details = FString::Printf(TEXT("%s%s"), *Message, LINE_TERMINATOR);
		if (OutToolNames.IsEmpty())
		{
			return Details + TEXT("No tools were advertised.");
		}

		Details += LINE_TERMINATOR;
		for (const FString& ToolName : OutToolNames)
		{
			Details += FString::Printf(TEXT("- %s%s"), *ToolName, LINE_TERMINATOR);
		}
		if (OutToolNames.Contains(TEXT("list_toolsets")))
		{
			Details += LINE_TERMINATOR TEXT("Tool-search mode is active. HyperAIStudio does not expand toolsets here because those calls run on the Unreal editor thread; use an external agent when you intentionally need live tool discovery.");
		}
		return Details;
	}

	FString ExtractNegotiatedProtocolVersion(const FString& Content)
	{
		TSharedPtr<FJsonObject> RootObject;
		if (!DeserializeJsonObjectFromResponse(Content, RootObject))
		{
			return FString();
		}

		const TSharedPtr<FJsonObject>* ResultObject = nullptr;
		if (!RootObject->TryGetObjectField(TEXT("result"), ResultObject) || !ResultObject || !ResultObject->IsValid())
		{
			return FString();
		}

		FString ProtocolVersion;
		(*ResultObject)->TryGetStringField(TEXT("protocolVersion"), ProtocolVersion);
		return ProtocolVersion;
	}

	void DeleteMcpSession(const FString& Endpoint, const FString& SessionId)
	{
		if (SessionId.IsEmpty())
		{
			return;
		}

		TSharedRef<IHttpRequest> DeleteRequest = FHttpModule::Get().CreateRequest();
		DeleteRequest->SetURL(Endpoint);
		DeleteRequest->SetVerb(TEXT("DELETE"));
		DeleteRequest->SetHeader(TEXT("Mcp-Session-Id"), SessionId);
		DeleteRequest->ProcessRequest();
	}

	bool ReadBoundedMcpResponseBody(
		const FHttpResponsePtr& Response,
		FString& OutBody,
		FString& OutError)
	{
		OutBody.Reset();
		OutError.Reset();
		if (!Response.IsValid())
		{
			OutError = TEXT("Unreal MCP returned no HTTP response.");
			return false;
		}
		if (!FHyperAIStudioCapabilityInventoryParser::ValidateResponseByteLengths(
			Response->GetContentLength(),
			Response->GetContent().Num(),
			OutError))
		{
			return false;
		}
		OutBody = Response->GetContentAsString();
		return true;
	}

	bool IsMcpRequestProvablyNotSent(const FHttpRequestPtr& Request)
	{
		// ConnectionError may include platform send/activity timeouts. Treat only a request that
		// never started as provably unsent; ProcessRequest() == false is handled synchronously.
		return Request.IsValid()
			&& Request->GetStatus() == EHttpRequestStatus::NotStarted;
	}

	class FShallowMcpProbeCompletion : public TSharedFromThis<FShallowMcpProbeCompletion>
	{
	public:
		FShallowMcpProbeCompletion(
			FString InEndpoint,
			FHyperAIStudioMcpSerialQueue::FComplete InQueueComplete,
			TFunction<void(bool, const FString&, int32)> InOnComplete)
			: Endpoint(MoveTemp(InEndpoint))
			, QueueComplete(MoveTemp(InQueueComplete))
			, OnComplete(MoveTemp(InOnComplete))
		{
		}

		void SetSession(FString InSessionId, FString InProtocolVersion)
		{
			SessionId = MoveTemp(InSessionId);
			ProtocolVersion = MoveTemp(InProtocolVersion);
		}

		void Complete(
			const bool bInReachable,
			FString InMessage,
			const int32 InToolCount,
			const bool bCleanupSafe,
			const bool bAmbiguousTransport = false)
		{
			if (CompletionCount.Increment() != 1)
			{
				return;
			}
			bReachable = bInReachable;
			Message = MoveTemp(InMessage);
			ToolCount = InToolCount;
			bQuarantineRelease = bAmbiguousTransport;

			if (bCleanupSafe && !SessionId.IsEmpty())
			{
				TSharedRef<IHttpRequest> DeleteRequest = FHttpModule::Get().CreateRequest();
				DeleteRequest->SetURL(Endpoint);
				DeleteRequest->SetVerb(TEXT("DELETE"));
				DeleteRequest->SetHeader(TEXT("Mcp-Session-Id"), SessionId);
				if (!ProtocolVersion.IsEmpty())
				{
					DeleteRequest->SetHeader(TEXT("Mcp-Protocol-Version"), ProtocolVersion);
				}
				DeleteRequest->SetTimeout(2.0f);
				DeleteRequest->OnProcessRequestComplete().BindLambda(
					[Self = AsShared()](FHttpRequestPtr, FHttpResponsePtr, const bool)
					{
						Self->Deliver();
					});
				if (DeleteRequest->ProcessRequest())
				{
					return;
				}
			}
			Deliver();
		}

	private:
		void Deliver()
		{
			if (DeliveryCount.Increment() != 1)
			{
				return;
			}
			if (QueueComplete)
			{
				QueueComplete(bQuarantineRelease
					? FHyperAIStudioMcpSerialQueue::ECompletionDisposition::AmbiguousTransport
					: FHyperAIStudioMcpSerialQueue::ECompletionDisposition::SafeToRelease);
			}
			if (OnComplete)
			{
				OnComplete(bReachable, Message, ToolCount);
			}
		}

		FString Endpoint;
		FString SessionId;
		FString ProtocolVersion;
		FHyperAIStudioMcpSerialQueue::FComplete QueueComplete;
		TFunction<void(bool, const FString&, int32)> OnComplete;
		FThreadSafeCounter CompletionCount;
		FThreadSafeCounter DeliveryCount;
		bool bReachable = false;
		bool bQuarantineRelease = false;
		FString Message;
		int32 ToolCount = 0;
	};

	FString QuotePowerShellSingle(const FString& Value)
	{
		FString Escaped = Value;
		Escaped.ReplaceInline(TEXT("'"), TEXT("''"));
		return FString::Printf(TEXT("'%s'"), *Escaped);
	}

	FString QuoteCommandLineDouble(const FString& Value)
	{
		FString Escaped = Value;
		Escaped.ReplaceInline(TEXT("\""), TEXT("\\\""));
		return FString::Printf(TEXT("\"%s\""), *Escaped);
	}

	FString ResolvePowerShellPath()
	{
		FString PowerShellPath;
#if PLATFORM_WINDOWS
		const FString SystemRoot = FPlatformMisc::GetEnvironmentVariable(TEXT("SystemRoot"));
		if (!SystemRoot.IsEmpty())
		{
			const FString SystemPowerShellPath = FPaths::Combine(SystemRoot, TEXT("System32"), TEXT("WindowsPowerShell"), TEXT("v1.0"), TEXT("powershell.exe"));
			if (FPaths::FileExists(SystemPowerShellPath))
			{
				PowerShellPath = SystemPowerShellPath;
			}
		}
#endif
		if (PowerShellPath.IsEmpty())
		{
			PowerShellPath = TEXT("powershell.exe");
		}
		return PowerShellPath;
	}

	bool TryGetUnsupportedCodexServiceTierValue(const FString& ConfigText, FString& OutValue)
	{
		TArray<FString> Lines;
		ConfigText.ParseIntoArrayLines(Lines, false);

		for (const FString& Line : Lines)
		{
			const FString TrimmedLine = Line.TrimStartAndEnd();
			if (TrimmedLine.IsEmpty() || TrimmedLine.StartsWith(TEXT("#")))
			{
				continue;
			}

			constexpr int32 KeyLength = 12; // service_tier
			if (!TrimmedLine.StartsWith(TEXT("service_tier"), ESearchCase::IgnoreCase))
			{
				continue;
			}

			if (TrimmedLine.Len() > KeyLength)
			{
				const TCHAR NextChar = TrimmedLine[KeyLength];
				if (NextChar != TEXT('=') && NextChar != TEXT(' ') && NextChar != TEXT('\t'))
				{
					continue;
				}
			}

			int32 EqualsIndex = INDEX_NONE;
			if (!TrimmedLine.FindChar(TEXT('='), EqualsIndex))
			{
				continue;
			}

			FString Value = TrimmedLine.Mid(EqualsIndex + 1).TrimStartAndEnd();
			int32 CommentIndex = INDEX_NONE;
			if (Value.FindChar(TEXT('#'), CommentIndex))
			{
				Value = Value.Left(CommentIndex).TrimStartAndEnd();
			}

			if (Value.Len() >= 2
				&& ((Value.StartsWith(TEXT("\"")) && Value.EndsWith(TEXT("\"")))
					|| (Value.StartsWith(TEXT("'")) && Value.EndsWith(TEXT("'")))))
			{
				Value = Value.Mid(1, Value.Len() - 2).TrimStartAndEnd();
			}

			if (!Value.IsEmpty()
				&& !Value.Equals(TEXT("fast"), ESearchCase::IgnoreCase)
				&& !Value.Equals(TEXT("flex"), ESearchCase::IgnoreCase))
			{
				OutValue = Value;
				return true;
			}
		}

		return false;
	}

	FString FindUnsupportedCodexServiceTierConfig()
	{
		const FString IgnoreValue = FPlatformMisc::GetEnvironmentVariable(TEXT("HYPERAISTUDIO_IGNORE_CODEX_CONFIG"));
		if (IgnoreValue == TEXT("1") || IgnoreValue.Equals(TEXT("true"), ESearchCase::IgnoreCase))
		{
			return FString();
		}

		TArray<FString> CandidatePaths;
		auto AddCandidate = [&CandidatePaths](const FString& CandidatePath)
		{
			if (CandidatePath.IsEmpty())
			{
				return;
			}

			const FString FullPath = FPaths::ConvertRelativePathToFull(CandidatePath);
			if (!CandidatePaths.Contains(FullPath))
			{
				CandidatePaths.Add(FullPath);
			}
		};

		AddCandidate(FPaths::Combine(FHyperAIStudioService::GetProjectRoot(), TEXT(".codex"), TEXT("config.toml")));

		const FString CodexHome = FPlatformMisc::GetEnvironmentVariable(TEXT("CODEX_HOME"));
		if (!CodexHome.IsEmpty())
		{
			AddCandidate(FPaths::Combine(CodexHome, TEXT("config.toml")));
		}

		const FString UserProfile = FPlatformMisc::GetEnvironmentVariable(TEXT("USERPROFILE"));
		if (!UserProfile.IsEmpty())
		{
			AddCandidate(FPaths::Combine(UserProfile, TEXT(".codex"), TEXT("config.toml")));
		}

		const FString Home = FPlatformMisc::GetEnvironmentVariable(TEXT("HOME"));
		if (!Home.IsEmpty())
		{
			AddCandidate(FPaths::Combine(Home, TEXT(".codex"), TEXT("config.toml")));
		}

		AddCandidate(FPaths::Combine(FPlatformProcess::UserDir(), TEXT(".codex"), TEXT("config.toml")));

		for (const FString& CandidatePath : CandidatePaths)
		{
			FString ConfigText;
			if (!FFileHelper::LoadFileToString(ConfigText, *CandidatePath))
			{
				continue;
			}

			FString UnsupportedValue;
			if (TryGetUnsupportedCodexServiceTierValue(ConfigText, UnsupportedValue))
			{
				return CandidatePath;
			}
		}

		return FString();
	}

	FString SanitizeFileNamePart(const FString& Value, const FString& Fallback)
	{
		FString Result;
		const FString Source = Value.IsEmpty() ? Fallback : Value;
		for (const TCHAR Ch : Source.ToLower())
		{
			if ((Ch >= TEXT('a') && Ch <= TEXT('z')) || (Ch >= TEXT('0') && Ch <= TEXT('9')) || Ch == TEXT('-') || Ch == TEXT('_'))
			{
				Result.AppendChar(Ch);
			}
			else if (Ch == TEXT(' ') || Ch == TEXT('.') || Ch == TEXT('/'))
			{
				Result.AppendChar(TEXT('-'));
			}
		}
		return Result.IsEmpty() ? Fallback : Result;
	}

	FString NormalizeAgentName(const FString& AgentName, const UHyperAIStudioSettings* Settings)
	{
		if (!AgentName.IsEmpty())
		{
			return AgentName;
		}
		if (Settings && !Settings->PreferredAgent.IsEmpty())
		{
			return Settings->PreferredAgent;
		}
		return TEXT("Codex");
	}

	FString GetProjectDisplayName()
	{
		const FString ProjectFile = FPaths::GetProjectFilePath();
		if (!ProjectFile.IsEmpty())
		{
			return FPaths::GetBaseFilename(ProjectFile);
		}
		return FApp::GetProjectName();
	}

	FString RelativeToProject(const FString& ProjectRoot, const FString& AbsolutePath)
	{
		FString RelativePath = AbsolutePath;
		FPaths::MakePathRelativeTo(RelativePath, *ProjectRoot);
		return RelativePath;
	}

	FString GetBlueprintNodeTitle(const UEdGraphNode* Node)
	{
		if (!Node)
		{
			return FString();
		}

		const FString Title = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
		return Title.IsEmpty() ? Node->GetName() : Title;
	}

	TArray<const UEdGraphNode*> CollectBlueprintContextNodes(const UEdGraph* Graph, const UEdGraphNode* ContextNode)
	{
		TArray<const UEdGraphNode*> Nodes;
		const UEdGraph* ContextGraph = Graph ? Graph : (ContextNode ? ContextNode->GetGraph() : nullptr);

		if (ContextGraph)
		{
			if (TSharedPtr<SGraphEditor> GraphEditor = SGraphEditor::FindGraphEditorForGraph(ContextGraph))
			{
				const FGraphPanelSelectionSet& Selection = GraphEditor->GetSelectedNodes();
				for (UObject* SelectedObject : Selection)
				{
					const UEdGraphNode* SelectedNode = Cast<UEdGraphNode>(SelectedObject);
					if (SelectedNode && SelectedNode->GetGraph() == ContextGraph)
					{
						Nodes.AddUnique(SelectedNode);
					}
				}
			}
		}

		if (ContextNode && (!ContextGraph || ContextNode->GetGraph() == ContextGraph))
		{
			Nodes.AddUnique(ContextNode);
		}

		Nodes.Sort([](const UEdGraphNode& Left, const UEdGraphNode& Right)
		{
			if (Left.NodePosY == Right.NodePosY)
			{
				return Left.NodePosX < Right.NodePosX;
			}
			return Left.NodePosY < Right.NodePosY;
		});

		return Nodes;
	}

	void AddBlueprintNodesToContext(const TSharedRef<FJsonObject>& Root, const UEdGraph* Graph, const UEdGraphNode* ContextNode)
	{
		const UEdGraph* ContextGraph = Graph ? Graph : (ContextNode ? ContextNode->GetGraph() : nullptr);
		TArray<TSharedPtr<FJsonValue>> NodeValues;

		if (ContextGraph)
		{
			TSharedRef<FJsonObject> GraphObject = MakeShared<FJsonObject>();
			GraphObject->SetStringField(TEXT("name"), ContextGraph->GetName());
			GraphObject->SetStringField(TEXT("path"), ContextGraph->GetPathName());
			if (const UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForGraph(ContextGraph))
			{
				GraphObject->SetStringField(TEXT("blueprintName"), Blueprint->GetName());
				GraphObject->SetStringField(TEXT("blueprintPath"), Blueprint->GetPathName());
			}
			Root->SetObjectField(TEXT("blueprintGraph"), GraphObject);
		}

		for (const UEdGraphNode* Node : CollectBlueprintContextNodes(ContextGraph, ContextNode))
		{
			if (!Node)
			{
				continue;
			}

			TSharedRef<FJsonObject> NodeObject = MakeShared<FJsonObject>();
			NodeObject->SetStringField(TEXT("name"), Node->GetName());
			NodeObject->SetStringField(TEXT("title"), GetBlueprintNodeTitle(Node));
			NodeObject->SetStringField(TEXT("class"), Node->GetClass() ? Node->GetClass()->GetName() : FString());
			NodeObject->SetStringField(TEXT("path"), Node->GetPathName());
			NodeObject->SetStringField(TEXT("guid"), Node->NodeGuid.ToString());
			NodeObject->SetStringField(TEXT("comment"), Node->NodeComment);
			NodeObject->SetNumberField(TEXT("x"), Node->NodePosX);
			NodeObject->SetNumberField(TEXT("y"), Node->NodePosY);
			NodeObject->SetNumberField(TEXT("width"), Node->NodeWidth);
			NodeObject->SetNumberField(TEXT("height"), Node->NodeHeight);
			NodeObject->SetBoolField(TEXT("isCommentNode"), Node->IsA<UEdGraphNode_Comment>());

			TArray<TSharedPtr<FJsonValue>> PinValues;
			for (const UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin)
				{
					continue;
				}

				TSharedRef<FJsonObject> PinObject = MakeShared<FJsonObject>();
				PinObject->SetStringField(TEXT("name"), Pin->PinName.ToString());
				PinObject->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("Input") : TEXT("Output"));
				PinObject->SetStringField(TEXT("category"), Pin->PinType.PinCategory.ToString());
				PinObject->SetStringField(TEXT("subCategory"), Pin->PinType.PinSubCategory.ToString());
				PinObject->SetStringField(TEXT("defaultValue"), Pin->DefaultValue);
				PinObject->SetNumberField(TEXT("linkedTo"), Pin->LinkedTo.Num());
				PinValues.Add(MakeShared<FJsonValueObject>(PinObject));
			}
			NodeObject->SetArrayField(TEXT("pins"), PinValues);

			NodeValues.Add(MakeShared<FJsonValueObject>(NodeObject));
		}

		Root->SetArrayField(TEXT("selectedBlueprintNodes"), NodeValues);
		Root->SetNumberField(TEXT("selectedBlueprintNodeCount"), NodeValues.Num());
	}

	int32 EstimateNodeWidth(const UEdGraphNode* Node)
	{
		return Node && Node->NodeWidth > 0 ? Node->NodeWidth : 280;
	}

	int32 EstimateNodeHeight(const UEdGraphNode* Node)
	{
		return Node && Node->NodeHeight > 0 ? Node->NodeHeight : FMath::Clamp(96 + (Node ? Node->Pins.Num() * 14 : 0), 120, 360);
	}

	FString BuildAutoCommentText(const TArray<const UEdGraphNode*>& Nodes)
	{
		if (Nodes.Num() == 0)
		{
			return TEXT("Blueprint logic");
		}

		TArray<FString> Titles;
		for (const UEdGraphNode* Node : Nodes)
		{
			const FString Title = GetBlueprintNodeTitle(Node).TrimStartAndEnd();
			if (!Title.IsEmpty())
			{
				Titles.AddUnique(Title);
			}
			if (Titles.Num() >= 3)
			{
				break;
			}
		}

		if (Titles.Num() == 0)
		{
			return FString::Printf(TEXT("Blueprint logic (%d nodes)"), Nodes.Num());
		}

		return FString::Printf(TEXT("%s%s"),
			*FString::Join(Titles, TEXT(" -> ")),
			Nodes.Num() > Titles.Num() ? *FString::Printf(TEXT(" + %d more"), Nodes.Num() - Titles.Num()) : TEXT(""));
	}

	void AddAssetDataToContext(const TSharedRef<FJsonObject>& Root, const TArray<FAssetData>& Assets)
	{
		TArray<TSharedPtr<FJsonValue>> AssetValues;
		for (const FAssetData& Asset : Assets)
		{
			if (!Asset.IsValid())
			{
				continue;
			}

			TSharedRef<FJsonObject> AssetObject = MakeShared<FJsonObject>();
			AssetObject->SetStringField(TEXT("assetName"), Asset.AssetName.ToString());
			AssetObject->SetStringField(TEXT("assetClass"), Asset.AssetClassPath.ToString());
			AssetObject->SetStringField(TEXT("packageName"), Asset.PackageName.ToString());
			AssetObject->SetStringField(TEXT("objectPath"), Asset.GetObjectPathString());
			AssetValues.Add(MakeShared<FJsonValueObject>(AssetObject));
		}
		Root->SetArrayField(TEXT("selectedAssets"), AssetValues);
	}

	TSharedRef<FJsonObject> CaptureContextObject(
		const FString& ProjectRoot,
		const FString& Endpoint,
		const FString& AgentName,
		const FString& Intent,
		const UEdGraph* BlueprintGraph,
		const UEdGraphNode* BlueprintNode,
		const TArray<FAssetData>* ExplicitSelectedAssets = nullptr)
	{
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("generatedAt"), FDateTime::UtcNow().ToIso8601());
		Root->SetStringField(TEXT("projectRoot"), ProjectRoot);
		Root->SetStringField(TEXT("projectFile"), FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()));
		Root->SetStringField(TEXT("endpoint"), Endpoint);
		Root->SetStringField(TEXT("agentName"), AgentName);
		Root->SetStringField(TEXT("intent"), Intent);

		if (GEditor)
		{
			if (UWorld* World = GEditor->GetEditorWorldContext().World())
			{
				Root->SetStringField(TEXT("currentMap"), World->GetMapName());
				Root->SetStringField(TEXT("worldPath"), World->GetPathName());
			}

			TArray<TSharedPtr<FJsonValue>> ActorValues;
			if (USelection* SelectedActors = GEditor->GetSelectedActors())
			{
				for (FSelectionIterator It(*SelectedActors); It; ++It)
				{
					if (const AActor* Actor = Cast<AActor>(*It))
					{
						TSharedRef<FJsonObject> ActorObject = MakeShared<FJsonObject>();
						ActorObject->SetStringField(TEXT("name"), Actor->GetName());
						ActorObject->SetStringField(TEXT("label"), Actor->GetActorLabel());
						ActorObject->SetStringField(TEXT("class"), Actor->GetClass() ? Actor->GetClass()->GetName() : FString());
						ActorObject->SetStringField(TEXT("path"), Actor->GetPathName());
						const FVector Location = Actor->GetActorLocation();
						ActorObject->SetStringField(TEXT("location"), FString::Printf(TEXT("%.2f, %.2f, %.2f"), Location.X, Location.Y, Location.Z));
						ActorValues.Add(MakeShared<FJsonValueObject>(ActorObject));
					}
				}
			}
			Root->SetArrayField(TEXT("selectedActors"), ActorValues);
		}

		if (ExplicitSelectedAssets)
		{
			AddAssetDataToContext(Root, *ExplicitSelectedAssets);
		}
		else
		{
			TArray<FAssetData> SelectedAssets;
			if (FContentBrowserModule* ContentBrowserModule = FModuleManager::Get().LoadModulePtr<FContentBrowserModule>(TEXT("ContentBrowser")))
			{
				ContentBrowserModule->Get().GetSelectedAssets(SelectedAssets);
			}
			AddAssetDataToContext(Root, SelectedAssets);
		}

		AddBlueprintNodesToContext(Root, BlueprintGraph, BlueprintNode);

		return Root;
	}

	FString BuildContextMarkdown(const TSharedRef<FJsonObject>& Context)
	{
		FString Markdown;
		Markdown += TEXT("# HyperAIStudio Context") LINE_TERMINATOR LINE_TERMINATOR;
		Markdown += FString::Printf(TEXT("- Project root: `%s`%s"), *Context->GetStringField(TEXT("projectRoot")), LINE_TERMINATOR);
		Markdown += FString::Printf(TEXT("- Project file: `%s`%s"), *Context->GetStringField(TEXT("projectFile")), LINE_TERMINATOR);
		Markdown += FString::Printf(TEXT("- Unreal MCP endpoint: `%s`%s"), *Context->GetStringField(TEXT("endpoint")), LINE_TERMINATOR);
		Markdown += FString::Printf(TEXT("- Target agent: `%s`%s"), *Context->GetStringField(TEXT("agentName")), LINE_TERMINATOR);
		Markdown += FString::Printf(TEXT("- Intent: %s%s"), *Context->GetStringField(TEXT("intent")), LINE_TERMINATOR);

		FString CurrentMap;
		if (Context->TryGetStringField(TEXT("currentMap"), CurrentMap))
		{
			Markdown += FString::Printf(TEXT("- Current map: `%s`%s"), *CurrentMap, LINE_TERMINATOR);
		}

		Markdown += LINE_TERMINATOR TEXT("## Selected Actors") LINE_TERMINATOR LINE_TERMINATOR;
		const TArray<TSharedPtr<FJsonValue>>* Actors = nullptr;
		if (Context->TryGetArrayField(TEXT("selectedActors"), Actors) && Actors && Actors->Num() > 0)
		{
			for (const TSharedPtr<FJsonValue>& ActorValue : *Actors)
			{
				const TSharedPtr<FJsonObject> Actor = ActorValue.IsValid() ? ActorValue->AsObject() : nullptr;
				if (Actor.IsValid())
				{
					Markdown += FString::Printf(TEXT("- `%s` (%s) path `%s` at %s%s"),
						*Actor->GetStringField(TEXT("label")),
						*Actor->GetStringField(TEXT("class")),
						*Actor->GetStringField(TEXT("path")),
						*Actor->GetStringField(TEXT("location")),
						LINE_TERMINATOR);
				}
			}
		}
		else
		{
			Markdown += TEXT("- No actors selected.") LINE_TERMINATOR;
		}

		Markdown += LINE_TERMINATOR TEXT("## Selected Assets") LINE_TERMINATOR LINE_TERMINATOR;
		const TArray<TSharedPtr<FJsonValue>>* Assets = nullptr;
		if (Context->TryGetArrayField(TEXT("selectedAssets"), Assets) && Assets && Assets->Num() > 0)
		{
			for (const TSharedPtr<FJsonValue>& AssetValue : *Assets)
			{
				const TSharedPtr<FJsonObject> Asset = AssetValue.IsValid() ? AssetValue->AsObject() : nullptr;
				if (Asset.IsValid())
				{
					Markdown += FString::Printf(TEXT("- `%s` (%s) package `%s`%s"),
						*Asset->GetStringField(TEXT("assetName")),
						*Asset->GetStringField(TEXT("assetClass")),
						*Asset->GetStringField(TEXT("packageName")),
						LINE_TERMINATOR);
				}
			}
		}
		else
		{
			Markdown += TEXT("- No assets selected.") LINE_TERMINATOR;
		}

		Markdown += LINE_TERMINATOR TEXT("## Selected Blueprint Nodes") LINE_TERMINATOR LINE_TERMINATOR;
		const TArray<TSharedPtr<FJsonValue>>* BlueprintNodes = nullptr;
		if (Context->TryGetArrayField(TEXT("selectedBlueprintNodes"), BlueprintNodes) && BlueprintNodes && BlueprintNodes->Num() > 0)
		{
			FString GraphPath;
			const TSharedPtr<FJsonObject>* GraphObject = nullptr;
			if (Context->TryGetObjectField(TEXT("blueprintGraph"), GraphObject) && GraphObject && GraphObject->IsValid())
			{
				(*GraphObject)->TryGetStringField(TEXT("path"), GraphPath);
			}
			if (!GraphPath.IsEmpty())
			{
				Markdown += FString::Printf(TEXT("- Graph: `%s`%s"), *GraphPath, LINE_TERMINATOR);
			}

			for (const TSharedPtr<FJsonValue>& NodeValue : *BlueprintNodes)
			{
				const TSharedPtr<FJsonObject> Node = NodeValue.IsValid() ? NodeValue->AsObject() : nullptr;
				if (Node.IsValid())
				{
					Markdown += FString::Printf(TEXT("- `%s` (%s) at %d,%d%s"),
						*Node->GetStringField(TEXT("title")),
						*Node->GetStringField(TEXT("class")),
						static_cast<int32>(Node->GetNumberField(TEXT("x"))),
						static_cast<int32>(Node->GetNumberField(TEXT("y"))),
						LINE_TERMINATOR);
				}
			}
		}
		else
		{
			Markdown += TEXT("- No Blueprint graph nodes selected or no graph context was available.") LINE_TERMINATOR;
		}

		return Markdown;
	}

	bool HasSelectionContext(const TSharedRef<FJsonObject>& Context)
	{
		const TArray<TSharedPtr<FJsonValue>>* Actors = nullptr;
		if (Context->TryGetArrayField(TEXT("selectedActors"), Actors) && Actors && Actors->Num() > 0)
		{
			return true;
		}

		const TArray<TSharedPtr<FJsonValue>>* Assets = nullptr;
		if (Context->TryGetArrayField(TEXT("selectedAssets"), Assets) && Assets && Assets->Num() > 0)
		{
			return true;
		}

		const TArray<TSharedPtr<FJsonValue>>* BlueprintNodes = nullptr;
		return Context->TryGetArrayField(TEXT("selectedBlueprintNodes"), BlueprintNodes) && BlueprintNodes && BlueprintNodes->Num() > 0;
	}

	FString GetInstructionFileForAgent(const FString& AgentName)
	{
		return AgentName.Contains(TEXT("Claude"), ESearchCase::IgnoreCase)
			? FString(TEXT("CLAUDE.md"))
			: FString(TEXT("AGENTS.md"));
	}

	FString BuildAgentPrompt(const FString& Intent, const FString& AgentName, const FString& ProjectRoot, const FString& Endpoint, const FString& ContextMarkdownPath, const FString& ContextJsonPath, bool bIncludeContextFiles)
	{
		FString TrimmedIntent = Intent;
		TrimmedIntent.TrimStartAndEndInline();
		if (TrimmedIntent.IsEmpty())
		{
			TrimmedIntent = FString::Printf(TEXT("Say you are connected to %s and ready, then wait for my task."), *GetProjectDisplayName());
		}

		FString Prompt;
		Prompt += TEXT("HyperAIStudio Unreal MCP session") LINE_TERMINATOR LINE_TERMINATOR;
		Prompt += TEXT("Task:") LINE_TERMINATOR;
		Prompt += TrimmedIntent + LINE_TERMINATOR LINE_TERMINATOR;
		Prompt += TEXT("Project:") LINE_TERMINATOR;
		Prompt += FString::Printf(TEXT("- Root: `%s`%s"), *ProjectRoot, LINE_TERMINATOR);
		Prompt += FString::Printf(TEXT("- Read `%s` from this working directory before acting.%s"), *GetInstructionFileForAgent(AgentName), LINE_TERMINATOR);
		Prompt += FString::Printf(TEXT("- Agent: `%s`%s"), *AgentName, LINE_TERMINATOR);
		Prompt += FString::Printf(TEXT("- MCP server: `unreal-mcp` is already configured at `%s`%s"), *Endpoint, LINE_TERMINATOR);
		if (bIncludeContextFiles)
		{
			Prompt += FString::Printf(TEXT("- Context markdown: `%s`%s"), *ContextMarkdownPath, LINE_TERMINATOR);
			Prompt += FString::Printf(TEXT("- Context JSON: `%s`%s"), *ContextJsonPath, LINE_TERMINATOR);
		}
		else
		{
			Prompt += TEXT("- Context files: none attached.") LINE_TERMINATOR;
		}
		Prompt += LINE_TERMINATOR;
		Prompt += TEXT("Rules:") LINE_TERMINATOR;
		Prompt += TEXT("- Follow the requested task directly; do not run unrelated setup or tool discovery.") LINE_TERMINATOR;
		Prompt += TEXT("- For current level/name questions, call `editor_toolset.toolsets.scene.SceneTools.get_current_level` through `call_tool` directly.") LINE_TERMINATOR;
		Prompt += TEXT("- If the task needs Unreal and the editor is closed or disconnected, run `.hyperai/scripts/Start-HyperAIStudioEditor.ps1`, then `.hyperai/scripts/Wait-HyperAIStudioMCP.ps1 -TimeoutSeconds 90`, even when native MCP tools are not currently exposed.") LINE_TERMINATOR;
		Prompt += TEXT("- Missing native MCP tools do not prove that this agent was opened from the wrong directory. Start and verify Unreal first; only request an MCP reload or project-root reopen if the endpoint is ready, the task requires Unreal tools, and this client still does not expose them.") LINE_TERMINATOR;
		Prompt += TEXT("- Put substantial planning, research, decisions, and task scratch files in a clearly named `.hyperai/work/YYYY-MM-DD-<clear-context-name>/` package; do not create generic agent-work folders in the project root.") LINE_TERMINATOR;
		if (bIncludeContextFiles)
		{
			Prompt += TEXT("- Read the referenced context files before acting.") LINE_TERMINATOR;
		}
		Prompt += TEXT("- Use one Unreal MCP call at a time.") LINE_TERMINATOR;
		return Prompt;
	}

	FString BuildQuickConnectPrompt()
	{
		return TEXT("Connect to this project's configured `unreal-mcp`. "
			"If Unreal is offline, run `.hyperai/scripts/Start-HyperAIStudioEditor.ps1` and retry. "
			"Verify the connection with `list_toolsets`. "
			"Say `Unreal MCP ready` only after that succeeds, then wait for my task.");
	}

	bool WriteQuickPromptPackageFiles(
		const FString& ProjectRoot,
		const FString& ResolvedAgentName,
		const FString& PromptText,
		FHyperAIStudioPromptPackage& OutPackage,
		FText& OutMessage)
	{
		const FString Timestamp = FDateTime::UtcNow().ToString(TEXT("%Y%m%d-%H%M%S"));
		const FString AgentSlug = SanitizeFileNamePart(ResolvedAgentName, TEXT("agent"));
		const FString PromptDir = FPaths::Combine(ProjectRoot, TEXT(".hyperai"), TEXT("prompts"));
		IFileManager::Get().MakeDirectory(*PromptDir, true);

		const FString PromptPath = FPaths::Combine(PromptDir, FString::Printf(TEXT("hyperai-%s-connect-%s-prompt.md"), *AgentSlug, *Timestamp));
		if (!WriteFileIfChanged(PromptPath, PromptText))
		{
			OutMessage = FText::FromString(TEXT("Could not write HyperAIStudio prompt file."));
			return false;
		}

		const FString SendScript = FPaths::Combine(ProjectRoot, TEXT(".hyperai"), TEXT("scripts"), TEXT("Send-HyperAIStudioPrompt.ps1"));
		OutPackage.bSuccess = true;
		OutPackage.AgentName = ResolvedAgentName;
		OutPackage.PromptText = PromptText;
		OutPackage.PromptPath = PromptPath;
		OutPackage.TerminalCommand = FString::Printf(TEXT("powershell -ExecutionPolicy Bypass -File %s -Agent %s -PromptFile %s"),
			*QuotePowerShellSingle(SendScript),
			*QuotePowerShellSingle(ResolvedAgentName),
			*QuotePowerShellSingle(PromptPath));
		OutPackage.Message = FString::Printf(TEXT("Quick prompt ready for %s."), *ResolvedAgentName);
		OutMessage = FText::FromString(OutPackage.Message);
		return true;
	}

	bool WritePromptPackageFiles(
		const FString& ProjectRoot,
		const FString& Endpoint,
		const FString& ResolvedAgentName,
		const FString& Intent,
		const TSharedRef<FJsonObject>& Context,
		FHyperAIStudioPromptPackage& OutPackage,
		FText& OutMessage)
	{
		const FString Timestamp = FDateTime::UtcNow().ToString(TEXT("%Y%m%d-%H%M%S"));
		const FString AgentSlug = SanitizeFileNamePart(ResolvedAgentName, TEXT("agent"));
		const FString PromptDir = FPaths::Combine(ProjectRoot, TEXT(".hyperai"), TEXT("prompts"));
		IFileManager::Get().MakeDirectory(*PromptDir, true);

		const FString BaseName = FString::Printf(TEXT("hyperai-%s-%s"), *AgentSlug, *Timestamp);
		const FString PromptPath = FPaths::Combine(PromptDir, BaseName + TEXT("-prompt.md"));
		const bool bIncludeContextFiles = HasSelectionContext(Context);
		FString ContextJsonPath;
		FString ContextMarkdownPath;

		if (bIncludeContextFiles)
		{
			const FString ContextDir = FPaths::Combine(ProjectRoot, TEXT(".hyperai"), TEXT("context"));
			IFileManager::Get().MakeDirectory(*ContextDir, true);

			ContextJsonPath = FPaths::Combine(ContextDir, BaseName + TEXT(".json"));
			ContextMarkdownPath = FPaths::Combine(ContextDir, BaseName + TEXT(".md"));
			if (!SerializeJsonToFile(ContextJsonPath, Context))
			{
				OutMessage = FText::FromString(TEXT("Could not write HyperAIStudio context JSON."));
				return false;
			}

			const FString ContextMarkdown = BuildContextMarkdown(Context);
			if (!WriteFileIfChanged(ContextMarkdownPath, ContextMarkdown))
			{
				OutMessage = FText::FromString(TEXT("Could not write HyperAIStudio context markdown."));
				return false;
			}
		}

		const FString PromptText = BuildAgentPrompt(
			Intent,
			ResolvedAgentName,
			ProjectRoot,
			Endpoint,
			ContextMarkdownPath,
			ContextJsonPath,
			bIncludeContextFiles);

		if (!WriteFileIfChanged(PromptPath, PromptText))
		{
			OutMessage = FText::FromString(TEXT("Could not write HyperAIStudio prompt file."));
			return false;
		}

		const FString SendScript = FPaths::Combine(ProjectRoot, TEXT(".hyperai"), TEXT("scripts"), TEXT("Send-HyperAIStudioPrompt.ps1"));
		OutPackage.bSuccess = true;
		OutPackage.AgentName = ResolvedAgentName;
		OutPackage.PromptText = PromptText;
		OutPackage.PromptPath = PromptPath;
		if (bIncludeContextFiles)
		{
			OutPackage.ContextJsonPath = ContextJsonPath;
			OutPackage.ContextMarkdownPath = ContextMarkdownPath;
		}
		OutPackage.TerminalCommand = FString::Printf(TEXT("powershell -ExecutionPolicy Bypass -File %s -Agent %s -PromptFile %s"),
			*QuotePowerShellSingle(SendScript),
			*QuotePowerShellSingle(ResolvedAgentName),
			*QuotePowerShellSingle(PromptPath));
		OutPackage.Message = FString::Printf(TEXT("Prompt package ready for %s."), *ResolvedAgentName);
		OutMessage = FText::FromString(OutPackage.Message);
		return true;
	}
}

FString FHyperAIStudioService::GetProjectRoot()
{
	return FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
}

FString FHyperAIStudioService::GetEndpoint(uint32 Port, const FString& UrlPath)
{
	return FString::Printf(TEXT("http://127.0.0.1:%u%s"), Port, *HyperAIStudio::Private::CleanUrlPath(UrlPath));
}

FString FHyperAIStudioService::GetDocumentationUrl()
{
	const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
	return Settings && !Settings->DocumentationUrl.IsEmpty()
		? Settings->DocumentationUrl
		: FString(TEXT("https://gamesbyhyper.com/docs/project-foundations/hyper-ai-studio/"));
}

FHyperAIStudioPluginSyncDecision FHyperAIStudioService::GetPluginSyncDecision(
	bool bRuntimeEnabled,
	bool bProjectDescriptorEnabled)
{
	FHyperAIStudioPluginSyncDecision Decision;
	Decision.bWriteProjectDescriptor = !bRuntimeEnabled && !bProjectDescriptorEnabled;
	Decision.bRestartRequired = !bRuntimeEnabled;
	return Decision;
}

FString FHyperAIStudioService::FindUnsupportedCodexServiceTierConfigPath()
{
	return HyperAIStudio::Private::FindUnsupportedCodexServiceTierConfig();
}

FString FHyperAIStudioService::GetCodexTerminalLaunchCommand()
{
	FString CodexPath;
	const FString CodexCommand = FindExecutableOnPath(TEXT("codex"), CodexPath)
		? HyperAIStudio::Private::QuoteCommandLineDouble(CodexPath)
		: FString(TEXT("codex"));
	FString LaunchCommand = CodexCommand + TEXT(" -c tui.alternate_screen=never");
	if (!FindUnsupportedCodexServiceTierConfigPath().IsEmpty())
	{
		LaunchCommand += TEXT(" -c service_tier=fast");
	}
	return LaunchCommand;
}

FString FHyperAIStudioService::GetAgentTerminalLaunchCommand(const FString& AgentName)
{
	auto BuildCommand = [](const FString& ExecutableName)
	{
		FString ExecutablePath;
		if (!FHyperAIStudioService::FindExecutableOnPath(ExecutableName, ExecutablePath))
		{
			return ExecutableName;
		}

		if (FPaths::GetExtension(ExecutablePath).Equals(TEXT("ps1"), ESearchCase::IgnoreCase))
		{
			return FString::Printf(TEXT("powershell -NoProfile -ExecutionPolicy Bypass -File %s"),
				*HyperAIStudio::Private::QuoteCommandLineDouble(ExecutablePath));
		}
		return HyperAIStudio::Private::QuoteCommandLineDouble(ExecutablePath);
	};

	if (AgentName.Contains(TEXT("Codex"), ESearchCase::IgnoreCase))
	{
		return GetCodexTerminalLaunchCommand();
	}
	if (AgentName.Contains(TEXT("Claude"), ESearchCase::IgnoreCase))
	{
		return BuildCommand(TEXT("claude"));
	}
	if (AgentName.Contains(TEXT("Gemini"), ESearchCase::IgnoreCase))
	{
		return BuildCommand(TEXT("gemini"));
	}
	return FString();
}

FString FHyperAIStudioService::GetCurrentContextSummary() const
{
	const FHyperAIStudioContextSnapshot Snapshot = GetCurrentContextSnapshot();
	return FString::Printf(
		TEXT("Current handoff context: %d selected actor(s), %d selected Content Browser asset(s), map `%s`. %s"),
		Snapshot.SelectedActorCount,
		Snapshot.SelectedAssetCount,
		*Snapshot.CurrentMap,
		*Snapshot.BlueprintContextHint);
}

FHyperAIStudioContextSnapshot FHyperAIStudioService::GetCurrentContextSnapshot() const
{
	const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
	const FString ProjectRoot = GetProjectRoot();
	const FString Endpoint = Settings
		? GetEndpoint(Settings->UnrealMCPPort, Settings->UnrealMCPUrlPath)
		: GetEndpoint(UE::ModelContextProtocol::DefaultServerPort, FString(UE::ModelContextProtocol::DefaultServerUrlPath));
	const FString AgentName = HyperAIStudio::Private::NormalizeAgentName(FString(), Settings);
	const TSharedRef<FJsonObject> Context = HyperAIStudio::Private::CaptureContextObject(
		ProjectRoot,
		Endpoint,
		AgentName,
		TEXT("Summarize current HyperAIStudio context."),
		nullptr,
		nullptr);

	FHyperAIStudioContextSnapshot Snapshot;
	Snapshot.ProjectRoot = ProjectRoot;
	Snapshot.Endpoint = Endpoint;
	Context->TryGetStringField(TEXT("projectFile"), Snapshot.ProjectFile);
	Context->TryGetStringField(TEXT("currentMap"), Snapshot.CurrentMap);
	if (Snapshot.CurrentMap.IsEmpty())
	{
		Snapshot.CurrentMap = TEXT("no editor world");
	}

	const TArray<TSharedPtr<FJsonValue>>* Actors = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Assets = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* BlueprintNodes = nullptr;
	Context->TryGetArrayField(TEXT("selectedActors"), Actors);
	Context->TryGetArrayField(TEXT("selectedAssets"), Assets);
	Context->TryGetArrayField(TEXT("selectedBlueprintNodes"), BlueprintNodes);

	Snapshot.SelectedActorCount = Actors ? Actors->Num() : 0;
	Snapshot.SelectedAssetCount = Assets ? Assets->Num() : 0;
	Snapshot.SelectedBlueprintNodeCount = BlueprintNodes ? BlueprintNodes->Num() : 0;
	Snapshot.BlueprintContextHint = Snapshot.SelectedBlueprintNodeCount > 0
		? FString::Printf(TEXT("%d selected Blueprint node(s)."), Snapshot.SelectedBlueprintNodeCount)
		: FString(TEXT("Blueprint node context is added from the Blueprint graph right-click menu."));

	return Snapshot;
}

FString FHyperAIStudioService::GetFirstRunPrompt() const
{
	const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
	const FString Endpoint = Settings
		? GetEndpoint(Settings->UnrealMCPPort, Settings->UnrealMCPUrlPath)
		: GetEndpoint(UE::ModelContextProtocol::DefaultServerPort, FString(UE::ModelContextProtocol::DefaultServerUrlPath));
	const FString ProjectRoot = GetProjectRoot();

	return FString::Printf(
		TEXT("HyperAIStudio is configured for this Unreal project.%s%s")
		TEXT("Project:%s")
		TEXT("- Root: `%s`%s")
		TEXT("- MCP: `unreal-mcp` at `%s`%s%s")
		TEXT("Agent instructions:%s")
		TEXT("- Read the generated instruction file supported by this agent client (`AGENTS.md` is canonical; `CLAUDE.md` contains the Claude Code notes).%s")
		TEXT("- Use this client's generated MCP config from the project root.%s%s")
		TEXT("If Unreal is disconnected, run `.hyperai/scripts/Start-HyperAIStudioEditor.ps1` and wait for MCP readiness. Missing native tools do not prove that the working directory is wrong; never request a new agent session merely to start Unreal. Wait for my task before changing assets."),
		LINE_TERMINATOR, LINE_TERMINATOR,
		LINE_TERMINATOR,
		*ProjectRoot, LINE_TERMINATOR,
		*Endpoint, LINE_TERMINATOR, LINE_TERMINATOR,
		LINE_TERMINATOR,
		LINE_TERMINATOR,
		LINE_TERMINATOR, LINE_TERMINATOR);
}

FString FHyperAIStudioService::GetCodexTomlPatch() const
{
	const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
	const FString Endpoint = Settings
		? GetEndpoint(Settings->UnrealMCPPort, Settings->UnrealMCPUrlPath)
		: GetEndpoint(UE::ModelContextProtocol::DefaultServerPort, FString(UE::ModelContextProtocol::DefaultServerUrlPath));

	return HyperAIStudio::Private::BuildManagedCodexTomlPatch(Endpoint, Settings);
}

TArray<FString> FHyperAIStudioService::GetRequiredPluginNames()
{
	return FHyperAIStudioCapabilityPresetResolver::GetFoundationPlugins();
}

bool FHyperAIStudioService::FindExecutableOnPath(const FString& ExecutableName, FString& OutPath)
{
	OutPath.Empty();
	FString CleanExecutableName = ExecutableName;
	CleanExecutableName.TrimStartAndEndInline();
	if (CleanExecutableName.IsEmpty())
	{
		return false;
	}

	if (FPaths::FileExists(CleanExecutableName))
	{
		OutPath = FPaths::ConvertRelativePathToFull(CleanExecutableName);
		return true;
	}

	const FString TestPathValue = FPlatformMisc::GetEnvironmentVariable(TEXT("HYPERAISTUDIO_TEST_PATH"));
	const FString PathValue = TestPathValue.IsEmpty()
		? FPlatformMisc::GetEnvironmentVariable(TEXT("PATH"))
		: TestPathValue;
	TArray<FString> Directories;
	PathValue.ParseIntoArray(Directories, TEXT(";"), true);
#if PLATFORM_WINDOWS
	if (TestPathValue.IsEmpty())
	{
		HyperAIStudio::Private::AppendKnownWindowsExecutableDirectories(Directories);
	}
#endif

	TArray<FString> CandidateNames;
#if PLATFORM_WINDOWS
	if (FPaths::GetExtension(CleanExecutableName).IsEmpty())
	{
		CandidateNames.Add(CleanExecutableName + TEXT(".exe"));
		CandidateNames.Add(CleanExecutableName + TEXT(".cmd"));
		CandidateNames.Add(CleanExecutableName + TEXT(".bat"));
		CandidateNames.Add(CleanExecutableName + TEXT(".ps1"));
	}
#endif
	CandidateNames.Add(CleanExecutableName);

	for (const FString& Directory : Directories)
	{
		FString CleanDirectory = Directory;
		CleanDirectory.TrimStartAndEndInline();
		CleanDirectory = CleanDirectory.TrimQuotes();
		if (CleanDirectory.IsEmpty())
		{
			continue;
		}

		for (const FString& CandidateName : CandidateNames)
		{
			const FString CandidatePath = FPaths::Combine(CleanDirectory, CandidateName);
			if (FPaths::FileExists(CandidatePath))
			{
				OutPath = FPaths::ConvertRelativePathToFull(CandidatePath);
				return true;
			}
		}
	}

	return false;
}

bool FHyperAIStudioService::IsExecutableOnPath(const FString& ExecutableName)
{
	FString FoundPath;
	return FindExecutableOnPath(ExecutableName, FoundPath);
}

TArray<FHyperAIStudioPrerequisiteStatus> FHyperAIStudioService::GetPrerequisites() const
{
	TArray<FHyperAIStudioPrerequisiteStatus> Result;
	const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
	const TArray<FString> FoundationPlugins = GetRequiredPluginNames();
	const TArray<FString> DesiredPlugins = Settings
		? FHyperAIStudioCapabilityPresetResolver::ResolveDesiredPlugins(*Settings)
		: FoundationPlugins;

	for (const FString& PluginName : DesiredPlugins)
	{
		FHyperAIStudioPrerequisiteStatus Entry;
		Entry.Id = PluginName;
		Entry.DisplayName = PluginName;
		Entry.bRequired = FoundationPlugins.Contains(PluginName);
		Entry.Detail = Entry.bRequired
			? TEXT("Unreal MCP foundation/workflow plugin")
			: TEXT("Selected capability-preset plugin");
		Entry.DocumentationUrl = TEXT("https://dev.epicgames.com/documentation/unreal-engine/unreal-mcp-in-unreal-editor");

		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(PluginName);
		Entry.bAvailable = Plugin.IsValid();
		Entry.bEnabled = Plugin.IsValid() && Plugin->IsEnabled();
		Entry.bCanEnable = Plugin.IsValid() && !Entry.bEnabled;
		Entry.bRestartRequired = Entry.bCanEnable;

		if (!Entry.bAvailable)
		{
			Entry.ActionHint = TEXT("Install an Unreal Engine version that includes this plugin.");
		}
		else if (!Entry.bEnabled)
		{
			Entry.ActionHint = TEXT("Enable the selected Unreal plugins, then restart the editor.");
		}
		else
		{
			Entry.ActionHint = TEXT("Ready");
		}

		Result.Add(Entry);
	}

	FHyperAIStudioPrerequisiteStatus Codex;
	Codex.Id = TEXT("codex");
	Codex.DisplayName = TEXT("Codex CLI");
	Codex.Detail = TEXT("External agent CLI");
	Codex.bRequired = false;
	const bool bCodexExecutableAvailable = IsExecutableOnPath(TEXT("codex"));
	const FString UnsupportedCodexConfigPath = HyperAIStudio::Private::FindUnsupportedCodexServiceTierConfig();
	Codex.bAvailable = bCodexExecutableAvailable;
	Codex.bEnabled = Codex.bAvailable;
	Codex.bCanEnable = false;
	if (!bCodexExecutableAvailable)
	{
		Codex.ActionHint = TEXT("Codex CLI was not found in Unreal's PATH or common user install folders. Install it, then restart Unreal if it still shows Missing.");
	}
	else if (!UnsupportedCodexConfigPath.IsEmpty())
	{
		Codex.ActionHint = FString::Printf(TEXT("Codex CLI is on PATH, but config `%s` contains an unsupported service_tier value. This Codex version accepts `fast` or `flex`. HyperAIStudio will launch Codex with `-c service_tier=fast`; remove that setting or change it to `fast`/`flex` to stop using the override."), *UnsupportedCodexConfigPath);
	}
	else
	{
		Codex.ActionHint = TEXT("Available on PATH");
	}
	Codex.InstallCommand = TEXT("powershell -ExecutionPolicy Bypass -Command \"irm https://chatgpt.com/codex/install.ps1 | iex\"");
	Codex.DocumentationUrl = TEXT("https://developers.openai.com/codex/quickstart");
	Result.Add(Codex);

	FHyperAIStudioPrerequisiteStatus Claude;
	Claude.Id = TEXT("claude");
	Claude.DisplayName = TEXT("Claude Code CLI");
	Claude.Detail = TEXT("External agent CLI");
	Claude.bRequired = false;
	Claude.bAvailable = IsExecutableOnPath(TEXT("claude"));
	Claude.bEnabled = Claude.bAvailable;
	Claude.bCanEnable = false;
	Claude.ActionHint = Claude.bAvailable ? TEXT("Available on PATH") : TEXT("Claude Code CLI was not found in Unreal's PATH or common user install folders. Install it, then restart Unreal if it still shows Missing.");
	Claude.InstallCommand = TEXT("powershell -ExecutionPolicy Bypass -Command \"irm https://claude.ai/install.ps1 | iex\"");
	Claude.DocumentationUrl = TEXT("https://docs.anthropic.com/en/docs/claude-code/quickstart");
	Result.Add(Claude);

	FHyperAIStudioPrerequisiteStatus Gemini;
	Gemini.Id = TEXT("gemini");
	Gemini.DisplayName = TEXT("Gemini CLI");
	Gemini.Detail = TEXT("External agent CLI");
	Gemini.bRequired = false;
	Gemini.bAvailable = IsExecutableOnPath(TEXT("gemini"));
	Gemini.bEnabled = Gemini.bAvailable;
	Gemini.bCanEnable = false;
	const bool bGeminiInstallerAvailable = IsExecutableOnPath(TEXT("node")) && IsExecutableOnPath(TEXT("npm"));
	Gemini.ActionHint = Gemini.bAvailable
		? TEXT("Available on PATH")
		: (bGeminiInstallerAvailable
			? TEXT("Gemini CLI was not found in Unreal's PATH or common user install folders. Install it, then restart Unreal if it still shows Missing.")
			: TEXT("Gemini CLI install requires Node.js 20+ and npm. Install Node.js LTS first, or click Install to open a visible terminal with the exact blocker."));
	Gemini.InstallCommand = TEXT("npm install -g @google/gemini-cli");
	Gemini.DocumentationUrl = TEXT("https://developers.google.com/gemini-code-assist/docs/gemini-cli");
	Result.Add(Gemini);

	FHyperAIStudioPrerequisiteStatus Cursor;
	Cursor.Id = TEXT("cursor");
	Cursor.DisplayName = TEXT("Cursor");
	Cursor.Detail = TEXT("External editor launcher");
	Cursor.bRequired = false;
	Cursor.bAvailable = IsExecutableOnPath(TEXT("cursor"));
	Cursor.bEnabled = Cursor.bAvailable;
	Cursor.bCanEnable = false;
	Cursor.ActionHint = Cursor.bAvailable ? TEXT("Available on PATH") : TEXT("Cursor launcher was not found in Unreal's PATH or common user install folders. Install Cursor, then restart Unreal if it still shows Missing.");
	Cursor.InstallCommand = TEXT("winget install -e --id Anysphere.Cursor");
	Cursor.DocumentationUrl = TEXT("https://cursor.com/docs/mcp");
	Result.Add(Cursor);

	FHyperAIStudioPrerequisiteStatus VSCode;
	VSCode.Id = TEXT("vscode");
	VSCode.DisplayName = TEXT("VS Code / Copilot");
	VSCode.Detail = TEXT("External editor launcher");
	VSCode.bRequired = false;
	VSCode.bAvailable = IsExecutableOnPath(TEXT("code"));
	VSCode.bEnabled = VSCode.bAvailable;
	VSCode.bCanEnable = false;
	VSCode.ActionHint = VSCode.bAvailable ? TEXT("Available on PATH") : TEXT("VS Code launcher was not found in Unreal's PATH or common user install folders. Install VS Code, then restart Unreal if it still shows Missing.");
	VSCode.InstallCommand = TEXT("winget install -e --id Microsoft.VisualStudioCode");
	VSCode.DocumentationUrl = TEXT("https://code.visualstudio.com/docs/agent-customization/mcp-servers");
	Result.Add(VSCode);

	FHyperAIStudioPrerequisiteStatus Git;
	Git.Id = TEXT("git");
	Git.DisplayName = TEXT("Git");
	Git.Detail = TEXT("Source control CLI and recommended Claude Code shell support on Windows");
	Git.bRequired = false;
	Git.bAvailable = IsExecutableOnPath(TEXT("git"));
	Git.bEnabled = Git.bAvailable;
	Git.bCanEnable = false;
	Git.ActionHint = Git.bAvailable ? TEXT("Available on PATH") : TEXT("Install Git for normal source control workflows and optional Claude Code Git Bash support.");
	Git.InstallCommand = TEXT("winget install -e --id Git.Git");
	Git.DocumentationUrl = TEXT("https://git-scm.com/download/win");
	Result.Add(Git);

	FHyperAIStudioPrerequisiteStatus Node;
	Node.Id = TEXT("node");
	Node.DisplayName = TEXT("Node.js");
	Node.Detail = TEXT("Often required by community MCP servers");
	Node.bRequired = false;
	Node.bAvailable = IsExecutableOnPath(TEXT("node"));
	Node.bEnabled = Node.bAvailable;
	Node.bCanEnable = false;
	Node.ActionHint = Node.bAvailable ? TEXT("Available on PATH") : TEXT("Install Node.js 20+ LTS for Gemini CLI and npm/npx command-based MCP servers.");
	Node.InstallCommand = TEXT("winget install -e --id OpenJS.NodeJS.LTS");
	Node.DocumentationUrl = TEXT("https://nodejs.org/en/download");
	Result.Add(Node);

	FHyperAIStudioPrerequisiteStatus Npm;
	Npm.Id = TEXT("npm");
	Npm.DisplayName = TEXT("npm");
	Npm.Detail = TEXT("Used by many MCP server packages");
	Npm.bRequired = false;
	Npm.bAvailable = IsExecutableOnPath(TEXT("npm"));
	Npm.bEnabled = Npm.bAvailable;
	Npm.bCanEnable = false;
	Npm.ActionHint = Npm.bAvailable ? TEXT("Available on PATH") : TEXT("Install Node.js LTS; npm is included with the normal Node.js install.");
	Npm.InstallCommand = Node.InstallCommand;
	Npm.DocumentationUrl = Node.DocumentationUrl;
	Result.Add(Npm);

	FHyperAIStudioPrerequisiteStatus Npx;
	Npx.Id = TEXT("npx");
	Npx.DisplayName = TEXT("npx");
	Npx.Detail = TEXT("Common launcher for stdio MCP servers");
	Npx.bRequired = false;
	Npx.bAvailable = IsExecutableOnPath(TEXT("npx"));
	Npx.bEnabled = Npx.bAvailable;
	Npx.bCanEnable = false;
	Npx.ActionHint = Npx.bAvailable ? TEXT("Available on PATH") : TEXT("Install Node.js LTS; npx is included with the normal Node.js/npm install.");
	Npx.InstallCommand = Node.InstallCommand;
	Npx.DocumentationUrl = Node.DocumentationUrl;
	Result.Add(Npx);

	FHyperAIStudioPrerequisiteStatus Uv;
	Uv.Id = TEXT("uv");
	Uv.DisplayName = TEXT("uv");
	Uv.Detail = TEXT("Optional launcher for Python-based MCP servers");
	Uv.bRequired = false;
	Uv.bAvailable = IsExecutableOnPath(TEXT("uv"));
	Uv.bEnabled = Uv.bAvailable;
	Uv.bCanEnable = false;
	Uv.ActionHint = Uv.bAvailable ? TEXT("Available on PATH") : TEXT("Optional. Install uv when a community MCP profile asks for uv or uvx.");
	Uv.InstallCommand = TEXT("powershell -ExecutionPolicy Bypass -Command \"irm https://astral.sh/uv/install.ps1 | iex\"");
	Uv.DocumentationUrl = TEXT("https://docs.astral.sh/uv/getting-started/installation/");
	Result.Add(Uv);

	return Result;
}

TArray<FHyperAIStudioTestCommand> FHyperAIStudioService::GetTestCommands() const
{
	const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
	const FString AgentName = HyperAIStudio::Private::NormalizeAgentName(FString(), Settings);
	const uint32 Port = Settings ? Settings->UnrealMCPPort : UE::ModelContextProtocol::DefaultServerPort;
	const FString UrlPath = Settings ? HyperAIStudio::Private::CleanUrlPath(Settings->UnrealMCPUrlPath) : FString(UE::ModelContextProtocol::DefaultServerUrlPath);
	const FString WaitScript = FPaths::Combine(GetProjectRoot(), TEXT(".hyperai"), TEXT("scripts"), TEXT("Wait-HyperAIStudioMCP.ps1"));

	TArray<FHyperAIStudioTestCommand> Commands;
	Commands.Add({
		TEXT("terminal-connection"),
		TEXT("Test connection in terminal"),
		TEXT("Runs the project-local MCP readiness probe and writes .hyperai/runtime/hyperai-status.json."),
		TEXT("Run the terminal readiness test, then report whether Unreal MCP is reachable."),
		AgentName,
		FString::Printf(TEXT("powershell -ExecutionPolicy Bypass -File %s -Port %u -UrlPath %s -TimeoutSeconds 90"),
			*HyperAIStudio::Private::QuotePowerShellSingle(WaitScript),
			Port,
			*HyperAIStudio::Private::QuotePowerShellSingle(UrlPath))
	});
		Commands.Add({
			TEXT("agent-connection"),
			TEXT("Test in agent app"),
			TEXT("Starts a fresh project-root agent session after the Unreal endpoint is ready and explains tool-search discovery clearly."),
			TEXT("Use native `unreal-mcp` when this task exposes it. In tool-search mode, 3 advertised tools are discovery dispatchers, not the full Epic and HyperAI inventory. If `unreal-mcp` is absent, this task did not load the project MCP config; report that separately from editor endpoint health, then wait."),
			AgentName,
			FString()
		});
	Commands.Add({
			TEXT("current-level-actors"),
			TEXT("Current level and actors"),
			TEXT("Asks the agent to inspect the open level and selected actors through Unreal MCP."),
				TEXT("Use native MCP only. Call `editor_toolset.toolsets.scene.SceneTools.get_current_level`, then `editor_toolset.toolsets.scene.SceneTools.find_actors` with `{ \"name\": \"\", \"tag\": \"\", \"collision_channels\": [] }`. Return the level and actor names."),
			AgentName,
			FString()
		});
	Commands.Add({
		TEXT("agent-instructions"),
		TEXT("Inspect agent instructions"),
		TEXT("Checks that generated instructions exist and are clear."),
		TEXT("Check the generated instruction file for your agent and mention any missing setup files."),
		AgentName,
		FString()
	});
	Commands.Add({
		TEXT("permission-settings"),
		TEXT("Instruction file check"),
		TEXT("Checks that generated agent rules point at native Unreal MCP."),
		TEXT("Check the generated instruction file for your agent and tell me whether it names native `unreal-mcp` and the direct level/actor fast paths."),
		AgentName,
		FString()
	});
	Commands.Add({
		TEXT("reconnect-drill"),
		TEXT("Reconnect after crash"),
		TEXT("Exercises the start/wait/reconnect instructions without assuming a persistent session."),
		TEXT("Explain the exact steps you would take if Unreal Editor crashed, including which .hyperai scripts to run and how you reconnect to unreal-mcp."),
		AgentName,
		FString()
	});

	return Commands;
}

FHyperAIStudioStatus FHyperAIStudioService::GetStatusSync() const
{
	const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
	const UModelContextProtocolSettings* MCPSettings = GetDefault<UModelContextProtocolSettings>();

	FHyperAIStudioStatus Status;
	Status.ConfiguredPort = Settings ? Settings->UnrealMCPPort : UE::ModelContextProtocol::DefaultServerPort;
	Status.UrlPath = Settings ? HyperAIStudio::Private::CleanUrlPath(Settings->UnrealMCPUrlPath) : FString(UE::ModelContextProtocol::DefaultServerUrlPath);
	Status.Endpoint = GetEndpoint(Status.ConfiguredPort, Status.UrlPath);

	IModelContextProtocolModule* Module = FModuleManager::Get().LoadModulePtr<IModelContextProtocolModule>(TEXT("ModelContextProtocol"));
	Status.bUnrealMCPModuleAvailable = Module != nullptr;

	if (MCPSettings && Settings)
	{
		Status.bUnrealMCPSettingsConfigured =
			MCPSettings->ServerPortNumber == Settings->UnrealMCPPort
			&& MCPSettings->ServerUrlPath == Status.UrlPath
			&& MCPSettings->bAutoStartServer == Settings->bAutoStartUnrealMCP
			&& MCPSettings->bEnableToolSearch == Settings->bEnableToolSearch;
	}

	if (Module)
	{
		Status.RegisteredToolCount = Module->GetTools().Num();
		if (FModelContextProtocolServer* Server = Module->GetServer())
		{
			Status.bServerRunning = Server->IsServerRunning();
			Status.ActivePort = Server->GetServerPort();
		}
	}

	Status.bRequiredPluginsReady = true;
	TArray<FString> MissingRequiredPlugins;
	const TArray<FString> FoundationPlugins = GetRequiredPluginNames();
	for (const FString& PluginName : FoundationPlugins)
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(PluginName);
		if (!Plugin.IsValid() || !Plugin->IsEnabled())
		{
			Status.bRequiredPluginsReady = false;
			MissingRequiredPlugins.Add(PluginName);
		}
	}

	const TArray<FString> DesiredCapabilityPlugins = Settings
		? FHyperAIStudioCapabilityPresetResolver::ResolveDesiredPlugins(*Settings)
		: FoundationPlugins;
	Status.DesiredCapabilityPluginCount = DesiredCapabilityPlugins.Num();
	Status.bDesiredCapabilityPluginsReady = true;
	TArray<FString> MissingSelectedCapabilityPlugins;
	for (const FString& PluginName : DesiredCapabilityPlugins)
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(PluginName);
		if (Plugin.IsValid() && Plugin->IsEnabled())
		{
			++Status.EnabledCapabilityPluginCount;
		}
		else
		{
			Status.bDesiredCapabilityPluginsReady = false;
			if (!FoundationPlugins.Contains(PluginName))
			{
				MissingSelectedCapabilityPlugins.Add(PluginName);
			}
		}
	}
	if (Settings)
	{
		if (const UEnum* PresetEnum = StaticEnum<EHyperAIStudioCapabilityPreset>())
		{
			Status.CapabilityPresetName = PresetEnum->GetDisplayNameTextByValue(
				static_cast<int64>(Settings->CapabilityPreset)).ToString();
		}
	}

	Status.bPortListening = IsPortListening(Status.ConfiguredPort);
	Status.bAgentFilesReady = AreAgentFilesReady(GetProjectRoot(), Settings);
	Status.ConfiguredAgentCount = CountConfiguredAgents(GetProjectRoot(), Settings);
	Status.SupportedAgentCount = 5;
	if (Status.bServerRunning && Status.bPortListening)
	{
		if (const TOptional<FHyperAIStudioCapabilityInventoryResult> CachedInventory =
			FHyperAIStudioCapabilityInventoryClient::GetCached(Status.Endpoint))
		{
			HyperAIStudio::Private::ApplyCapabilityInventoryResult(Status, CachedInventory.GetValue());
		}
	}

	if (!Status.bUnrealMCPModuleAvailable)
	{
		Status.Warnings.Add(TEXT("Epic ModelContextProtocol module is not loaded."));
	}
	if (!Status.bRequiredPluginsReady)
	{
		Status.Warnings.Add(FString::Printf(TEXT("Required Unreal plugins need enable/restart before editor tools are exposed: %s."), *HyperAIStudio::Private::JoinReadable(MissingRequiredPlugins, TEXT("none"))));
	}
	if (!MissingSelectedCapabilityPlugins.IsEmpty())
	{
		Status.Warnings.Add(FString::Printf(
			TEXT("The selected `%s` capability preset is not fully effective until these plugins are available/enabled and Unreal restarts: %s. Sync the Epic MCP toolsets to restore full readiness."),
			Status.CapabilityPresetName.IsEmpty() ? TEXT("custom") : *Status.CapabilityPresetName,
			*FString::Join(MissingSelectedCapabilityPlugins, TEXT(", "))));
	}
	if (Status.bServerRunning && Status.ActivePort != Status.ConfiguredPort)
	{
		Status.Warnings.Add(FString::Printf(TEXT("Unreal MCP is running on port %u, but HyperAIStudio is configured for %u."), Status.ActivePort, Status.ConfiguredPort));
	}
	if (Status.bServerRunning && !Status.bPortListening)
	{
		Status.Warnings.Add(FString::Printf(TEXT("The configured Unreal MCP endpoint is not listening on port %u."), Status.ConfiguredPort));
	}
	if (!Status.bAgentFilesReady)
	{
		Status.Warnings.Add(TEXT("Project agent files are missing or incomplete."));
	}
	if (Status.ConfiguredAgentCount <= 0)
	{
		Status.Warnings.Add(TEXT("No external agent is configured yet."));
	}
	Status.Summary = Status.IsReady() ? TEXT("Ready") : TEXT("Setup needed");
	return Status;
}

FHyperAIStudioStatus FHyperAIStudioService::GetMcpRuntimeStatusFast() const
{
	const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
	const UModelContextProtocolSettings* MCPSettings = GetDefault<UModelContextProtocolSettings>();

	FHyperAIStudioStatus Status;
	Status.ConfiguredPort = Settings
		? Settings->UnrealMCPPort
		: UE::ModelContextProtocol::DefaultServerPort;
	Status.UrlPath = Settings
		? HyperAIStudio::Private::CleanUrlPath(Settings->UnrealMCPUrlPath)
		: FString(UE::ModelContextProtocol::DefaultServerUrlPath);
	Status.Endpoint = GetEndpoint(Status.ConfiguredPort, Status.UrlPath);
	Status.bToolSearchMode = MCPSettings && MCPSettings->bEnableToolSearch;
	if (MCPSettings && Settings)
	{
		Status.bUnrealMCPSettingsConfigured =
			MCPSettings->ServerPortNumber == Settings->UnrealMCPPort
			&& MCPSettings->ServerUrlPath == Status.UrlPath
			&& MCPSettings->bAutoStartServer == Settings->bAutoStartUnrealMCP
			&& MCPSettings->bEnableToolSearch == Settings->bEnableToolSearch;
	}

	IModelContextProtocolModule* Module =
		FModuleManager::Get().GetModulePtr<IModelContextProtocolModule>(TEXT("ModelContextProtocol"));
	Status.bUnrealMCPModuleAvailable = Module != nullptr;
	if (Module)
	{
		Status.RegisteredToolCount = Module->GetTools().Num();
		if (FModelContextProtocolServer* Server = Module->GetServer())
		{
			Status.bServerRunning = Server->IsServerRunning();
			Status.ActivePort = Server->GetServerPort();
			// Epic owns the bound HTTP router; a running router on the configured port is
			// stronger and cheaper evidence than opening another loopback socket here.
			Status.bPortListening = Status.bServerRunning
				&& Status.ActivePort == Status.ConfiguredPort;
		}
	}

	Status.Summary = !Status.bUnrealMCPModuleAvailable
		? TEXT("MCP module unavailable")
		: (!Status.bServerRunning
			? TEXT("MCP server stopped")
			: (Status.bPortListening ? TEXT("MCP runtime ready") : TEXT("MCP port mismatch")));
	return Status;
}

void FHyperAIStudioService::RefreshStatusAsync(TFunction<void(const FHyperAIStudioStatus&)> OnComplete) const
{
	FHyperAIStudioStatus Status = GetStatusSync();

	if (!Status.bServerRunning || !Status.bPortListening)
	{
		Status.bHasProbeRun = true;
		Status.bToolsListReachable = false;
		Status.ProbeMessage = Status.bServerRunning
			? FString::Printf(TEXT("Unreal MCP is running, but the configured endpoint is not listening on %s."), *Status.Endpoint)
			: FString::Printf(TEXT("Unreal MCP is not running on %s yet."), *Status.Endpoint);
		OnComplete(Status);
		return;
	}

	Status.bProbeInProgress = true;
	OnComplete(Status);

	const FString Endpoint = Status.Endpoint;
	ProbeToolsListAsync(Endpoint, [this, Endpoint, OnComplete](bool bReachable, const FString& Message, int32 ToolCount)
	{
		FHyperAIStudioStatus UpdatedStatus = GetStatusSync();
		UpdatedStatus.bHasProbeRun = true;
		UpdatedStatus.bProbeInProgress = false;
		UpdatedStatus.bToolsListReachable = bReachable;
		UpdatedStatus.ProbeMessage = Message;
		UpdatedStatus.ProbeToolCount = ToolCount;
		UpdatedStatus.bCapabilityInventoryInProgress = bReachable;
		UpdatedStatus.Summary = UpdatedStatus.IsReady() ? TEXT("Ready") : TEXT("Setup needed");
		if (UpdatedStatus.IsReady())
		{
			if (UHyperAIStudioSettings* MutableSettings = GetMutableDefault<UHyperAIStudioSettings>();
				MutableSettings && MutableSettings->bResumeSetupAfterRestart)
			{
				MutableSettings->bResumeSetupAfterRestart = false;
				MutableSettings->SaveConfig();
			}
		}
		WriteStatusJson(GetProjectRoot(), UpdatedStatus);
		OnComplete(UpdatedStatus);

		if (bReachable)
		{
			FHyperAIStudioCapabilityInventoryClient::RefreshAsync(Endpoint, false,
				[this, OnComplete, UpdatedStatus](const FHyperAIStudioCapabilityInventoryResult& Inventory) mutable
				{
					HyperAIStudio::Private::ApplyCapabilityInventoryResult(UpdatedStatus, Inventory);
					if (!Inventory.bSuccess)
					{
						UpdatedStatus.Warnings.AddUnique(FString::Printf(
							TEXT("Deep Unreal MCP capability inventory is unavailable: %s"),
							*Inventory.Message));
					}
					WriteStatusJson(GetProjectRoot(), UpdatedStatus);
					OnComplete(UpdatedStatus);
				});
		}
	});
}

EHyperAIStudioSetupOutcome FHyperAIStudioService::SetUpHyperAIStudio(FText& OutMessage)
{
	FText PluginMessage;
	bool bRestartRequired = false;
	if (!EnableRequiredPlugins(PluginMessage, &bRestartRequired))
	{
		OutMessage = PluginMessage;
		return EHyperAIStudioSetupOutcome::Failed;
	}

	UHyperAIStudioSettings* MutableSettings = GetMutableDefault<UHyperAIStudioSettings>();
	if (!MutableSettings)
	{
		OutMessage = FText::FromString(TEXT("HyperAIStudio settings are unavailable."));
		return EHyperAIStudioSetupOutcome::Failed;
	}
	if (bRestartRequired)
	{
		MutableSettings->bResumeSetupAfterRestart = true;
		MutableSettings->SaveConfig();
	}

	if (!ConfigureUnrealMCPSettings(OutMessage))
	{
		return EHyperAIStudioSetupOutcome::Failed;
	}

	FText FilesMessage;
	if (!GenerateProjectFiles(FilesMessage))
	{
		OutMessage = FilesMessage;
		return EHyperAIStudioSetupOutcome::Failed;
	}

	if (bRestartRequired)
	{
		OutMessage = FText::Format(
			NSLOCTEXT("HyperAIStudio", "SetupPreparedForRestart", "Setup prepared successfully: Epic MCP toolsets were synchronized and agent instructions, configs, and helper scripts were generated. Unreal must restart once; HyperAIStudio will finish setup automatically after relaunch. {0}"),
			PluginMessage);
		return EHyperAIStudioSetupOutcome::RestartRequired;
	}

	FText StartMessage;
	if (!StartUnrealMCP(StartMessage))
	{
		OutMessage = StartMessage;
		return EHyperAIStudioSetupOutcome::Failed;
	}

	// Refresh once after a normal setup or the automatic post-restart resume. Keep this out of
	// StartUnrealMCP: agent launches may call that helper repeatedly.
	if (IModelContextProtocolModule* MCPModule =
		FModuleManager::Get().GetModulePtr<IModelContextProtocolModule>(TEXT("ModelContextProtocol")))
	{
		MCPModule->RefreshTools();
		const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
		FHyperAIStudioCapabilityInventoryClient::Invalidate(GetEndpoint(
			Settings ? Settings->UnrealMCPPort : UE::ModelContextProtocol::DefaultServerPort,
			Settings ? Settings->UnrealMCPUrlPath : FString(UE::ModelContextProtocol::DefaultServerUrlPath)));
	}

	OutMessage = FText::FromString(TEXT("HyperAIStudio setup finished. Agent files are ready and Unreal MCP tools were refreshed; readiness validation is running."));
	return EHyperAIStudioSetupOutcome::Complete;
}

bool FHyperAIStudioService::EnableRequiredPlugins(FText& OutMessage, bool* bOutRestartRequired) const
{
	bool bChanged = false;
	bool bRestartRequired = false;
	if (bOutRestartRequired)
	{
		*bOutRestartRequired = false;
	}
	TArray<FString> MissingSelectedPlugins;
	TArray<FString> FailedSelectedPlugins;
	const TArray<FString> FoundationPlugins = GetRequiredPluginNames();
	const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
	const TArray<FString> DesiredPlugins = Settings
		? FHyperAIStudioCapabilityPresetResolver::ResolvePluginsToEnable(*Settings)
		: FoundationPlugins;

	for (const FString& PluginName : DesiredPlugins)
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(PluginName);
		if (!Plugin.IsValid())
		{
			if (FoundationPlugins.Contains(PluginName))
			{
				OutMessage = FText::FromString(FString::Printf(TEXT("Required Unreal plugin `%s` was not found in this engine install."), *PluginName));
				return false;
			}
			MissingSelectedPlugins.Add(PluginName);
			continue;
		}

		const bool bRuntimeEnabled = Plugin->IsEnabled();
		bool bProjectDescriptorEnabled = false;
		if (const FProjectDescriptor* ProjectDescriptor = IProjectManager::Get().GetCurrentProject())
		{
			if (const FPluginReferenceDescriptor* Reference = ProjectDescriptor->Plugins.FindByPredicate(
				[&PluginName](const FPluginReferenceDescriptor& Candidate)
				{
					return Candidate.Name.Equals(PluginName, ESearchCase::IgnoreCase);
				}))
			{
				bProjectDescriptorEnabled = Reference->bEnabled;
			}
		}

		const FHyperAIStudioPluginSyncDecision Decision = GetPluginSyncDecision(
			bRuntimeEnabled,
			bProjectDescriptorEnabled);
		if (Decision.bWriteProjectDescriptor)
		{
			FText FailReason;
			if (!IProjectManager::Get().SetPluginEnabled(PluginName, true, FailReason))
			{
				if (FoundationPlugins.Contains(PluginName))
				{
					OutMessage = FText::Format(NSLOCTEXT("HyperAIStudio", "EnablePluginFailed", "Could not enable {0}: {1}"), FText::FromString(PluginName), FailReason);
					return false;
				}
				FailedSelectedPlugins.Add(FString::Printf(TEXT("%s (%s)"), *PluginName, *FailReason.ToString()));
				continue;
			}
			bChanged = true;
			bRestartRequired = true;
		}
		else
		{
			bRestartRequired |= Decision.bRestartRequired;
		}
	}
	if (bOutRestartRequired)
	{
		*bOutRestartRequired = bRestartRequired;
	}

	const FString SelectedWarning = MissingSelectedPlugins.IsEmpty() && FailedSelectedPlugins.IsEmpty()
		? FString()
		: FString::Printf(
			TEXT(" Selected capability plugins still unavailable: %s%s%s."),
			*FString::Join(MissingSelectedPlugins, TEXT(", ")),
			!MissingSelectedPlugins.IsEmpty() && !FailedSelectedPlugins.IsEmpty() ? TEXT("; ") : TEXT(""),
			*FString::Join(FailedSelectedPlugins, TEXT(", ")));

	if (bChanged)
	{
		if (Settings)
		{
			FHyperAIStudioCapabilityInventoryClient::Invalidate(GetEndpoint(
				Settings->UnrealMCPPort,
				HyperAIStudio::Private::CleanUrlPath(Settings->UnrealMCPUrlPath)));
		}
		FText FailReason;
		if (!IProjectManager::Get().SaveCurrentProjectToDisk(FailReason))
		{
			OutMessage = FText::Format(NSLOCTEXT("HyperAIStudio", "SaveProjectFailed", "Required plugins were enabled, but the project file could not be saved: {0}"), FailReason);
			return false;
		}

		OutMessage = FText::FromString(FString::Printf(TEXT("Epic MCP toolsets were synchronized in the project. Unreal must restart once to load them.%s"), *SelectedWarning));
		return true;
	}

	OutMessage = bRestartRequired
		? FText::FromString(FString::Printf(
			TEXT("Epic MCP toolsets are already synchronized in the project and are waiting for Unreal to restart.%s"),
			*SelectedWarning))
		: FText::FromString(FString::Printf(
			TEXT("Required Unreal plugins and Epic MCP toolsets are already active.%s"),
			*SelectedWarning));
	return true;
}

bool FHyperAIStudioService::StartUnrealMCP(FText& OutMessage) const
{
	const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
	const uint32 Port = Settings ? Settings->UnrealMCPPort : UE::ModelContextProtocol::DefaultServerPort;
	const FString UrlPath = Settings ? HyperAIStudio::Private::CleanUrlPath(Settings->UnrealMCPUrlPath) : FString(UE::ModelContextProtocol::DefaultServerUrlPath);

	IModelContextProtocolModule* Module = FModuleManager::Get().LoadModulePtr<IModelContextProtocolModule>(TEXT("ModelContextProtocol"));
	if (!Module)
	{
		OutMessage = FText::FromString(TEXT("Could not load Epic ModelContextProtocol module."));
		return false;
	}

	if (FModelContextProtocolServer* Server = Module->GetServer())
	{
		if (Server->IsServerRunning() && Server->GetServerPort() == Port)
		{
			OutMessage = FText::FromString(TEXT("Unreal MCP is already running."));
			return true;
		}

		if (Server->IsServerRunning())
		{
			const FString PreviousRunningEndpoint = GetEndpoint(Server->GetServerPort(), UrlPath);
			Module->StopServer();
			if (Server->IsServerRunning())
			{
				OutMessage = FText::FromString(TEXT("Could not confirm that the previous Unreal MCP server stopped; start was not attempted."));
				return false;
			}
			FHyperAIStudioCapabilityInventoryClient::NotifyConfirmedServerStopped(PreviousRunningEndpoint);
		}
	}

	Module->StartServer(Port, UrlPath);
	FHyperAIStudioCapabilityInventoryClient::Invalidate(GetEndpoint(Port, UrlPath));
	OutMessage = FText::FromString(FString::Printf(TEXT("Unreal MCP start requested on %s."), *GetEndpoint(Port, UrlPath)));
	return true;
}

bool FHyperAIStudioService::StopUnrealMCP(FText& OutMessage) const
{
	const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
	const uint32 Port = Settings ? Settings->UnrealMCPPort : UE::ModelContextProtocol::DefaultServerPort;
	const FString UrlPath = Settings
		? HyperAIStudio::Private::CleanUrlPath(Settings->UnrealMCPUrlPath)
		: FString(UE::ModelContextProtocol::DefaultServerUrlPath);
	IModelContextProtocolModule* Module = FModuleManager::Get().LoadModulePtr<IModelContextProtocolModule>(TEXT("ModelContextProtocol"));
	if (!Module)
	{
		OutMessage = FText::FromString(TEXT("Could not load Epic ModelContextProtocol module."));
		return false;
	}

	FModelContextProtocolServer* Server = Module->GetServer();
	const FString RunningEndpoint = Server && Server->IsServerRunning()
		? GetEndpoint(Server->GetServerPort(), UrlPath)
		: GetEndpoint(Port, UrlPath);
	Module->StopServer();
	if (Server && Server->IsServerRunning())
	{
		OutMessage = FText::FromString(TEXT("Unreal MCP stop was requested, but the server is still running; ambiguous calls remain locked."));
		return false;
	}
	if (Server)
	{
		// The in-process Epic server object now positively reports stopped, so no request from
		// its previous epoch can continue executing. A missing object is not used as reset proof.
		FHyperAIStudioCapabilityInventoryClient::NotifyConfirmedServerStopped(RunningEndpoint);
	}
	else
	{
		FHyperAIStudioCapabilityInventoryClient::Invalidate(RunningEndpoint);
	}
	OutMessage = FText::FromString(TEXT("Unreal MCP stopped."));
	return true;
}

bool FHyperAIStudioService::RestartUnrealMCP(FText& OutMessage) const
{
	FText Ignored;
	if (!StopUnrealMCP(Ignored))
	{
		OutMessage = Ignored;
		return false;
	}
	return StartUnrealMCP(OutMessage);
}

bool FHyperAIStudioService::GenerateProjectFiles(FText& OutMessage) const
{
	UHyperAIStudioSettings* MutableSettings = GetMutableDefault<UHyperAIStudioSettings>();
	if (!MutableSettings)
	{
		OutMessage = FText::FromString(TEXT("HyperAIStudio settings are unavailable."));
		return false;
	}

	const FString ProjectRoot = GetProjectRoot();
	const FString Endpoint = GetEndpoint(MutableSettings->UnrealMCPPort, MutableSettings->UnrealMCPUrlPath);

	FText StepMessage;
	if (!GenerateManagedProjectFilesTransactional(ProjectRoot, Endpoint, MutableSettings, StepMessage))
	{
		OutMessage = StepMessage;
		return false;
	}
	const FString ConfigMessage = StepMessage.ToString();

	FHyperAIStudioStatus Status = GetStatusSync();
	WriteStatusJson(ProjectRoot, Status);

	OutMessage = FText::FromString(FString::Printf(
		TEXT("Agent files and helper scripts generated. %s"),
		*ConfigMessage));
	return true;
}

bool FHyperAIStudioService::CreateQuickConnectPromptPackage(const FString& AgentName, FHyperAIStudioPromptPackage& OutPackage, FText& OutMessage) const
{
	OutPackage = FHyperAIStudioPromptPackage();

	const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
	if (!Settings)
	{
		OutMessage = FText::FromString(TEXT("HyperAIStudio settings are unavailable."));
		return false;
	}

	const FString ProjectRoot = GetProjectRoot();
	const FString ResolvedAgentName = HyperAIStudio::Private::NormalizeAgentName(AgentName, Settings);

	if (!AreAgentFilesReady(ProjectRoot, Settings))
	{
		FText FilesMessage;
		if (!GenerateProjectFiles(FilesMessage))
		{
			OutMessage = FText::Format(NSLOCTEXT("HyperAIStudio", "QuickPromptFilesGenerationFailed", "Agent files were missing and could not be generated: {0}"), FilesMessage);
			return false;
		}
	}

	if (!HyperAIStudio::Private::WriteQuickPromptPackageFiles(
		ProjectRoot,
		ResolvedAgentName,
		HyperAIStudio::Private::BuildQuickConnectPrompt(),
		OutPackage,
		OutMessage))
	{
		return false;
	}

	if (Settings->bAllowClipboardPromptCopy)
	{
		CopyTextToClipboard(OutPackage.PromptText);
	}

	return true;
}

bool FHyperAIStudioService::CreatePromptPackage(const FString& UserIntent, const FString& AgentName, FHyperAIStudioPromptPackage& OutPackage, FText& OutMessage) const
{
	OutPackage = FHyperAIStudioPromptPackage();

	const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
	if (!Settings)
	{
		OutMessage = FText::FromString(TEXT("HyperAIStudio settings are unavailable."));
		return false;
	}

	const FString ProjectRoot = GetProjectRoot();
	const FString Endpoint = GetEndpoint(Settings->UnrealMCPPort, Settings->UnrealMCPUrlPath);
	const FString ResolvedAgentName = HyperAIStudio::Private::NormalizeAgentName(AgentName, Settings);
	const FString Intent = UserIntent.IsEmpty()
		? FString::Printf(TEXT("Say you are connected to %s and ready, then wait for my task."), *HyperAIStudio::Private::GetProjectDisplayName())
		: UserIntent;

	if (!AreAgentFilesReady(ProjectRoot, Settings))
	{
		FText FilesMessage;
		if (!GenerateProjectFiles(FilesMessage))
		{
			OutMessage = FText::Format(NSLOCTEXT("HyperAIStudio", "PromptFilesGenerationFailed", "Agent files were missing and could not be generated: {0}"), FilesMessage);
			return false;
		}
	}

	const TSharedRef<FJsonObject> Context = HyperAIStudio::Private::CaptureContextObject(ProjectRoot, Endpoint, ResolvedAgentName, Intent, nullptr, nullptr);
	if (!HyperAIStudio::Private::WritePromptPackageFiles(ProjectRoot, Endpoint, ResolvedAgentName, Intent, Context, OutPackage, OutMessage))
	{
		return false;
	}
	if (Settings->bAllowClipboardPromptCopy)
	{
		CopyTextToClipboard(OutPackage.PromptText);
	}

	return true;
}

bool FHyperAIStudioService::CreatePromptPackageForAssets(const FString& UserIntent, const FString& AgentName, const TArray<FAssetData>& SelectedAssets, FHyperAIStudioPromptPackage& OutPackage, FText& OutMessage) const
{
	OutPackage = FHyperAIStudioPromptPackage();

	const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
	if (!Settings)
	{
		OutMessage = FText::FromString(TEXT("HyperAIStudio settings are unavailable."));
		return false;
	}
	if (SelectedAssets.IsEmpty())
	{
		OutMessage = FText::FromString(TEXT("No Content Browser assets were selected."));
		return false;
	}

	const FString ProjectRoot = GetProjectRoot();
	const FString Endpoint = GetEndpoint(Settings->UnrealMCPPort, Settings->UnrealMCPUrlPath);
	const FString ResolvedAgentName = HyperAIStudio::Private::NormalizeAgentName(AgentName, Settings);
	const FString Intent = UserIntent.IsEmpty()
		? FString(TEXT("Use the selected Content Browser assets as context. Identify what they are, how they are referenced, and one safe improvement path before making changes."))
		: UserIntent;

	if (!AreAgentFilesReady(ProjectRoot, Settings))
	{
		FText FilesMessage;
		if (!GenerateProjectFiles(FilesMessage))
		{
			OutMessage = FText::Format(NSLOCTEXT("HyperAIStudio", "AssetPromptFilesGenerationFailed", "Agent files were missing and could not be generated: {0}"), FilesMessage);
			return false;
		}
	}

	const TSharedRef<FJsonObject> Context = HyperAIStudio::Private::CaptureContextObject(ProjectRoot, Endpoint, ResolvedAgentName, Intent, nullptr, nullptr, &SelectedAssets);
	if (!HyperAIStudio::Private::WritePromptPackageFiles(ProjectRoot, Endpoint, ResolvedAgentName, Intent, Context, OutPackage, OutMessage))
	{
		return false;
	}

	if (Settings->bAllowClipboardPromptCopy)
	{
		CopyTextToClipboard(OutPackage.PromptText);
	}

	return true;
}

bool FHyperAIStudioService::CreateBlueprintPromptPackage(const FString& UserIntent, const FString& AgentName, const UEdGraph* Graph, const UEdGraphNode* ContextNode, FHyperAIStudioPromptPackage& OutPackage, FText& OutMessage) const
{
	OutPackage = FHyperAIStudioPromptPackage();

	const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
	if (!Settings)
	{
		OutMessage = FText::FromString(TEXT("HyperAIStudio settings are unavailable."));
		return false;
	}

	const UEdGraph* ContextGraph = Graph ? Graph : (ContextNode ? ContextNode->GetGraph() : nullptr);
	if (!ContextGraph && !ContextNode)
	{
		OutMessage = FText::FromString(TEXT("No Blueprint graph context was available."));
		return false;
	}

	const FString ProjectRoot = GetProjectRoot();
	const FString Endpoint = GetEndpoint(Settings->UnrealMCPPort, Settings->UnrealMCPUrlPath);
	const FString ResolvedAgentName = HyperAIStudio::Private::NormalizeAgentName(AgentName, Settings);
	const FString Intent = UserIntent.IsEmpty()
		? FString(TEXT("Use the selected Blueprint nodes as context. Explain what they do and wait for the user's next concrete instruction."))
		: UserIntent;

	if (!AreAgentFilesReady(ProjectRoot, Settings))
	{
		FText FilesMessage;
		if (!GenerateProjectFiles(FilesMessage))
		{
			OutMessage = FText::Format(NSLOCTEXT("HyperAIStudio", "BlueprintPromptFilesGenerationFailed", "Agent files were missing and could not be generated: {0}"), FilesMessage);
			return false;
		}
	}

	const TSharedRef<FJsonObject> Context = HyperAIStudio::Private::CaptureContextObject(ProjectRoot, Endpoint, ResolvedAgentName, Intent, ContextGraph, ContextNode);
	if (!HyperAIStudio::Private::WritePromptPackageFiles(ProjectRoot, Endpoint, ResolvedAgentName, Intent, Context, OutPackage, OutMessage))
	{
		return false;
	}

	if (Settings->bAllowClipboardPromptCopy)
	{
		CopyTextToClipboard(OutPackage.PromptText);
	}

	return true;
}

bool FHyperAIStudioService::CreateTestPromptPackage(const FString& TestId, const FString& AgentName, FHyperAIStudioPromptPackage& OutPackage, FText& OutMessage) const
{
	if (TestId == TEXT("agent-connection"))
	{
		return CreateQuickConnectPromptPackage(AgentName, OutPackage, OutMessage);
	}

	for (const FHyperAIStudioTestCommand& Command : GetTestCommands())
	{
		if (Command.Id == TestId)
		{
			const FString ResolvedAgentName = AgentName.IsEmpty() ? Command.AgentName : AgentName;
			if (!CreatePromptPackage(Command.Intent, ResolvedAgentName, OutPackage, OutMessage))
			{
				return false;
			}

			if (!Command.TerminalCommand.IsEmpty())
			{
				OutPackage.TerminalCommand = Command.TerminalCommand;
			}
			OutPackage.Message = FString::Printf(TEXT("%s is ready."), *Command.Title);
			OutMessage = FText::FromString(OutPackage.Message);
			return true;
		}
	}

	OutMessage = FText::FromString(FString::Printf(TEXT("Unknown HyperAIStudio test command `%s`."), *TestId));
	return false;
}

bool FHyperAIStudioService::AutoCommentBlueprintNodes(const UEdGraph* Graph, const UEdGraphNode* ContextNode, FText& OutMessage) const
{
	const UEdGraph* ContextGraph = Graph ? Graph : (ContextNode ? ContextNode->GetGraph() : nullptr);
	UEdGraph* MutableGraph = const_cast<UEdGraph*>(ContextGraph);
	if (!MutableGraph)
	{
		OutMessage = FText::FromString(TEXT("No Blueprint graph context was available."));
		return false;
	}

	TArray<const UEdGraphNode*> Nodes = HyperAIStudio::Private::CollectBlueprintContextNodes(ContextGraph, ContextNode);
	Nodes.RemoveAll([](const UEdGraphNode* Node)
	{
		return !Node || Node->IsA<UEdGraphNode_Comment>();
	});

	if (Nodes.Num() == 0)
	{
		OutMessage = FText::FromString(TEXT("Select one or more Blueprint nodes before auto-commenting."));
		return false;
	}

	int32 MinX = TNumericLimits<int32>::Max();
	int32 MinY = TNumericLimits<int32>::Max();
	int32 MaxX = TNumericLimits<int32>::Min();
	int32 MaxY = TNumericLimits<int32>::Min();
	for (const UEdGraphNode* Node : Nodes)
	{
		MinX = FMath::Min(MinX, Node->NodePosX);
		MinY = FMath::Min(MinY, Node->NodePosY);
		MaxX = FMath::Max(MaxX, Node->NodePosX + HyperAIStudio::Private::EstimateNodeWidth(Node));
		MaxY = FMath::Max(MaxY, Node->NodePosY + HyperAIStudio::Private::EstimateNodeHeight(Node));
	}

	constexpr int32 Padding = 72;
	const int32 CommentX = MinX - Padding;
	const int32 CommentY = MinY - Padding;
	const int32 CommentWidth = FMath::Max(360, (MaxX - MinX) + Padding * 2);
	const int32 CommentHeight = FMath::Max(220, (MaxY - MinY) + Padding * 2);

	FScopedTransaction Transaction(NSLOCTEXT("HyperAIStudio", "AutoCommentBlueprintNodesTransaction", "HyperAIStudio Quick Auto Comment Blueprint Nodes"));
	if (UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForGraph(ContextGraph))
	{
		Blueprint->Modify();
	}
	MutableGraph->Modify();

	UEdGraphNode_Comment* CommentNode = NewObject<UEdGraphNode_Comment>(MutableGraph);
	CommentNode->SetFlags(RF_Transactional);
	CommentNode->CreateNewGuid();
	CommentNode->PostPlacedNewNode();
	CommentNode->NodePosX = CommentX;
	CommentNode->NodePosY = CommentY;
	CommentNode->NodeWidth = CommentWidth;
	CommentNode->NodeHeight = CommentHeight;
	CommentNode->NodeComment = HyperAIStudio::Private::BuildAutoCommentText(Nodes);
	CommentNode->FontSize = 18;
	CommentNode->CommentColor = FLinearColor(0.08f, 0.18f, 0.24f, 0.85f);
	CommentNode->MoveMode = ECommentBoxMode::GroupMovement;

	MutableGraph->AddNode(CommentNode, true, false);
	if (UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForGraph(ContextGraph))
	{
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	}
	MutableGraph->NotifyGraphChanged();

	OutMessage = FText::FromString(FString::Printf(TEXT("Added a quick local comment around %d selected Blueprint node(s)."), Nodes.Num()));
	return true;
}

bool FHyperAIStudioService::AddDefaultCustomServer(FText& OutMessage) const
{
	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	if (!Settings)
	{
		OutMessage = FText::FromString(TEXT("HyperAIStudio settings are unavailable."));
		return false;
	}

	const int32 NextIndex = Settings->ExtraServers.Num() + 1;
	FHyperAIStudioMCPServerEntry Entry;
	Entry.Id = FString::Printf(TEXT("custom-mcp-%d"), NextIndex);
	Entry.DisplayName = FString::Printf(TEXT("Custom MCP Server %d"), NextIndex);
	Entry.Description = TEXT("Project-local custom MCP server.");
	Entry.Url = TEXT("http://127.0.0.1:9000/mcp");
	Entry.Priority = 100 + NextIndex;
	Entry.bEnabled = false;
	Settings->ExtraServers.Add(Entry);
	Settings->SaveConfig();

	OutMessage = FText::FromString(TEXT("Added a disabled custom MCP server entry. Edit it in Editor Preferences > HyperAIStudio."));
	return true;
}

bool FHyperAIStudioService::ImportMCPProfileFromClipboard(FText& OutMessage) const
{
	FString ClipboardText;
	FPlatformApplicationMisc::ClipboardPaste(ClipboardText);
	if (ClipboardText.TrimStartAndEnd().IsEmpty())
	{
		OutMessage = FText::FromString(TEXT("Clipboard is empty. Copy a hyperai-mcp.json profile first."));
		return false;
	}

	return ImportMCPProfileFromJson(ClipboardText, OutMessage);
}

bool FHyperAIStudioService::ImportExampleMCPProfile(FText& OutMessage) const
{
	return ImportMCPProfileFromJson(GetExampleMCPProfileJson(), OutMessage);
}

bool FHyperAIStudioService::ImportHyperUEMCPProfile(FText& OutMessage) const
{
	OutMessage = FText::FromString(TEXT("The retired built-in server profile is no longer available. Run setup to migrate managed configs to the single Epic unreal-mcp endpoint."));
	return false;
}

bool FHyperAIStudioService::DisableHyperUEMCPProfile(FText& OutMessage) const
{
	OutMessage = FText::FromString(TEXT("Legacy profiles are retired and cannot be changed through this route. Run setup to apply the fingerprinted, ownership-safe migration; customized entries are preserved fail-closed."));
	return false;
}

bool FHyperAIStudioService::ImportMCPProfileFromJson(const FString& JsonText, FText& OutMessage) const
{
	constexpr int32 MaxImportedProfileCharacters = 1024 * 1024;
	if (JsonText.Len() > MaxImportedProfileCharacters)
	{
		OutMessage = FText::FromString(TEXT("MCP profile JSON exceeds the 1 MiB import limit."));
		return false;
	}

	TSharedPtr<FJsonObject> RootObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
	if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
	{
		OutMessage = FText::FromString(TEXT("Clipboard did not contain valid JSON."));
		return false;
	}

	FString Schema;
	RootObject->TryGetStringField(TEXT("schema"), Schema);
	if (!Schema.IsEmpty() && Schema != TEXT("hyperai.mcp.v1"))
	{
		OutMessage = FText::FromString(FString::Printf(TEXT("Unsupported MCP profile schema `%s`."), *Schema));
		return false;
	}

	FString Id;
	FString Name;
	FString Description;
	FString Transport;
	FString Url;
	FString Command;
	FString InstallCommand;
	FString DocumentationUrl;
	FString ProfileSetupNotes;
	int32 Priority = 100;
	RootObject->TryGetStringField(TEXT("id"), Id);
	RootObject->TryGetStringField(TEXT("name"), Name);
	RootObject->TryGetStringField(TEXT("displayName"), Name);
	RootObject->TryGetStringField(TEXT("description"), Description);
	RootObject->TryGetStringField(TEXT("transport"), Transport);
	RootObject->TryGetStringField(TEXT("url"), Url);
	RootObject->TryGetStringField(TEXT("command"), Command);
	RootObject->TryGetStringField(TEXT("installCommand"), InstallCommand);
	RootObject->TryGetStringField(TEXT("documentationUrl"), DocumentationUrl);
	RootObject->TryGetStringField(TEXT("docsUrl"), DocumentationUrl);
	RootObject->TryGetStringField(TEXT("setupNotes"), ProfileSetupNotes);
	RootObject->TryGetNumberField(TEXT("priority"), Priority);

	const TArray<FString> Args = HyperAIStudio::Private::ReadStringArrayField(RootObject.ToSharedRef(), TEXT("args"));
	const TArray<FString> Domains = HyperAIStudio::Private::ReadStringArrayField(RootObject.ToSharedRef(), TEXT("domains"));
	const TArray<FString> TargetClients = HyperAIStudio::Private::ReadStringArrayField(RootObject.ToSharedRef(), TEXT("targetClients"));
	const TArray<FString> AgentInstructions = HyperAIStudio::Private::ReadStringArrayField(RootObject.ToSharedRef(), TEXT("agentInstructions"));

	// Historical ids are reserved migration identities. They may never be rebound
	// through the generic importer, even when a profile has been customized. The
	// pure recognizer supplies diagnostics but is not used as permission to write.
	FHyperAIStudioLegacyProfileInput LegacyInput;
	LegacyInput.ServerId = Id.IsEmpty() ? Name : Id;
	LegacyInput.Transport = Transport;
	LegacyInput.Command = Command;
	LegacyInput.Arguments = Args;
	LegacyInput.Url = Url;
	LegacyInput.bHasEnvironment = RootObject->HasField(TEXT("env"));

	static const TSet<FString> HistoricalProfileFields = {
		TEXT("schema"), TEXT("id"), TEXT("name"), TEXT("displayName"),
		TEXT("description"), TEXT("transport"), TEXT("url"), TEXT("command"),
		TEXT("args"), TEXT("env"), TEXT("installCommand"),
		TEXT("documentationUrl"), TEXT("docsUrl"), TEXT("setupNotes"),
		TEXT("priority"), TEXT("domains"), TEXT("targetClients"),
		TEXT("agentInstructions")
	};
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : RootObject->Values)
	{
		if (!HistoricalProfileFields.Contains(Field.Key))
		{
			LegacyInput.bHasUnrecognizedFields = true;
			break;
		}
	}

	const auto HasWrongFieldType = [&RootObject](const TCHAR* FieldName, const EJson ExpectedType)
	{
		const TSharedPtr<FJsonValue>* Value = RootObject->Values.Find(FieldName);
		return Value && (!Value->IsValid() || (*Value)->Type != ExpectedType);
	};
	LegacyInput.bStructurallyMalformed =
		HasWrongFieldType(TEXT("id"), EJson::String)
		|| HasWrongFieldType(TEXT("name"), EJson::String)
		|| HasWrongFieldType(TEXT("displayName"), EJson::String)
		|| HasWrongFieldType(TEXT("transport"), EJson::String)
		|| HasWrongFieldType(TEXT("url"), EJson::String)
		|| HasWrongFieldType(TEXT("command"), EJson::String)
		|| HasWrongFieldType(TEXT("args"), EJson::Array);
	if (!LegacyInput.bStructurallyMalformed)
	{
		const TArray<TSharedPtr<FJsonValue>>* RawArguments = nullptr;
		if (RootObject->TryGetArrayField(TEXT("args"), RawArguments) && RawArguments)
		{
			LegacyInput.bStructurallyMalformed = RawArguments->ContainsByPredicate([](const TSharedPtr<FJsonValue>& Value)
			{
				return !Value.IsValid() || Value->Type != EJson::String;
			});
		}
	}

	const FString NormalizedCandidateId = HyperAIStudio::Private::NormalizeServerId(Id, Name);
	const bool bUsesReservedLegacyId = NormalizedCandidateId == TEXT("hyper-ue-mcp")
		|| NormalizedCandidateId == TEXT("hyper-ue-mcp-full")
		|| NormalizedCandidateId == TEXT("hyper-knowledge-mcp");
	if (bUsesReservedLegacyId)
	{
		const FHyperAIStudioLegacyProfileMatch LegacyMatch =
			FHyperAIStudioLegacyProfileRecognizer::Recognize(LegacyInput);
		const FString Classification = LegacyMatch.Classification == EHyperAIStudioLegacyProfileClassification::ExactOwned
			? TEXT("an exact retired HyperAIStudio profile")
			: LegacyMatch.Classification == EHyperAIStudioLegacyProfileClassification::CustomizedReservedId
				? TEXT("a customized profile using a reserved retired id")
				: TEXT("a malformed profile using a reserved retired id");
		OutMessage = FText::FromString(FString::Printf(
			TEXT("Profile `%s` is %s and was not imported. HyperAIStudio preserves existing customized entries for migration review, but retired ids cannot be installed or rebound. Choose a new id for an independent community server."),
			*NormalizedCandidateId,
			*Classification));
		return false;
	}

	const FString NormalizedTransport = Transport.ToLower();
	const bool bCommandTransport = NormalizedTransport == TEXT("stdio") || NormalizedTransport == TEXT("command");
	const bool bHttpTransport = NormalizedTransport.IsEmpty()
		|| NormalizedTransport == TEXT("http")
		|| NormalizedTransport == TEXT("streamable-http")
		|| NormalizedTransport == TEXT("streamable_http")
		|| NormalizedTransport == TEXT("url");

	if (!bCommandTransport && !bHttpTransport)
	{
		OutMessage = FText::FromString(FString::Printf(TEXT("Unsupported MCP profile transport `%s`."), *Transport));
		return false;
	}

	if (Name.IsEmpty())
	{
		Name = Id.IsEmpty() ? TEXT("Imported MCP Server") : Id;
	}
	if (Id.IsEmpty())
	{
		Id = Name;
	}

	if (bCommandTransport && Command.IsEmpty())
	{
		OutMessage = FText::FromString(TEXT("Command/stdio MCP profile did not include a command."));
		return false;
	}
	if (bHttpTransport && Url.IsEmpty())
	{
		OutMessage = FText::FromString(TEXT("HTTP MCP profile did not include a url."));
		return false;
	}

	FString SetupNotes;
	const TArray<TSharedPtr<FJsonValue>>* EnvValues = nullptr;
	if (RootObject->TryGetArrayField(TEXT("env"), EnvValues) && EnvValues)
	{
		TArray<FString> EnvNames;
		for (const TSharedPtr<FJsonValue>& EnvValue : *EnvValues)
		{
			const TSharedPtr<FJsonObject> EnvObject = EnvValue.IsValid() ? EnvValue->AsObject() : nullptr;
			if (!EnvObject.IsValid())
			{
				continue;
			}

			FString EnvName;
			if (EnvObject->TryGetStringField(TEXT("name"), EnvName) && !EnvName.IsEmpty())
			{
				EnvNames.Add(EnvName);
			}
		}
		if (EnvNames.Num() > 0)
		{
			SetupNotes = FString::Printf(TEXT("Profile declares environment variables: %s. HyperAIStudio did not import secret values."), *HyperAIStudio::Private::JoinReadable(EnvNames, TEXT("none")));
		}
	}
	if (!InstallCommand.IsEmpty())
	{
		SetupNotes += FString::Printf(TEXT("%sInstall command: %s."), SetupNotes.IsEmpty() ? TEXT("") : TEXT(" "), *InstallCommand);
	}
	if (!DocumentationUrl.IsEmpty())
	{
		SetupNotes += FString::Printf(TEXT("%sDocs: %s."), SetupNotes.IsEmpty() ? TEXT("") : TEXT(" "), *DocumentationUrl);
	}
	if (!ProfileSetupNotes.IsEmpty())
	{
		SetupNotes += FString::Printf(TEXT("%s%s"), SetupNotes.IsEmpty() ? TEXT("") : TEXT(" "), *ProfileSetupNotes);
	}

	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	if (!Settings)
	{
		OutMessage = FText::FromString(TEXT("HyperAIStudio settings are unavailable."));
		return false;
	}

	FHyperAIStudioMCPServerEntry Entry;
	Entry.Id = HyperAIStudio::Private::NormalizeServerId(Id, Name);
	Entry.DisplayName = Name;
	Entry.Description = Description;
	Entry.Transport = bCommandTransport ? EHyperAIStudioMCPTransport::Command : EHyperAIStudioMCPTransport::StreamableHttp;
	Entry.Url = Url;
	Entry.Command = Command;
	Entry.Arguments = Args;
	Entry.Priority = FMath::Clamp(Priority, 0, 10000);
	Entry.Domains = Domains;
	Entry.TargetClients = TargetClients;
	Entry.AgentInstructions = AgentInstructions;
	Entry.SetupNotes = SetupNotes;
	Entry.bEnabled = false;

	const int32 ExistingIndex = Settings->ExtraServers.IndexOfByPredicate([&Entry](const FHyperAIStudioMCPServerEntry& ExistingEntry)
	{
		return HyperAIStudio::Private::NormalizeServerId(ExistingEntry.Id, ExistingEntry.DisplayName) == Entry.Id;
	});
	if (Settings->ExtraServers.IsValidIndex(ExistingIndex))
	{
		Settings->ExtraServers[ExistingIndex] = Entry;
		Settings->SaveConfig();
		OutMessage = FText::FromString(FString::Printf(TEXT("Updated disabled MCP profile `%s`. Review it, then enable it when ready."), *Entry.DisplayName));
		return true;
	}

	Settings->ExtraServers.Add(Entry);
	Settings->SaveConfig();

	OutMessage = FText::FromString(FString::Printf(TEXT("Imported disabled MCP profile `%s`. Review it, then enable it when ready."), *Entry.DisplayName));
	return true;
}

void FHyperAIStudioService::TestExtraMCPServerAsync(int32 ServerIndex, TFunction<void(bool bOk, const FString& Message)> OnComplete) const
{
	const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
	if (!Settings || !Settings->ExtraServers.IsValidIndex(ServerIndex))
	{
		OnComplete(false, TEXT("That MCP server entry no longer exists."));
		return;
	}

	const FHyperAIStudioMCPServerEntry Entry = Settings->ExtraServers[ServerIndex];
	if (Entry.Transport == EHyperAIStudioMCPTransport::Command)
	{
		if (Entry.Command.IsEmpty())
		{
			OnComplete(false, FString::Printf(TEXT("%s has no command configured."), *Entry.DisplayName));
			return;
		}

		if (!IsExecutableOnPath(Entry.Command))
		{
			OnComplete(false, FString::Printf(TEXT("%s command `%s` was not found on PATH. Review setup notes/install command before enabling this server."), *Entry.DisplayName, *Entry.Command));
			return;
		}

		OnComplete(true, FString::Printf(TEXT("%s command `%s` is available on PATH. HyperAIStudio does not start stdio MCP servers; the external agent client will launch it from the generated config."), *Entry.DisplayName, *Entry.Command));
		return;
	}

	if (Entry.Url.IsEmpty())
	{
		OnComplete(false, FString::Printf(TEXT("%s has no URL configured."), *Entry.DisplayName));
		return;
	}

	ProbeToolsListAsync(Entry.Url, [Entry, OnComplete](bool bReachable, const FString& Message, int32 ToolCount)
	{
		if (bReachable)
		{
			OnComplete(true, FString::Printf(TEXT("%s tools/list reachable with %d advertised tools."), *Entry.DisplayName, ToolCount));
			return;
		}
		OnComplete(false, FString::Printf(TEXT("%s test failed: %s"), *Entry.DisplayName, *Message));
	});
}

FString FHyperAIStudioService::GetExampleMCPProfileJson() const
{
	return TEXT("{\n")
		TEXT("  \"schema\": \"hyperai.mcp.v1\",\n")
		TEXT("  \"id\": \"project-docs\",\n")
		TEXT("  \"name\": \"Project Docs Filesystem MCP\",\n")
		TEXT("  \"description\": \"Disabled template for exposing a reviewed project docs folder through the official filesystem MCP server.\",\n")
		TEXT("  \"transport\": \"stdio\",\n")
		TEXT("  \"command\": \"npx\",\n")
		TEXT("  \"args\": [\"-y\", \"@modelcontextprotocol/server-filesystem\", \".\"],\n")
		TEXT("  \"url\": \"\",\n")
		TEXT("  \"installCommand\": \"npm view @modelcontextprotocol/server-filesystem version\",\n")
		TEXT("  \"documentationUrl\": \"https://github.com/modelcontextprotocol/servers/tree/main/src/filesystem\",\n")
		TEXT("  \"setupNotes\": \"Disabled template. Before enabling, replace the final args entry with the smallest project docs/design-notes folder the agent should read. Do not expose more of the project than needed.\",\n")
		TEXT("  \"domains\": [\"docs\", \"project\"],\n")
		TEXT("  \"priority\": 60,\n")
		TEXT("  \"targetClients\": [\"codex\", \"claude\", \"cursor\", \"vscode\"],\n")
		TEXT("  \"agentInstructions\": [\n")
		TEXT("    \"Use this server only for reviewed project documentation and design notes.\",\n")
		TEXT("    \"Use unreal-mcp for live Unreal Editor actions.\"\n")
		TEXT("  ]\n")
		TEXT("}\n");
}

FString FHyperAIStudioService::GetProviderMCPProfileJson(const FString& ProviderId) const
{
	const FString NormalizedProviderId = HyperAIStudio::Private::NormalizeServerId(ProviderId, ProviderId);

	FString Id;
	FString Name;
	FString Description;
	FString Url;
	FString DocumentationUrl;
	FString EnvName;
	FString EnvDescription;
	FString SetupNotes;
	TArray<FString> Domains;
	int32 Priority = 120;

	if (NormalizedProviderId == TEXT("meshy"))
	{
		Id = TEXT("meshy-provider");
		Name = TEXT("Meshy Provider MCP");
		Description = TEXT("Optional 3D asset generation profile for a local Meshy-compatible MCP server.");
		Url = TEXT("http://127.0.0.1:9101/mcp");
		DocumentationUrl = TEXT("https://docs.meshy.ai/en/api/quick-start");
		EnvName = TEXT("MESHY_API_KEY");
		EnvDescription = TEXT("Meshy API key for your local provider MCP server.");
		SetupNotes = TEXT("Disabled template. Configure and run your chosen local Meshy MCP server, set MESHY_API_KEY outside shareable profile files, update the URL if needed, then enable it.");
		Domains = { TEXT("3d"), TEXT("assets"), TEXT("provider") };
	}
	else if (NormalizedProviderId == TEXT("tripo"))
	{
		Id = TEXT("tripo-provider");
		Name = TEXT("Tripo Provider MCP");
		Description = TEXT("Optional text/image-to-3D profile for a local Tripo-compatible MCP server.");
		Url = TEXT("http://127.0.0.1:9102/mcp");
		DocumentationUrl = TEXT("https://platform.tripo3d.ai/docs/quick-start");
		EnvName = TEXT("TRIPO_API_KEY");
		EnvDescription = TEXT("Tripo API key for your local provider MCP server.");
		SetupNotes = TEXT("Disabled template. Configure and run your chosen local Tripo MCP server, set TRIPO_API_KEY outside shareable profile files, update the URL if needed, then enable it.");
		Domains = { TEXT("3d"), TEXT("assets"), TEXT("provider") };
	}
	else if (NormalizedProviderId == TEXT("elevenlabs"))
	{
		Id = TEXT("elevenlabs-provider");
		Name = TEXT("ElevenLabs Provider MCP");
		Description = TEXT("Optional voice/audio profile for a local ElevenLabs-compatible MCP server.");
		Url = TEXT("http://127.0.0.1:9103/mcp");
		DocumentationUrl = TEXT("https://elevenlabs.io/docs/api-reference/introduction");
		EnvName = TEXT("ELEVENLABS_API_KEY");
		EnvDescription = TEXT("ElevenLabs API key for your local provider MCP server.");
		SetupNotes = TEXT("Disabled template. Configure and run your chosen local ElevenLabs MCP server, set ELEVENLABS_API_KEY outside shareable profile files, update the URL if needed, then enable it.");
		Domains = { TEXT("audio"), TEXT("voice"), TEXT("provider") };
	}
	else if (NormalizedProviderId == TEXT("custom"))
	{
		Id = TEXT("custom-provider");
		Name = TEXT("Custom Provider MCP");
		Description = TEXT("Optional template for a studio-specific local MCP provider.");
		Url = TEXT("http://127.0.0.1:9100/mcp");
		DocumentationUrl = TEXT("https://modelcontextprotocol.io/docs/getting-started/intro");
		EnvName = TEXT("CUSTOM_PROVIDER_API_KEY");
		EnvDescription = TEXT("Optional key for your local provider MCP server.");
		SetupNotes = TEXT("Disabled template. Replace the name, URL, domains, and environment variable with your studio provider server details before enabling it.");
		Domains = { TEXT("provider"), TEXT("custom") };
		Priority = 140;
	}
	else
	{
		return FString();
	}

	TSharedRef<FJsonObject> RootObject = MakeShared<FJsonObject>();
	RootObject->SetStringField(TEXT("schema"), TEXT("hyperai.mcp.v1"));
	RootObject->SetStringField(TEXT("id"), Id);
	RootObject->SetStringField(TEXT("name"), Name);
	RootObject->SetStringField(TEXT("description"), Description);
	RootObject->SetStringField(TEXT("transport"), TEXT("http"));
	RootObject->SetStringField(TEXT("url"), Url);
	RootObject->SetStringField(TEXT("documentationUrl"), DocumentationUrl);
	RootObject->SetStringField(TEXT("setupNotes"), SetupNotes);
	RootObject->SetNumberField(TEXT("priority"), Priority);

	TArray<TSharedPtr<FJsonValue>> DomainValues;
	for (const FString& Domain : Domains)
	{
		DomainValues.Add(MakeShared<FJsonValueString>(Domain));
	}
	RootObject->SetArrayField(TEXT("domains"), DomainValues);

	TArray<TSharedPtr<FJsonValue>> TargetValues;
	const TArray<FString> TargetClients = { TEXT("codex"), TEXT("claude"), TEXT("cursor"), TEXT("vscode"), TEXT("gemini") };
	for (const FString& TargetClient : TargetClients)
	{
		TargetValues.Add(MakeShared<FJsonValueString>(TargetClient));
	}
	RootObject->SetArrayField(TEXT("targetClients"), TargetValues);

	TArray<TSharedPtr<FJsonValue>> EnvValues;
	TSharedRef<FJsonObject> EnvObject = MakeShared<FJsonObject>();
	EnvObject->SetStringField(TEXT("name"), EnvName);
	EnvObject->SetBoolField(TEXT("required"), NormalizedProviderId != TEXT("custom"));
	EnvObject->SetBoolField(TEXT("secret"), true);
	EnvObject->SetStringField(TEXT("description"), EnvDescription);
	EnvValues.Add(MakeShared<FJsonValueObject>(EnvObject));
	RootObject->SetArrayField(TEXT("env"), EnvValues);

	TArray<TSharedPtr<FJsonValue>> InstructionValues;
	InstructionValues.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("Use %s only after the user explicitly asks for that provider workflow."), *Name)));
	InstructionValues.Add(MakeShared<FJsonValueString>(TEXT("Use unreal-mcp for live Unreal Editor actions, asset imports, and project inspection.")));
	RootObject->SetArrayField(TEXT("agentInstructions"), InstructionValues);

	FString Output;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	if (!FJsonSerializer::Serialize(RootObject, Writer))
	{
		return FString();
	}
	Output += LINE_TERMINATOR;
	return Output;
}

bool FHyperAIStudioService::OpenAgentsFile(FText& OutMessage) const
{
	const FString Path = FPaths::Combine(GetProjectRoot(), TEXT("AGENTS.md"));
	if (!FPaths::FileExists(Path))
	{
		OutMessage = FText::FromString(TEXT("AGENTS.md does not exist yet. Run Set Up HyperAIStudio first."));
		return false;
	}

	FPlatformProcess::LaunchFileInDefaultExternalApplication(*Path);
	OutMessage = FText::FromString(TEXT("Opened AGENTS.md."));
	return true;
}

bool FHyperAIStudioService::OpenClaudeFile(FText& OutMessage) const
{
	const FString Path = FPaths::Combine(GetProjectRoot(), TEXT("CLAUDE.md"));
	if (!FPaths::FileExists(Path))
	{
		OutMessage = FText::FromString(TEXT("CLAUDE.md does not exist yet. Enable Claude instructions and run Set Up HyperAIStudio."));
		return false;
	}

	FPlatformProcess::LaunchFileInDefaultExternalApplication(*Path);
	OutMessage = FText::FromString(TEXT("Opened CLAUDE.md."));
	return true;
}

bool FHyperAIStudioService::OpenIncludedDocs(FText& OutMessage) const
{
	const FString Url = GetDocumentationUrl();
	if (Url.IsEmpty())
	{
		OutMessage = FText::FromString(TEXT("HyperAIStudio documentation URL is not configured."));
		return false;
	}

	FPlatformProcess::LaunchURL(*Url, nullptr, nullptr);
	OutMessage = FText::FromString(TEXT("Opened HyperAIStudio documentation."));
	return true;
}

bool FHyperAIStudioService::OpenProjectFolder(FText& OutMessage) const
{
	const FString ProjectRoot = GetProjectRoot();
	if (!FPaths::DirectoryExists(ProjectRoot))
	{
		OutMessage = FText::FromString(TEXT("Project folder was not found."));
		return false;
	}

	FPlatformProcess::ExploreFolder(*ProjectRoot);
	OutMessage = FText::FromString(TEXT("Opened project folder."));
	return true;
}

bool FHyperAIStudioService::BuildTerminalLaunchPlanForPrompt(const FHyperAIStudioPromptPackage& Package, FHyperAIStudioTerminalLaunchPlan& OutPlan, FText& OutMessage) const
{
	OutPlan = FHyperAIStudioTerminalLaunchPlan();

	const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
	if (Settings && !Settings->bAllowTerminalLaunch)
	{
		OutMessage = FText::FromString(TEXT("Terminal launch is disabled in HyperAIStudio settings."));
		return false;
	}

	if (Package.PromptPath.IsEmpty() || !FPaths::FileExists(Package.PromptPath))
	{
		OutMessage = FText::FromString(TEXT("Copy or send a prompt before opening an external agent."));
		return false;
	}

	const FString ProjectRoot = GetProjectRoot();
	const FString SendScript = FPaths::Combine(ProjectRoot, TEXT(".hyperai"), TEXT("scripts"), TEXT("Send-HyperAIStudioPrompt.ps1"));
	FString ExistingSendScript;
	const bool bSendScriptReady = FFileHelper::LoadFileToString(ExistingSendScript, *SendScript)
		&& ExistingSendScript.Contains(TEXT("# HyperAIStudio handoff v2: wait for Unreal MCP, then start a fresh project-root agent process."));
	if (!bSendScriptReady)
	{
		FText FilesMessage;
		const FString Endpoint = Settings
			? GetEndpoint(Settings->UnrealMCPPort, Settings->UnrealMCPUrlPath)
			: FString();
		if (!Settings
			|| !GenerateHelperScripts(ProjectRoot, Endpoint, Settings, FilesMessage)
			|| !FFileHelper::LoadFileToString(ExistingSendScript, *SendScript)
			|| !ExistingSendScript.Contains(TEXT("# HyperAIStudio handoff v2: wait for Unreal MCP, then start a fresh project-root agent process.")))
		{
			OutMessage = FText::Format(NSLOCTEXT("HyperAIStudio", "SendScriptMissing", "The current Send-HyperAIStudioPrompt.ps1 could not be generated: {0}"), FilesMessage);
			return false;
		}
	}

	const FString ScriptArgs = FString::Printf(TEXT("-NoExit -ExecutionPolicy Bypass -File %s -Agent %s -PromptFile %s"),
		*HyperAIStudio::Private::QuoteCommandLineDouble(SendScript),
		*HyperAIStudio::Private::QuoteCommandLineDouble(Package.AgentName),
		*HyperAIStudio::Private::QuoteCommandLineDouble(Package.PromptPath));

#if PLATFORM_WINDOWS
	FString WindowsTerminalPath;
	if (FindExecutableOnPath(TEXT("wt"), WindowsTerminalPath))
	{
		const FString WtArgs = FString::Printf(TEXT("-d %s powershell %s"),
			*HyperAIStudio::Private::QuoteCommandLineDouble(ProjectRoot),
			*ScriptArgs);
		OutPlan.bSuccess = true;
		OutPlan.bUsesWindowsTerminal = true;
		OutPlan.RouteName = TEXT("Windows Terminal");
		OutPlan.ExecutablePath = WindowsTerminalPath;
		OutPlan.Arguments = WtArgs;
		OutPlan.WorkingDirectory = ProjectRoot;
		OutMessage = FText::FromString(TEXT("External agent launch plan ready."));
		return true;
	}
#endif

	FString PowerShellPath;
	FindExecutableOnPath(TEXT("powershell"), PowerShellPath);
	if (PowerShellPath.IsEmpty())
	{
		PowerShellPath = HyperAIStudio::Private::ResolvePowerShellPath();
	}

	OutPlan.bSuccess = true;
	OutPlan.bUsesPowerShell = true;
	OutPlan.RouteName = TEXT("PowerShell");
	OutPlan.ExecutablePath = PowerShellPath;
	OutPlan.Arguments = ScriptArgs;
	OutPlan.WorkingDirectory = ProjectRoot;
	OutMessage = FText::FromString(TEXT("External agent launch plan ready."));
	return true;
}

bool FHyperAIStudioService::OpenTerminalForPrompt(const FHyperAIStudioPromptPackage& Package, FText& OutMessage) const
{
	auto CopyPromptForManualUse = [this, &Package]()
	{
		const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
		if (Settings && !Settings->bAllowClipboardPromptCopy)
		{
			return false;
		}

		FString PromptToCopy = Package.PromptText;
		if (PromptToCopy.IsEmpty())
		{
			FFileHelper::LoadFileToString(PromptToCopy, *Package.PromptPath);
		}
		if (PromptToCopy.IsEmpty())
		{
			return false;
		}

		CopyTextToClipboard(PromptToCopy);
		return true;
	};

	auto MissingAgentHandled = [&CopyPromptForManualUse, &OutMessage](const FString& DisplayName)
	{
		const bool bCopied = CopyPromptForManualUse();
		OutMessage = FText::FromString(FString::Printf(
			TEXT("%s was not found in PATH or common user install folders. %s After installing it, click Verify; if it still shows Missing, restart Unreal and try again."),
			*DisplayName,
			bCopied ? TEXT("Prompt copied.") : TEXT("Prompt file is ready.")));
		return true;
	};

	if (Package.PromptPath.IsEmpty() || !FPaths::FileExists(Package.PromptPath))
	{
		OutMessage = FText::FromString(TEXT("Copy or send a prompt before opening an external agent."));
		return false;
	}

	const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
	if (Settings && !Settings->bAllowTerminalLaunch)
	{
		const bool bCopied = CopyPromptForManualUse();
		OutMessage = FText::FromString(bCopied
			? TEXT("External agent launch is disabled. Prompt copied; open your agent manually from the project root.")
			: TEXT("External agent launch is disabled. Prompt file is ready; open your agent manually from the project root."));
		return true;
	}

	const bool bCodexAgent = Package.AgentName.Contains(TEXT("Codex"), ESearchCase::IgnoreCase);
	const bool bTerminalCliAgent = bCodexAgent
		|| Package.AgentName.Contains(TEXT("Claude"), ESearchCase::IgnoreCase)
		|| Package.AgentName.Contains(TEXT("Gemini"), ESearchCase::IgnoreCase);
	if (bCodexAgent)
	{
		FString CodexPath;
		if (!FindExecutableOnPath(TEXT("codex"), CodexPath))
		{
			return MissingAgentHandled(TEXT("Codex CLI"));
		}

		const UHyperAIStudioSettings* LaunchSettings = GetDefault<UHyperAIStudioSettings>();
		if (!LaunchSettings)
		{
			OutMessage = FText::FromString(TEXT("HyperAIStudio settings are unavailable."));
			return false;
		}

		const FString ProjectRoot = GetProjectRoot();
		const FString CodexConfigPath = FPaths::Combine(ProjectRoot, TEXT(".codex"), TEXT("config.toml"));
		const FString Endpoint = GetEndpoint(LaunchSettings->UnrealMCPPort, LaunchSettings->UnrealMCPUrlPath);
		const FString ExpectedManagedConfig = HyperAIStudio::Private::BuildCodexTomlBlock(Endpoint, LaunchSettings);
		FString ExistingCodexConfig;
		bool bConfigReady = FFileHelper::LoadFileToString(ExistingCodexConfig, *CodexConfigPath)
			&& ExistingCodexConfig.Contains(ExpectedManagedConfig);
		if (!bConfigReady)
		{
			if (!HyperAIStudio::Private::UpsertManagedBlock(
				CodexConfigPath,
				ExpectedManagedConfig,
				HyperAIStudio::Private::ManagedTomlBegin,
				HyperAIStudio::Private::ManagedTomlEnd))
			{
				OutMessage = FText::FromString(TEXT("Codex project config could not be prepared without replacing user-authored settings."));
				return false;
			}

			ExistingCodexConfig.Reset();
			bConfigReady = FFileHelper::LoadFileToString(ExistingCodexConfig, *CodexConfigPath)
				&& ExistingCodexConfig.Contains(ExpectedManagedConfig);
		}
		if (!bConfigReady)
		{
			OutMessage = FText::FromString(TEXT("Codex project config is missing or does not match the configured Unreal MCP endpoint."));
			return false;
		}
	}

	auto TryOpenEditorAgent = [this, &Package, &CopyPromptForManualUse, &MissingAgentHandled, &OutMessage](const FString& AgentNeedle, const FString& ExecutableName, const FString& DisplayName)
	{
		if (!Package.AgentName.Contains(AgentNeedle, ESearchCase::IgnoreCase))
		{
			return false;
		}

		FString ExecutablePath;
		if (!FindExecutableOnPath(ExecutableName, ExecutablePath))
		{
			MissingAgentHandled(DisplayName);
			return true;
		}

		CopyPromptForManualUse();
		const FString ProjectRoot = GetProjectRoot();
		const FString Command = FString::Printf(TEXT("Set-Location -LiteralPath %s; & %s ."),
			*HyperAIStudio::Private::QuotePowerShellSingle(ProjectRoot),
			*HyperAIStudio::Private::QuotePowerShellSingle(ExecutablePath));
		const FString Arguments = FString::Printf(TEXT("-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -Command %s"),
			*HyperAIStudio::Private::QuoteCommandLineDouble(Command));

		uint32 ProcessId = 0;
		const FProcHandle Handle = FPlatformProcess::CreateProc(
			*HyperAIStudio::Private::ResolvePowerShellPath(),
			*Arguments,
			true,
			true,
			true,
			&ProcessId,
			0,
			*ProjectRoot,
			nullptr);
		if (!Handle.IsValid())
		{
			OutMessage = FText::FromString(FString::Printf(TEXT("Could not open %s for this project. Prompt copied; paste it into %s from the project root."), *DisplayName, *DisplayName));
			return true;
		}

		OutMessage = FText::FromString(FString::Printf(TEXT("Prompt copied. Opened %s for this project. Paste the prompt into %s if it did not appear automatically."), *DisplayName, *DisplayName));
		return true;
	};

	if (TryOpenEditorAgent(TEXT("Cursor"), TEXT("cursor"), TEXT("Cursor")))
	{
		return true;
	}
	if (Package.AgentName.Contains(TEXT("VS Code"), ESearchCase::IgnoreCase)
		|| Package.AgentName.Contains(TEXT("Copilot"), ESearchCase::IgnoreCase)
		|| Package.AgentName.Contains(TEXT("VSCode"), ESearchCase::IgnoreCase))
	{
		if (TryOpenEditorAgent(TEXT("VS"), TEXT("code"), TEXT("VS Code")))
		{
			return true;
		}
	}

	if (Package.AgentName.Contains(TEXT("Claude"), ESearchCase::IgnoreCase))
	{
		FString ClaudePath;
		if (!FindExecutableOnPath(TEXT("claude"), ClaudePath))
		{
			return MissingAgentHandled(TEXT("Claude Code CLI"));
		}
	}
	else if (Package.AgentName.Contains(TEXT("Gemini"), ESearchCase::IgnoreCase))
	{
		FString GeminiPath;
		if (!FindExecutableOnPath(TEXT("gemini"), GeminiPath))
		{
			return MissingAgentHandled(TEXT("Gemini CLI"));
		}
	}

	if (bTerminalCliAgent)
	{
		FText StartMessage;
		if (!StartUnrealMCP(StartMessage))
		{
			OutMessage = FText::Format(
				NSLOCTEXT("HyperAIStudio", "AgentLaunchMCPStartFailed", "Unreal MCP could not be started before launching the agent: {0}"),
				StartMessage);
			return false;
		}
	}

	FHyperAIStudioTerminalLaunchPlan LaunchPlan;
	if (!BuildTerminalLaunchPlanForPrompt(Package, LaunchPlan, OutMessage))
	{
		return false;
	}

	uint32 ProcessId = 0;
	const FProcHandle Handle = FPlatformProcess::CreateProc(*LaunchPlan.ExecutablePath, *LaunchPlan.Arguments, true, false, false, &ProcessId, 0, *LaunchPlan.WorkingDirectory, nullptr);
	if (!Handle.IsValid())
	{
		OutMessage = FText::FromString(TEXT("Could not open a terminal for the prompt."));
		return false;
	}

	OutMessage = FText::FromString(FString::Printf(TEXT("Opened %s with the HyperAIStudio handoff script. Prompt is copied; paste it into the agent if the terminal does not start it automatically."), *LaunchPlan.RouteName));
	return true;
}

bool FHyperAIStudioService::OpenUnrealTerminal(FText& OutMessage) const
{
	const TSharedPtr<IPlugin> TerminalPlugin = IPluginManager::Get().FindPlugin(TEXT("Terminal"));
	if (!TerminalPlugin.IsValid())
	{
		OutMessage = FText::FromString(TEXT("The Unreal Terminal plugin is not installed in this engine."));
		return false;
	}
	if (!TerminalPlugin->IsEnabled())
	{
		OutMessage = FText::FromString(TEXT("The Unreal Terminal plugin is disabled. Enable Terminal, restart the editor, or use Copy Startup Commands."));
		return false;
	}

	FModuleManager::Get().LoadModule(TEXT("Terminal"));
	const TSharedPtr<SDockTab> TerminalTab = FGlobalTabmanager::Get()->TryInvokeTab(FName(TEXT("Terminal")));
	if (!TerminalTab.IsValid())
	{
		OutMessage = FText::FromString(TEXT("Could not open the Unreal Terminal tab. Enable Terminal and restart the editor."));
		return false;
	}

	OutMessage = FText::FromString(TEXT("Opened Unreal Terminal."));
	return true;
}

void FHyperAIStudioService::CopyTextToClipboard(const FString& Text) const
{
	FPlatformApplicationMisc::ClipboardCopy(*Text);
}

bool FHyperAIStudioService::ConfigureUnrealMCPSettings(FText& OutMessage)
{
	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	UModelContextProtocolSettings* MCPSettings = GetMutableDefault<UModelContextProtocolSettings>();
	if (!Settings || !MCPSettings)
	{
		OutMessage = FText::FromString(TEXT("Required settings objects are unavailable."));
		return false;
	}
	const FString PreviousEndpoint = GetEndpoint(
		MCPSettings->ServerPortNumber,
		HyperAIStudio::Private::CleanUrlPath(MCPSettings->ServerUrlPath));

	Settings->UnrealMCPUrlPath = HyperAIStudio::Private::CleanUrlPath(Settings->UnrealMCPUrlPath);
	Settings->bAutoStartUnrealMCP = true;
	Settings->bEnableToolSearch = true;
	Settings->SaveConfig();

	MCPSettings->ServerPortNumber = Settings->UnrealMCPPort;
	MCPSettings->ServerUrlPath = Settings->UnrealMCPUrlPath;
	MCPSettings->bAutoStartServer = Settings->bAutoStartUnrealMCP;
	MCPSettings->bEnableToolSearch = Settings->bEnableToolSearch;
	MCPSettings->SaveConfig();
	FHyperAIStudioCapabilityInventoryClient::Invalidate(PreviousEndpoint);
	FHyperAIStudioCapabilityInventoryClient::Invalidate(GetEndpoint(
		Settings->UnrealMCPPort,
		Settings->UnrealMCPUrlPath));

	OutMessage = FText::FromString(TEXT("Epic Unreal MCP settings configured."));
	return true;
}

void BuildAgentInstructionBlocks(
	const FString& ProjectRoot,
	const FString& Endpoint,
	const UHyperAIStudioSettings* Settings,
	FString& OutAgentsBlock,
	FString& OutClaudeBlock)
{
	FString ExtraServersText;
	TArray<FHyperAIStudioMCPServerEntry> SortedServers = Settings->ExtraServers;
	SortedServers.Sort([](const FHyperAIStudioMCPServerEntry& A, const FHyperAIStudioMCPServerEntry& B)
	{
		return A.Priority < B.Priority;
	});

	for (const FHyperAIStudioMCPServerEntry& Entry : SortedServers)
	{
		if (!Entry.bEnabled || HyperAIStudio::Private::IsRetiredExactOwnedLegacy(Entry))
		{
			continue;
		}
		const FString ServerId = HyperAIStudio::Private::NormalizeServerId(Entry.Id, Entry.DisplayName);
		ExtraServersText += FString::Printf(TEXT("- `%s` priority %d: %s%s"),
			*ServerId,
			Entry.Priority,
			Entry.Transport == EHyperAIStudioMCPTransport::StreamableHttp ? TEXT("HTTP ") : TEXT("command "),
			Entry.Transport == EHyperAIStudioMCPTransport::StreamableHttp ? *Entry.Url : *Entry.Command);
		if (!Entry.Description.IsEmpty())
		{
			ExtraServersText += FString::Printf(TEXT(" - %s"), *Entry.Description);
		}
		if (Entry.Domains.Num() > 0)
		{
			ExtraServersText += FString::Printf(TEXT(" Domains: %s."), *HyperAIStudio::Private::JoinReadable(Entry.Domains, TEXT("")));
		}
		if (Entry.AgentInstructions.Num() > 0)
		{
			ExtraServersText += FString::Printf(TEXT(" Instructions: %s."), *HyperAIStudio::Private::JoinReadable(Entry.AgentInstructions, TEXT("")));
		}
		ExtraServersText += LINE_TERMINATOR;
	}

	if (ExtraServersText.IsEmpty())
	{
		ExtraServersText = TEXT("- No extra MCP servers are enabled yet.") LINE_TERMINATOR;
	}

	OutAgentsBlock =
		FString::Printf(TEXT("# HyperAIStudio Agent Instructions%s%s"), LINE_TERMINATOR, LINE_TERMINATOR) +
		TEXT("## Project") LINE_TERMINATOR LINE_TERMINATOR +
		FString::Printf(TEXT("- Root: `%s`%s- Native MCP: `unreal-mcp` at `%s`%s%s"),
			*ProjectRoot, LINE_TERMINATOR, *Endpoint, LINE_TERMINATOR, LINE_TERMINATOR) +
		TEXT("- Use native MCP for Unreal editor actions and the generated Start/Wait helpers for lifecycle and readiness checks; do not hand-roll MCP protocol calls.") LINE_TERMINATOR +
		TEXT("- Follow the user's requested intent first. If asked to start, open, or reconnect the project, run `.hyperai/scripts/Start-HyperAIStudioEditor.ps1`, wait for it to report MCP ready, and then continue.") LINE_TERMINATOR +
		TEXT("- Missing native MCP tools do not prove that the agent was opened from the wrong directory. Start and verify Unreal first.") LINE_TERMINATOR +
		TEXT("- After readiness, use native `unreal-mcp` tools when the current agent client exposes them. If it still does not, continue work that does not require Unreal tools; only ask for an MCP reload or project-root reopen when the requested task actually requires those tools.") LINE_TERMINATOR +
		TEXT("- Never ask for a new agent session merely to start Unreal Editor, and never claim that editor startup failed only because native tools are absent from the current session.") LINE_TERMINATOR +
		TEXT("- Use normal filesystem tools for source files.") LINE_TERMINATOR +
		TEXT("- Do not overlap Unreal MCP calls.") LINE_TERMINATOR LINE_TERMINATOR +
		TEXT("## Fast Paths") LINE_TERMINATOR LINE_TERMINATOR +
		TEXT("- Current level: `call_tool` -> `editor_toolset.toolsets.scene.SceneTools` / `get_current_level` / `{}`.") LINE_TERMINATOR +
		TEXT("- List actors: `call_tool` -> `editor_toolset.toolsets.scene.SceneTools` / `find_actors` / `{\"name\":\"\",\"tag\":\"\",\"collision_channels\":[]}`.") LINE_TERMINATOR LINE_TERMINATOR +
		TEXT("## Reconnect") LINE_TERMINATOR LINE_TERMINATOR +
		TEXT("- If Unreal is closed or disconnected, run `.hyperai/scripts/Start-HyperAIStudioEditor.ps1`; this also waits for MCP readiness.") LINE_TERMINATOR +
		TEXT("- Use `.hyperai/scripts/Wait-HyperAIStudioMCP.ps1 -TimeoutSeconds 90` for a readiness-only retry, then continue the requested task.") LINE_TERMINATOR LINE_TERMINATOR +
		TEXT("## Context Packs") LINE_TERMINATOR LINE_TERMINATOR +
		TEXT("- Only read `.hyperai/context/` files when the prompt explicitly references them.") LINE_TERMINATOR LINE_TERMINATOR +
		TEXT("## Domain Guides") LINE_TERMINATOR LINE_TERMINATOR +
		TEXT("- Before designing or changing an Animation Blueprint, read `.hyperai/docs/HyperAIStudio-AnimationCookbook.md`.") LINE_TERMINATOR LINE_TERMINATOR +
		TEXT("## Shared Project Workspace") LINE_TERMINATOR LINE_TERMINATOR +
		TEXT("- Treat `.hyperai/` as the shared, Git-trackable workspace for useful agent context, prompts, plans, research, decisions, logs, and task-local scratch files.") LINE_TERMINATOR +
		TEXT("- Do not create generic agent-work folders such as `Planning`, `Temp`, `Research`, `Notes`, or `Scripts` in the project root.") LINE_TERMINATOR +
		TEXT("- For substantial work, create `.hyperai/work/YYYY-MM-DD-<clear-context-name>/` using a clear lowercase kebab-case context name. Add only the useful parts: `README.md`, `plan.md`, `context/`, `prompts/`, `research/`, `decisions/`, `log/`, or `temp/`.") LINE_TERMINATOR +
		TEXT("- Name architecture and product decisions `decisions/YYYY-MM-DD-<system>-<decision>.md`. After meaningful implementation or design work, add a short dated entry under `log/` with what changed, decisions made, relevant files, and follow-up work.") LINE_TERMINATOR +
		TEXT("- Do not create history for simple questions or read-only checks. Consult `.hyperai/INDEX.md` only when prior project context is relevant; do not scan all history every turn.") LINE_TERMINATOR +
		TEXT("- Keep real source, content, config, and other project-owned files in their normal project locations. Never move an existing project folder merely because its name resembles an agent-work folder.") LINE_TERMINATOR +
		TEXT("- Do not store secrets, credentials, large generated binaries, or unfiltered terminal dumps in `.hyperai/`.") LINE_TERMINATOR LINE_TERMINATOR +
		TEXT("## Extra MCP Servers") LINE_TERMINATOR LINE_TERMINATOR +
		ExtraServersText +
		LINE_TERMINATOR +
		TEXT("Use extra MCP servers only when relevant to the task.") LINE_TERMINATOR;

	OutClaudeBlock =
		FString(TEXT("@AGENTS.md") LINE_TERMINATOR LINE_TERMINATOR) +
		FString(TEXT("# Claude Code HyperAIStudio Notes") LINE_TERMINATOR LINE_TERMINATOR) +
		FString::Printf(TEXT("- Project root: `%s`%s- Native MCP: `unreal-mcp` at `%s`%s- Follow the user's requested intent first. If asked to start, open, or reconnect the project, run `.hyperai/scripts/Start-HyperAIStudioEditor.ps1`, wait for MCP readiness, and continue.%s- Missing native MCP tools do not prove that the working directory is wrong. Start and verify Unreal first.%s- Use native `unreal-mcp` tools when this client exposes them. Only request an MCP reload or project-root reopen if the endpoint is ready and the requested task actually requires unavailable Unreal tools.%s- Never ask for a new agent session merely to start Unreal Editor.%s- Use generated Start/Wait helpers for lifecycle and readiness checks; do not hand-roll MCP protocol calls.%s- Fast paths: current level = `SceneTools.get_current_level`; actors = `SceneTools.find_actors` via `call_tool`.%s- Only read `.hyperai/context/` files when the prompt explicitly references them.%s- Do not overlap Unreal MCP calls."),
			*ProjectRoot, LINE_TERMINATOR,
			*Endpoint, LINE_TERMINATOR,
			LINE_TERMINATOR,
			LINE_TERMINATOR,
			LINE_TERMINATOR,
			LINE_TERMINATOR,
			LINE_TERMINATOR,
			LINE_TERMINATOR,
			LINE_TERMINATOR);
}

bool FHyperAIStudioService::GenerateManagedProjectFilesTransactional(
	const FString& ProjectRoot,
	const FString& Endpoint,
	UHyperAIStudioSettings* Settings,
	FText& OutMessage)
{
	using namespace HyperAIStudio::Private;
	static constexpr int32 ManagedMigrationVersion = 2;
	static constexpr int32 MaxReconcileEntriesPerClient = 2048;
	static constexpr bool bAllowUnownedDesiredReplacement = false;
	static const FString PendingCapabilityHash =
		TEXT("sha256:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd");
	static const FString PendingRecoveryValidatorHash =
		TEXT("sha256:eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee");
	static_assert(!bAllowUnownedDesiredReplacement,
		"Unowned desired-id replacement must remain fail-closed even with transaction-bound recovery evidence.");

	struct FJsonTargetMutation
	{
		FString ClientId;
		FString RelativePath;
		FString Container;
		FString FilePath;
		TArray<uint8> OriginalBytes;
		TArray<uint8> DesiredBytes;
		TArray<FHyperAIStudioManagedConfigLedgerEntry> DesiredLedgerEntries;
		bool bExisted = false;
	};

	if (!Settings)
	{
		OutMessage = FText::FromString(TEXT("HyperAIStudio settings are unavailable."));
		return false;
	}
	FString LockError;
	FHyperAIStudioOperationJournal ProjectMutationLock(ProjectRoot);
	if (!ProjectMutationLock.Load(LockError))
	{
		OutMessage = FText::FromString(FString::Printf(
			TEXT("Could not acquire the project managed-config lock: %s"),
			*LockError));
		return false;
	}
	const FString HyperDir = FPaths::Combine(ProjectRoot, TEXT(".hyperai"));
	const FString StateDir = FPaths::Combine(HyperDir, TEXT("state"));
	const FString RuntimeDir = FPaths::Combine(HyperDir, TEXT("runtime"));
	const FString LedgerPath = FPaths::Combine(StateDir, TEXT("managed-config-state.json"));
	const FString PendingLedgerPath = FPaths::Combine(RuntimeDir, TEXT("managed-config-pending.json"));
	const FString SettingsEvidencePath = FPaths::Combine(RuntimeDir, TEXT("managed-config-settings-pending.json"));
	struct FLegacyWorkspaceFile
	{
		FString LegacyPath;
		FString OrganizedPath;
	};
	const TArray<FLegacyWorkspaceFile> LegacyWorkspaceFiles = {
		{ FPaths::Combine(HyperDir, TEXT("managed-config-state.json")), LedgerPath },
		{ FPaths::Combine(HyperDir, TEXT("managed-config-pending.json")), PendingLedgerPath },
		{ FPaths::Combine(HyperDir, TEXT("managed-config-settings-pending.json")), SettingsEvidencePath }
	};
	for (const FLegacyWorkspaceFile& LegacyFile : LegacyWorkspaceFiles)
	{
		FString MigrationReason;
		if (!MigrateLegacyManagedFile(LegacyFile.LegacyPath, LegacyFile.OrganizedPath, MigrationReason))
		{
			OutMessage = FText::FromString(FString::Printf(
				TEXT("Could not organize legacy HyperAIStudio state '%s' (%s). Existing files were preserved."),
				*LegacyFile.LegacyPath,
				*MigrationReason));
			return false;
		}
	}
	const FString SettingsConfigPath = GEditorPerProjectIni;
	FString SettingsRecoveryReason;
	if (!ReconcileSettingsEvidenceAtEntry(
		Settings,
		SettingsConfigPath,
		SettingsEvidencePath,
		SettingsRecoveryReason))
	{
		OutMessage = FText::FromString(FString::Printf(
			TEXT("Managed settings recovery stopped fail-closed (%s)."),
			*SettingsRecoveryReason));
		return false;
	}

	TArray<FString> Warnings;
	TArray<int32> ExactLegacySettingsIndices;
	TMap<FString, TMap<FString, TArray<FString>>> HistoricalHashesByClientAndId;
	for (int32 Index = 0; Index < Settings->ExtraServers.Num(); ++Index)
	{
		const FHyperAIStudioMCPServerEntry& Entry = Settings->ExtraServers[Index];
		const FHyperAIStudioLegacyProfileMatch Match = RecognizeLegacySettingsEntry(Entry);
		const FString NormalizedId = NormalizeServerId(Entry.Id, Entry.DisplayName);
		if (FHyperAIStudioLegacyProfileRecognizer::IsExactOwnedRetiredProfile(Match))
		{
			const FString HistoricalJson = SerializeJsonObjectToString(MakeCommandServerObject(Entry));
			const FHyperAIStudioCanonicalJsonResult HistoricalHash =
				FHyperAIStudioCanonicalJson::HashObject(HistoricalJson);
			if (!HistoricalHash.bSuccess)
			{
				OutMessage = FText::FromString(TEXT("Could not hash an exact historical MCP profile."));
				return false;
			}
			ExactLegacySettingsIndices.Add(Index);
			// Historical JSON generation ignored TargetClients, so cleanup ownership
			// evidence must cover every legacy JSON coordinate. Codex is reconciled
			// separately through its owned TOML managed block.
			for (const FString& HistoricalClientId :
				FHyperAIStudioLegacyProfileRecognizer::GetHistoricalJsonClientIdsForCleanup(Match))
			{
				HistoricalHashesByClientAndId
					.FindOrAdd(HistoricalClientId)
					.FindOrAdd(NormalizedId)
					.AddUnique(HistoricalHash.ObjectHash);
			}
			continue;
		}

		if (NormalizedId == TEXT("hyper-ue-mcp")
			|| NormalizedId == TEXT("hyper-ue-mcp-full")
			|| NormalizedId == TEXT("hyper-knowledge-mcp"))
		{
			OutMessage = FText::FromString(FString::Printf(
				TEXT("Preserved customized legacy server '%s'. Resolve its reserved id before migration can continue."),
				*NormalizedId));
			return false;
		}
	}
	for (const FHyperAIStudioMCPServerEntry& Entry : Settings->ExtraServers)
	{
		for (FString Target : Entry.TargetClients)
		{
			Target.TrimStartAndEndInline();
			Target = Target.ToLower();
			if (!IsSupportedTargetClient(Target))
			{
				Warnings.AddUnique(FString::Printf(
					TEXT("Unknown TargetClients id '%s' was preserved and ignored."),
					*Target));
			}
		}
	}

	struct FJsonClientSpec
	{
		const TCHAR* ClientId;
		const TCHAR* RelativePath;
		const TCHAR* Container;
		bool bEnabled = false;
		bool bGemini = false;
	};
	const TArray<FJsonClientSpec> ClientSpecs = {
		{ TEXT("claude"), TEXT(".mcp.json"), TEXT("mcpServers"), Settings->bEnableClaudeAgent, false },
		{ TEXT("cursor"), TEXT(".cursor/mcp.json"), TEXT("mcpServers"), Settings->bEnableCursorAgent, false },
		{ TEXT("vscode"), TEXT(".vscode/mcp.json"), TEXT("servers"), Settings->bEnableVSCodeAgent, false },
		{ TEXT("gemini"), TEXT(".gemini/settings.json"), TEXT("mcpServers"), Settings->bEnableGeminiAgent, true }
	};
	struct FClientEnablement
	{
		const TCHAR* ClientId;
		bool bEnabled = false;
	};
	const TArray<FClientEnablement> AllClientEnablements = {
		{ TEXT("codex"), Settings->bEnableCodexAgent },
		{ TEXT("claude"), Settings->bEnableClaudeAgent },
		{ TEXT("cursor"), Settings->bEnableCursorAgent },
		{ TEXT("vscode"), Settings->bEnableVSCodeAgent },
		{ TEXT("gemini"), Settings->bEnableGeminiAgent }
	};
	TArray<FString> DesiredCodexServerIds;
	for (const FClientEnablement& Client : AllClientEnablements)
	{
		if (!Client.bEnabled)
		{
			continue;
		}
		TSet<FString> DesiredIds;
		DesiredIds.Add(TEXT("unreal-mcp"));
		for (const FHyperAIStudioMCPServerEntry& Entry : Settings->ExtraServers)
		{
			if (!Entry.bEnabled
				|| IsRetiredExactOwnedLegacy(Entry)
				|| !TargetsClient(Entry, Client.ClientId))
			{
				continue;
			}
			const FString ServerId = NormalizeServerId(Entry.Id, Entry.DisplayName);
			const bool bValidPayload =
				(Entry.Transport == EHyperAIStudioMCPTransport::StreamableHttp && !Entry.Url.IsEmpty())
				|| (Entry.Transport == EHyperAIStudioMCPTransport::Command && !Entry.Command.IsEmpty());
			if (!bValidPayload)
			{
				OutMessage = FText::FromString(FString::Printf(
					TEXT("Enabled MCP entry '%s' has no valid payload for client %s. No writes were made."),
					*ServerId,
					Client.ClientId));
				return false;
			}
			if (DesiredIds.Contains(ServerId))
			{
				OutMessage = FText::FromString(FString::Printf(
					TEXT("Duplicate or native-colliding MCP id '%s' for client %s. No writes were made."),
					*ServerId,
					Client.ClientId));
				return false;
			}
			DesiredIds.Add(ServerId);
		}
		if (FString(Client.ClientId) == TEXT("codex"))
		{
			DesiredCodexServerIds = DesiredIds.Array();
			DesiredCodexServerIds.Sort();
		}
	}

	struct FMarkerTarget
	{
		FString Path;
		FString Begin;
		FString End;
		FString Content;
		bool bDesired = false;
		bool bExisted = false;
		bool bChanged = false;
		TArray<uint8> OriginalBytes;
		TArray<uint8> DesiredBytes;
	};
	FString AgentsBlock;
	FString ClaudeBlock;
	BuildAgentInstructionBlocks(ProjectRoot, Endpoint, Settings, AgentsBlock, ClaudeBlock);
	TArray<FMarkerTarget> MarkerTargets = {
		{ FPaths::Combine(ProjectRoot, TEXT("AGENTS.md")), ManagedMarkdownBegin, ManagedMarkdownEnd, MoveTemp(AgentsBlock), true },
		{ FPaths::Combine(ProjectRoot, TEXT("CLAUDE.md")), ManagedMarkdownBegin, ManagedMarkdownEnd, MoveTemp(ClaudeBlock), Settings->bEnableClaudeAgent },
		{ FPaths::Combine(ProjectRoot, TEXT(".codex"), TEXT("config.toml")), ManagedTomlBegin, ManagedTomlEnd, BuildCodexTomlBlock(Endpoint, Settings), Settings->bEnableCodexAgent }
	};
	for (FMarkerTarget& Marker : MarkerTargets)
	{
		Marker.bExisted = FPaths::FileExists(Marker.Path);
		FString ReasonCode;
		if (!BuildManagedBlockBytes(
			Marker.Path,
			Marker.Content,
			Marker.Begin,
			Marker.End,
			Marker.bDesired,
			Marker.OriginalBytes,
			Marker.DesiredBytes,
			Marker.bChanged,
			ReasonCode))
		{
			OutMessage = FText::FromString(FString::Printf(
				TEXT("Preserved malformed managed file '%s' (%s). No migration writes were made."),
				*Marker.Path,
				*ReasonCode));
			return false;
		}
		if (Marker.bDesired
			&& FPaths::GetCleanFilename(Marker.Path) == TEXT("config.toml"))
		{
			FString ExistingToml;
			EManagedTextEncoding ExistingEncoding = EManagedTextEncoding::Utf8;
			FString DecodeReason;
			if (!DecodeManagedText(Marker.OriginalBytes, ExistingToml, ExistingEncoding, DecodeReason))
			{
				OutMessage = FText::FromString(FString::Printf(
					TEXT("Preserved Codex config because it could not be inspected safely (%s)."),
					*DecodeReason));
				return false;
			}
			const FHyperAIStudioCodexTomlCollisionInspection Collision =
				FHyperAIStudioCodexTomlCollisionInspector::Inspect(
					ExistingToml,
					Marker.Begin,
					Marker.End,
					DesiredCodexServerIds);
			if (!Collision.MayWriteManagedBlock())
			{
				const FString CollisionIds = Collision.CollidingServerIds.IsEmpty()
					? TEXT("none")
					: FString::Join(Collision.CollidingServerIds, TEXT(", "));
				OutMessage = FText::FromString(FString::Printf(
					TEXT("Preserved user-owned Codex MCP tables (reason=%s, collisions=%s). No migration writes were made."),
					*Collision.ReasonCode,
					*CollisionIds));
				return false;
			}
		}
		if (Marker.bExisted != FPaths::FileExists(Marker.Path))
		{
			OutMessage = FText::FromString(FString::Printf(
				TEXT("Managed file changed existence during preflight: %s."),
				*Marker.Path));
			return false;
		}
	}

	FHyperAIStudioManagedConfigOwnershipLedger NextLedger;
	TArray<uint8> ExistingLedgerBytes;
	const bool bLedgerExisted = FPaths::FileExists(LedgerPath);
	if (!ReadFileBytes(LedgerPath, ExistingLedgerBytes))
	{
		OutMessage = FText::FromString(TEXT("Could not read the managed-config ownership ledger."));
		return false;
	}
	if (bLedgerExisted != FPaths::FileExists(LedgerPath))
	{
		OutMessage = FText::FromString(TEXT("Ownership ledger changed existence during preflight."));
		return false;
	}
	if (bLedgerExisted)
	{
		FString LedgerText;
		EManagedTextEncoding LedgerEncoding = EManagedTextEncoding::Utf8;
		FString LedgerDecodeReason;
		if (!DecodeManagedText(ExistingLedgerBytes, LedgerText, LedgerEncoding, LedgerDecodeReason)
			|| !FHyperAIStudioManagedConfigLedgerCodec::Parse(LedgerText, NextLedger, LedgerDecodeReason))
		{
			OutMessage = FText::FromString(FString::Printf(
				TEXT("Preserved invalid managed-config ownership ledger (%s)."),
				*LedgerDecodeReason));
			return false;
		}
	}
	FHyperAIStudioManagedConfigPendingTransaction PendingEvidence;
	FString PendingOperationId;
	TOptional<EHyperAIStudioOperationState> PendingBoundState;
	TArray<uint8> ExistingPendingLedgerBytes;
	const bool bPendingLedgerExisted = FPaths::FileExists(PendingLedgerPath);
	if (!ReadFileBytes(PendingLedgerPath, ExistingPendingLedgerBytes))
	{
		OutMessage = FText::FromString(TEXT("Could not read the managed-config pending ledger."));
		return false;
	}
	if (bPendingLedgerExisted != FPaths::FileExists(PendingLedgerPath))
	{
		OutMessage = FText::FromString(TEXT("Pending ownership ledger changed existence during preflight."));
		return false;
	}
	if (bPendingLedgerExisted)
	{
		FString PendingText;
		EManagedTextEncoding PendingEncoding = EManagedTextEncoding::Utf8;
		FString PendingReason;
		if (!DecodeManagedText(ExistingPendingLedgerBytes, PendingText, PendingEncoding, PendingReason)
			|| !FHyperAIStudioManagedConfigPendingCodec::Parse(PendingText, PendingEvidence, PendingReason))
		{
			OutMessage = FText::FromString(FString::Printf(
				TEXT("Preserved invalid managed-config transaction evidence (%s)."),
				*PendingReason));
			return false;
		}
		if (!PendingEvidence.IsIdle())
		{
			PendingOperationId = TEXT("managed-config-") + PendingEvidence.TransactionId;
			const TOptional<FHyperAIStudioOperationRecord> BoundRecord =
				ProjectMutationLock.Find(PendingOperationId);
			const FString EvidencePlanHash =
				FHyperAIStudioManagedConfigPendingCodec::HashBytes(ExistingPendingLedgerBytes);
			if (!BoundRecord.IsSet()
				|| BoundRecord->PlanHash != EvidencePlanHash
				|| BoundRecord->CapabilityHash != PendingCapabilityHash
				|| (BoundRecord->State != EHyperAIStudioOperationState::OutcomeUnknown
					&& BoundRecord->State != EHyperAIStudioOperationState::CommitStarted
					&& BoundRecord->State != EHyperAIStudioOperationState::Completed))
			{
				OutMessage = FText::FromString(TEXT("Pending managed-config evidence has no exact durable precommit journal binding. Setup stopped fail-closed."));
				return false;
			}
			PendingBoundState = BoundRecord->State;
		}
	}

	auto RemoveLedgerEntry = [&NextLedger](
		const FString& ClientId,
		const FString& RelativePath,
		const FString& Container,
		const FString& EntryId)
	{
		for (FHyperAIStudioManagedConfigLedgerClient& Client : NextLedger.Clients)
		{
			if (Client.ClientId == ClientId
				&& Client.RelativePath == RelativePath
				&& Client.Container == Container)
			{
				Client.Entries.RemoveAll([&EntryId](const FHyperAIStudioManagedConfigLedgerEntry& Entry)
				{
					return Entry.Id == EntryId;
				});
			}
		}
	};

	auto SetLedgerEntry = [&NextLedger](
		const FString& ClientId,
		const FString& RelativePath,
		const FString& Container,
		const FString& EntryId,
		const FString& ObjectHash)
	{
		FHyperAIStudioManagedConfigLedgerClient* Client = NextLedger.Clients.FindByPredicate(
			[&](const FHyperAIStudioManagedConfigLedgerClient& Candidate)
			{
				return Candidate.ClientId == ClientId
					&& Candidate.RelativePath == RelativePath
					&& Candidate.Container == Container;
			});
		if (!Client)
		{
			FHyperAIStudioManagedConfigLedgerClient NewClient;
			NewClient.ClientId = ClientId;
			NewClient.RelativePath = RelativePath;
			NewClient.Container = Container;
			Client = &NextLedger.Clients.Add_GetRef(MoveTemp(NewClient));
		}

		FHyperAIStudioManagedConfigLedgerEntry* Entry = Client->Entries.FindByPredicate(
			[&EntryId](const FHyperAIStudioManagedConfigLedgerEntry& Candidate)
			{
				return Candidate.Id == EntryId;
			});
		if (!Entry)
		{
			Entry = &Client->Entries.AddDefaulted_GetRef();
			Entry->Id = EntryId;
		}
		Entry->ObjectHash = ObjectHash;
		Entry->WriterVersion = 1;
	};
	auto ReplaceLedgerCoordinate = [&NextLedger](
		const FString& ClientId,
		const FString& RelativePath,
		const FString& Container,
		const TArray<FHyperAIStudioManagedConfigLedgerEntry>& Entries)
	{
		NextLedger.Clients.RemoveAll([&](const FHyperAIStudioManagedConfigLedgerClient& Client)
		{
			return Client.ClientId == ClientId
				&& Client.RelativePath == RelativePath
				&& Client.Container == Container;
		});
		if (!Entries.IsEmpty())
		{
			FHyperAIStudioManagedConfigLedgerClient Client;
			Client.ClientId = ClientId;
			Client.RelativePath = RelativePath;
			Client.Container = Container;
			Client.Entries = Entries;
			NextLedger.Clients.Add(MoveTemp(Client));
		}
	};
	TArray<FJsonTargetMutation> JsonMutations;
	int32 MatchedPendingTargets = 0;
	for (const FJsonClientSpec& Spec : ClientSpecs)
	{
		const FString ClientId(Spec.ClientId);
		const FString RelativePath(Spec.RelativePath);
		const FString Container(Spec.Container);
		const FString FilePath = FPaths::Combine(ProjectRoot, RelativePath);
		const bool bFileExists = FPaths::FileExists(FilePath);
		TArray<uint8> OriginalBytes;
		if (!ReadFileBytes(FilePath, OriginalBytes))
		{
			OutMessage = FText::FromString(FString::Printf(TEXT("Could not read %s."), *RelativePath));
			return false;
		}
		if (bFileExists != FPaths::FileExists(FilePath))
		{
			OutMessage = FText::FromString(FString::Printf(
				TEXT("Config changed existence during preflight: %s."),
				*RelativePath));
			return false;
		}

		FString ExistingText;
		EManagedTextEncoding ExistingEncoding = EManagedTextEncoding::Utf8;
		FString DecodeReason;
		if (bFileExists && !DecodeManagedText(OriginalBytes, ExistingText, ExistingEncoding, DecodeReason))
		{
			OutMessage = FText::FromString(FString::Printf(
				TEXT("Preserved unreadable JSON config %s (%s)."),
				*RelativePath,
				*DecodeReason));
			return false;
		}

		const bool bEmptyFile = bFileExists && ExistingText.TrimStartAndEnd().IsEmpty();
		TSharedPtr<FJsonObject> RootObject = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> ServersObject;
		bool bContainerExists = false;
		if (bFileExists && !bEmptyFile)
		{
			const FHyperAIStudioJsonEntryInspection Structural =
				FHyperAIStudioJsonEntryInspector::InspectExistingFile(
					ExistingText,
					Container,
					TEXT("__hyperai_preflight_sentinel__"));
			if (Structural.State == EHyperAIStudioJsonEntryState::InvalidJson
				|| Structural.State == EHyperAIStudioJsonEntryState::RootNotObject
				|| Structural.State == EHyperAIStudioJsonEntryState::ContainerNotObject)
			{
				OutMessage = FText::FromString(FString::Printf(
					TEXT("Preserved structurally invalid JSON config %s (%s)."),
					*RelativePath,
					*Structural.ReasonCode));
				return false;
			}

			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ExistingText);
			if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
			{
				OutMessage = FText::FromString(FString::Printf(TEXT("Could not materialize %s."), *RelativePath));
				return false;
			}
			const TSharedPtr<FJsonObject>* ExistingServers = nullptr;
			if (RootObject->TryGetObjectField(Container, ExistingServers)
				&& ExistingServers
				&& ExistingServers->IsValid())
			{
				ServersObject = *ExistingServers;
				bContainerExists = true;
			}
		}

		TMap<FString, TSharedPtr<FJsonObject>> DesiredObjects;
		TMap<FString, FString> DesiredHashes;
		if (Spec.bEnabled)
		{
			DesiredObjects.Add(TEXT("unreal-mcp"), MakeHttpServerObject(Endpoint, Spec.bGemini));
			for (const FHyperAIStudioMCPServerEntry& Entry : Settings->ExtraServers)
			{
				if (!Entry.bEnabled
					|| IsRetiredExactOwnedLegacy(Entry)
					|| !TargetsClient(Entry, ClientId, &Warnings))
				{
					continue;
				}

				const FString ServerId = NormalizeServerId(Entry.Id, Entry.DisplayName);
				if (DesiredObjects.Contains(ServerId))
				{
					OutMessage = FText::FromString(FString::Printf(
						TEXT("Duplicate desired MCP id '%s' for client %s was preserved; no writes were made."),
						*ServerId,
						*ClientId));
					return false;
				}

				if (Entry.Transport == EHyperAIStudioMCPTransport::StreamableHttp && !Entry.Url.IsEmpty())
				{
					DesiredObjects.Add(ServerId, MakeHttpServerObject(Entry.Url, Spec.bGemini));
				}
				else if (Entry.Transport == EHyperAIStudioMCPTransport::Command && !Entry.Command.IsEmpty())
				{
					DesiredObjects.Add(ServerId, MakeCommandServerObject(Entry));
				}
				else
				{
					OutMessage = FText::FromString(FString::Printf(
						TEXT("Enabled MCP entry '%s' has no valid transport payload."),
						*ServerId));
					return false;
				}
			}
		}

		for (const TPair<FString, TSharedPtr<FJsonObject>>& Desired : DesiredObjects)
		{
			const FHyperAIStudioCanonicalJsonResult Hash = FHyperAIStudioCanonicalJson::HashObject(
				SerializeJsonObjectToString(Desired.Value.ToSharedRef()));
			if (!Hash.bSuccess)
			{
				OutMessage = FText::FromString(TEXT("Could not hash a desired MCP object."));
				return false;
			}
			DesiredHashes.Add(Desired.Key, Hash.ObjectHash);
		}
		TArray<FHyperAIStudioManagedConfigLedgerEntry> DesiredLedgerEntries;
		DesiredLedgerEntries.Reserve(DesiredHashes.Num());
		for (const TPair<FString, FString>& DesiredHash : DesiredHashes)
		{
			DesiredLedgerEntries.Add({
				DesiredHash.Key,
				DesiredHash.Value,
				FHyperAIStudioManagedConfigLedgerCodec::CurrentWriterVersion });
		}
		DesiredLedgerEntries.Sort([](
			const FHyperAIStudioManagedConfigLedgerEntry& Left,
			const FHyperAIStudioManagedConfigLedgerEntry& Right)
		{
			return Left.Id.Compare(Right.Id, ESearchCase::CaseSensitive) < 0;
		});

		const FHyperAIStudioManagedConfigPendingTarget* RecoveryTarget =
			PendingEvidence.IsIdle()
				? nullptr
				: PendingEvidence.FindTarget(ClientId, RelativePath, Container);
		EHyperAIStudioTransactionEvidenceMatch RecoveryMatch =
			EHyperAIStudioTransactionEvidenceMatch::Conflict;
		if (RecoveryTarget)
		{
			++MatchedPendingTargets;
			bool bDesiredLedgerMatches = RecoveryTarget->DesiredLedgerEntries.Num() == DesiredLedgerEntries.Num();
			for (int32 Index = 0; bDesiredLedgerMatches && Index < DesiredLedgerEntries.Num(); ++Index)
			{
				bDesiredLedgerMatches = RecoveryTarget->DesiredLedgerEntries[Index].Id == DesiredLedgerEntries[Index].Id
					&& RecoveryTarget->DesiredLedgerEntries[Index].ObjectHash == DesiredLedgerEntries[Index].ObjectHash
					&& RecoveryTarget->DesiredLedgerEntries[Index].WriterVersion == DesiredLedgerEntries[Index].WriterVersion;
			}
			if (!bDesiredLedgerMatches)
			{
				OutMessage = FText::FromString(FString::Printf(
					TEXT("Pending transaction %s no longer matches the desired ledger for %s. Setup stopped fail-closed."),
					*PendingEvidence.TransactionId,
					*RelativePath));
				return false;
			}
			const FString CurrentFileHash = bFileExists
				? FHyperAIStudioManagedConfigPendingCodec::HashBytes(OriginalBytes)
				: FString();
			RecoveryMatch = FHyperAIStudioManagedConfigPendingCodec::Classify(
				*RecoveryTarget,
				bFileExists,
				CurrentFileHash);
			if (RecoveryMatch == EHyperAIStudioTransactionEvidenceMatch::Conflict)
			{
				OutMessage = FText::FromString(FString::Printf(
					TEXT("Pending transaction %s found a third state for %s. User content was preserved and setup stopped fail-closed."),
					*PendingEvidence.TransactionId,
					*RelativePath));
				return false;
			}
			if (PendingBoundState.IsSet()
				&& PendingBoundState.GetValue() == EHyperAIStudioOperationState::Completed
				&& RecoveryMatch != EHyperAIStudioTransactionEvidenceMatch::ExactDesired)
			{
				OutMessage = FText::FromString(FString::Printf(
					TEXT("Completed transaction %s no longer has its exact desired target state for %s. User content was preserved and setup stopped fail-closed."),
					*PendingEvidence.TransactionId,
					*RelativePath));
				return false;
			}
			if (RecoveryMatch == EHyperAIStudioTransactionEvidenceMatch::ExactDesired)
			{
				ReplaceLedgerCoordinate(ClientId, RelativePath, Container, DesiredLedgerEntries);
			}
		}

		TSet<FString> CandidateIds;
		for (const TPair<FString, TSharedPtr<FJsonObject>>& Desired : DesiredObjects)
		{
			CandidateIds.Add(Desired.Key);
		}
		if (const FHyperAIStudioManagedConfigLedgerClient* LedgerClient = NextLedger.Clients.FindByPredicate(
			[&](const FHyperAIStudioManagedConfigLedgerClient& Candidate)
			{
				return Candidate.ClientId == ClientId
					&& Candidate.RelativePath == RelativePath
					&& Candidate.Container == Container;
			}))
		{
			for (const FHyperAIStudioManagedConfigLedgerEntry& Entry : LedgerClient->Entries)
			{
				CandidateIds.Add(Entry.Id);
			}
		}
		const TMap<FString, TArray<FString>>* ClientHistoricalHashes =
			HistoricalHashesByClientAndId.Find(ClientId);
		if (ClientHistoricalHashes)
		{
			for (const TPair<FString, TArray<FString>>& Historical : *ClientHistoricalHashes)
			{
				CandidateIds.Add(Historical.Key);
			}
		}
		if (CandidateIds.Num() > MaxReconcileEntriesPerClient)
		{
			OutMessage = FText::FromString(FString::Printf(TEXT("Too many managed entries in %s."), *RelativePath));
			return false;
		}

		bool bTargetChanged = false;
		for (const FString& EntryId : CandidateIds)
		{
			FHyperAIStudioManagedEntryReconcileInput Input;
			Input.bClientEnabledForGeneration = Spec.bEnabled;
			Input.bDesiredEntry = DesiredObjects.Contains(EntryId);
			Input.bAllowReplaceUnownedDesired = bAllowUnownedDesiredReplacement;
			if (const FString* DesiredHash = DesiredHashes.Find(EntryId))
			{
				Input.DesiredObjectHash = *DesiredHash;
			}

			if (!bFileExists)
			{
				Input.ExistingState = EHyperAIStudioJsonEntryState::MissingFile;
			}
			else if (bEmptyFile)
			{
				Input.ExistingState = EHyperAIStudioJsonEntryState::EmptyFile;
			}
			else if (!bContainerExists)
			{
				Input.ExistingState = EHyperAIStudioJsonEntryState::ContainerMissing;
			}
			else if (!ServersObject->HasField(EntryId))
			{
				Input.ExistingState = EHyperAIStudioJsonEntryState::EntryMissing;
			}
			else
			{
				const TSharedPtr<FJsonObject>* ExistingObject = nullptr;
				if (!ServersObject->TryGetObjectField(EntryId, ExistingObject)
					|| !ExistingObject
					|| !ExistingObject->IsValid())
				{
					Input.ExistingState = EHyperAIStudioJsonEntryState::EntryNotObject;
				}
				else
				{
					const FHyperAIStudioCanonicalJsonResult ExistingHash =
						FHyperAIStudioCanonicalJson::HashObject(
							SerializeJsonObjectToString(ExistingObject->ToSharedRef()));
					if (!ExistingHash.bSuccess)
					{
						OutMessage = FText::FromString(FString::Printf(TEXT("Could not hash %s/%s."), *RelativePath, *EntryId));
						return false;
					}
					Input.ExistingState = EHyperAIStudioJsonEntryState::EntryObject;
					Input.ExistingObjectHash = ExistingHash.ObjectHash;
				}
			}

			if (const FHyperAIStudioManagedConfigLedgerEntry* LedgerEntry = NextLedger.FindEntry(
				ClientId,
				RelativePath,
				Container,
				EntryId))
			{
				Input.LedgerObjectHash = LedgerEntry->ObjectHash;
			}
			if (const TArray<FString>* HistoricalHashes = ClientHistoricalHashes
				? ClientHistoricalHashes->Find(EntryId)
				: nullptr)
			{
				Input.bExactHistoricalSettingsOwnership = !HistoricalHashes->IsEmpty();
				if (!HistoricalHashes->IsEmpty())
				{
					Input.HistoricalObjectHash = HistoricalHashes->Contains(Input.ExistingObjectHash)
						? Input.ExistingObjectHash
						: (*HistoricalHashes)[0];
				}
			}

			const FHyperAIStudioManagedEntryReconcileResult Reconcile =
				FHyperAIStudioManagedEntryReconciler::Plan(Input);
			if (Reconcile.Decision == EHyperAIStudioManagedEntryDecision::Conflict)
			{
				OutMessage = FText::FromString(FString::Printf(
					TEXT("Preserved conflicting MCP entry %s/%s/%s (%s). No migration writes were made."),
					*RelativePath,
					*Container,
					*EntryId,
					*Reconcile.ReasonCode));
				return false;
			}

			if (Reconcile.Decision == EHyperAIStudioManagedEntryDecision::Write)
			{
				if (!ServersObject.IsValid())
				{
					ServersObject = MakeShared<FJsonObject>();
					RootObject->SetObjectField(Container, ServersObject);
					bContainerExists = true;
				}
				ServersObject->SetObjectField(EntryId, DesiredObjects.FindChecked(EntryId));
				SetLedgerEntry(ClientId, RelativePath, Container, EntryId, DesiredHashes.FindChecked(EntryId));
				bTargetChanged = true;
			}
			else if (Reconcile.Decision == EHyperAIStudioManagedEntryDecision::Remove)
			{
				if (ServersObject.IsValid())
				{
					ServersObject->RemoveField(EntryId);
					bTargetChanged = true;
				}
				RemoveLedgerEntry(ClientId, RelativePath, Container, EntryId);
			}
			else if (Reconcile.bDropLedgerRecordAfterSuccess)
			{
				RemoveLedgerEntry(ClientId, RelativePath, Container, EntryId);
			}
		}

		if (bTargetChanged)
		{
			FString Output;
			const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
			if (!FJsonSerializer::Serialize(RootObject.ToSharedRef(), Writer))
			{
				OutMessage = FText::FromString(FString::Printf(TEXT("Could not serialize %s."), *RelativePath));
				return false;
			}
			Output += LINE_TERMINATOR;
			TArray<uint8> DesiredBytes;
			EncodeManagedTextFragment(Output, EManagedTextEncoding::Utf8, DesiredBytes);
			if (RecoveryTarget
				&& RecoveryMatch == EHyperAIStudioTransactionEvidenceMatch::ExactOriginal
				&& (!RecoveryTarget->bDesiredExists
					|| RecoveryTarget->DesiredFileHash
						!= FHyperAIStudioManagedConfigPendingCodec::HashBytes(DesiredBytes)))
			{
				OutMessage = FText::FromString(FString::Printf(
					TEXT("Pending transaction %s no longer produces its bound desired state for %s. Setup stopped fail-closed."),
					*PendingEvidence.TransactionId,
					*RelativePath));
				return false;
			}
			FJsonTargetMutation Mutation;
			Mutation.ClientId = ClientId;
			Mutation.RelativePath = RelativePath;
			Mutation.Container = Container;
			Mutation.FilePath = FilePath;
			Mutation.OriginalBytes = MoveTemp(OriginalBytes);
			Mutation.DesiredBytes = MoveTemp(DesiredBytes);
			Mutation.DesiredLedgerEntries = MoveTemp(DesiredLedgerEntries);
			Mutation.bExisted = bFileExists;
			JsonMutations.Add(MoveTemp(Mutation));
		}
		else if (RecoveryTarget
			&& RecoveryMatch == EHyperAIStudioTransactionEvidenceMatch::ExactOriginal)
		{
			OutMessage = FText::FromString(FString::Printf(
				TEXT("Pending transaction %s expected a retry mutation for %s, but the current plan is a no-op. Setup stopped fail-closed."),
				*PendingEvidence.TransactionId,
				*RelativePath));
			return false;
		}
	}
	if (!PendingEvidence.IsIdle() && MatchedPendingTargets != PendingEvidence.Targets.Num())
	{
		OutMessage = FText::FromString(TEXT("Pending managed-config evidence contains an unprocessed target. Setup stopped fail-closed."));
		return false;
	}

	NextLedger.Clients.RemoveAll([](const FHyperAIStudioManagedConfigLedgerClient& Client)
	{
		return Client.Entries.IsEmpty();
	});
	FString NextLedgerJson;
	FString LedgerReason;
	if (!FHyperAIStudioManagedConfigLedgerCodec::Serialize(NextLedger, NextLedgerJson, LedgerReason))
	{
		OutMessage = FText::FromString(FString::Printf(TEXT("Could not serialize ownership ledger (%s)."), *LedgerReason));
		return false;
	}
	NextLedgerJson += LINE_TERMINATOR;
	TArray<uint8> NextLedgerBytes;
	EncodeManagedTextFragment(NextLedgerJson, EManagedTextEncoding::Utf8, NextLedgerBytes);
	const bool bLedgerChanged = ExistingLedgerBytes.Num() != NextLedgerBytes.Num()
		|| (NextLedgerBytes.Num() > 0
			&& FMemory::Memcmp(ExistingLedgerBytes.GetData(), NextLedgerBytes.GetData(), NextLedgerBytes.Num()) != 0);
	FHyperAIStudioManagedConfigPendingTransaction PreparedPendingEvidence = PendingEvidence;
	TArray<uint8> NextPendingLedgerBytes;
	bool bPendingLedgerWriteNeeded = false;
	if (PendingEvidence.IsIdle() && !JsonMutations.IsEmpty())
	{
		PreparedPendingEvidence.TransactionId = FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
		PreparedPendingEvidence.Phase = TEXT("targets_prepared");
		PreparedPendingEvidence.Targets.Reset();
		for (const FJsonTargetMutation& Mutation : JsonMutations)
		{
			FHyperAIStudioManagedConfigPendingTarget Target;
			Target.ClientId = Mutation.ClientId;
			Target.RelativePath = Mutation.RelativePath;
			Target.Container = Mutation.Container;
			Target.bOriginalExists = Mutation.bExisted;
			Target.OriginalFileHash = Mutation.bExisted
				? FHyperAIStudioManagedConfigPendingCodec::HashBytes(Mutation.OriginalBytes)
				: FString();
			Target.bDesiredExists = true;
			Target.DesiredFileHash = FHyperAIStudioManagedConfigPendingCodec::HashBytes(Mutation.DesiredBytes);
			Target.DesiredLedgerEntries = Mutation.DesiredLedgerEntries;
			PreparedPendingEvidence.Targets.Add(MoveTemp(Target));
		}
		FString NextPendingJson;
		FString PendingReason;
		if (!FHyperAIStudioManagedConfigPendingCodec::Serialize(
			PreparedPendingEvidence,
			NextPendingJson,
			PendingReason))
		{
			OutMessage = FText::FromString(FString::Printf(
				TEXT("Could not serialize pending ownership evidence (%s)."),
				*PendingReason));
			return false;
		}
		NextPendingJson += LINE_TERMINATOR;
		EncodeManagedTextFragment(NextPendingJson, EManagedTextEncoding::Utf8, NextPendingLedgerBytes);
		bPendingLedgerWriteNeeded = NextPendingLedgerBytes != ExistingPendingLedgerBytes;
	}
	else if (!PendingEvidence.IsIdle())
	{
		NextPendingLedgerBytes = ExistingPendingLedgerBytes;
	}
	FHyperAIStudioManagedConfigPendingTransaction EmptyPendingEvidence;
	FString EmptyPendingJson;
	FString EmptyPendingReason;
	if (!FHyperAIStudioManagedConfigPendingCodec::Serialize(
		EmptyPendingEvidence,
		EmptyPendingJson,
		EmptyPendingReason))
	{
		OutMessage = FText::FromString(FString::Printf(
			TEXT("Could not serialize cleared pending ownership evidence (%s)."),
			*EmptyPendingReason));
		return false;
	}
	EmptyPendingJson += LINE_TERMINATOR;
	TArray<uint8> EmptyPendingLedgerBytes;
	EncodeManagedTextFragment(EmptyPendingJson, EManagedTextEncoding::Utf8, EmptyPendingLedgerBytes);

	const bool bSelectionFlagsWillChange =
		(Settings->bEnableCodexAgent && !Settings->bGenerateCodexConfig)
		|| (Settings->bEnableClaudeAgent && (!Settings->bGenerateClaudeConfig || !Settings->bGenerateClaudeMarkdown))
		|| (Settings->bEnableCursorAgent && !Settings->bGenerateCursorConfig)
		|| (Settings->bEnableVSCodeAgent && !Settings->bGenerateVSCodeConfig)
		|| (Settings->bEnableGeminiAgent && !Settings->bGenerateGeminiConfig);
	const bool bSettingsWillChange = !ExactLegacySettingsIndices.IsEmpty()
		|| bSelectionFlagsWillChange
		|| Settings->NativeToolArchitectureMigrationVersion < ManagedMigrationVersion;
	TArray<uint8> OriginalSettingsBytes;
	bool bSettingsConfigExisted = false;
	if (bSettingsWillChange)
	{
		// GEditorPerProjectIni is shared by many settings classes. Persist any dirty
		// in-process values before taking the byte snapshot that our atomic replace owns.
		if (!GConfig
			|| SettingsConfigPath.IsEmpty()
			|| !GConfig->Flush(false, SettingsConfigPath))
		{
			OutMessage = FText::FromString(TEXT("Could not flush shared editor settings before managed-config preflight."));
			return false;
		}
		bSettingsConfigExisted = FPaths::FileExists(SettingsConfigPath);
		if (!ReadFileBytes(SettingsConfigPath, OriginalSettingsBytes))
		{
			OutMessage = FText::FromString(TEXT("Could not preflight HyperAIStudio settings."));
			return false;
		}
	}

	for (const FMarkerTarget& Marker : MarkerTargets)
	{
		TArray<uint8> CurrentBytes;
		if (Marker.bChanged
			&& (FPaths::FileExists(Marker.Path) != Marker.bExisted
				|| !ReadFileBytes(Marker.Path, CurrentBytes)
				|| CurrentBytes != Marker.OriginalBytes))
		{
			OutMessage = FText::FromString(FString::Printf(
				TEXT("Managed file changed after preflight: %s. No config writes were made."),
				*Marker.Path));
			return false;
		}
	}
	for (const FJsonTargetMutation& Mutation : JsonMutations)
	{
		TArray<uint8> CurrentBytes;
		if (FPaths::FileExists(Mutation.FilePath) != Mutation.bExisted
			|| !ReadFileBytes(Mutation.FilePath, CurrentBytes)
			|| CurrentBytes != Mutation.OriginalBytes)
		{
			OutMessage = FText::FromString(FString::Printf(
				TEXT("Config changed after preflight: %s. No config writes were made."),
				*Mutation.RelativePath));
			return false;
		}
	}
	TArray<uint8> CurrentLedgerBytes;
	TArray<uint8> CurrentPendingBytes;
	TArray<uint8> CurrentSettingsBytes;
	if (FPaths::FileExists(LedgerPath) != bLedgerExisted
		|| !ReadFileBytes(LedgerPath, CurrentLedgerBytes)
		|| CurrentLedgerBytes != ExistingLedgerBytes
		|| FPaths::FileExists(PendingLedgerPath) != bPendingLedgerExisted
		|| !ReadFileBytes(PendingLedgerPath, CurrentPendingBytes)
		|| CurrentPendingBytes != ExistingPendingLedgerBytes
		|| (bSettingsWillChange
			&& (FPaths::FileExists(SettingsConfigPath) != bSettingsConfigExisted
				|| !ReadFileBytes(SettingsConfigPath, CurrentSettingsBytes)
				|| CurrentSettingsBytes != OriginalSettingsBytes)))
	{
		OutMessage = FText::FromString(TEXT("Managed-config metadata or settings changed after preflight. No config writes were made."));
		return false;
	}

	if (bPendingLedgerWriteNeeded)
	{
		PendingOperationId = TEXT("managed-config-") + PreparedPendingEvidence.TransactionId;
		FHyperAIStudioOperationRecord OperationRecord;
		FString JournalError;
		const EHyperAIStudioOperationBeginResult BeginResult = ProjectMutationLock.BeginOperation(
			PendingOperationId,
			FHyperAIStudioManagedConfigPendingCodec::HashBytes(NextPendingLedgerBytes),
			PendingCapabilityHash,
			OperationRecord,
			JournalError);
		if (BeginResult != EHyperAIStudioOperationBeginResult::Created
			|| !ProjectMutationLock.Transition(
				PendingOperationId,
				EHyperAIStudioOperationState::Queued,
				EHyperAIStudioOperationState::Running,
				EHyperAIStudioRollbackState::NotNeeded,
				false,
				false,
				TEXT("managed_config_validated"),
				OperationRecord,
				JournalError)
			|| !ProjectMutationLock.Transition(
				PendingOperationId,
				EHyperAIStudioOperationState::Running,
				EHyperAIStudioOperationState::CommitStarted,
				EHyperAIStudioRollbackState::NotNeeded,
				false,
				false,
				TEXT("managed_config_commit_started"),
				OperationRecord,
				JournalError))
		{
			OutMessage = FText::FromString(FString::Printf(
				TEXT("Could not bind pending managed-config evidence to the durable operation journal (%s)."),
				*JournalError));
			return false;
		}
		if (!WriteBytesAtomically(
			PendingLedgerPath,
			NextPendingLedgerBytes,
			&ExistingPendingLedgerBytes,
			bPendingLedgerExisted))
		{
			OutMessage = FText::FromString(TEXT("Could not persist transaction-bound pending evidence. No target writes were made."));
			return false;
		}
	}
	for (const FMarkerTarget& Marker : MarkerTargets)
	{
		if (Marker.bChanged
			&& !WriteBytesAtomically(
				Marker.Path,
				Marker.DesiredBytes,
				&Marker.OriginalBytes,
				Marker.bExisted))
		{
			OutMessage = FText::FromString(FString::Printf(
				TEXT("Could not atomically write %s. Migration remains pending."),
				*Marker.Path));
			return false;
		}
	}

	for (const FJsonTargetMutation& Mutation : JsonMutations)
	{
		if (!WriteBytesAtomically(
			Mutation.FilePath,
			Mutation.DesiredBytes,
			&Mutation.OriginalBytes,
			Mutation.bExisted))
		{
			OutMessage = FText::FromString(FString::Printf(TEXT("Could not atomically write %s."), *Mutation.RelativePath));
			return false;
		}
	}
	if (!PreparedPendingEvidence.IsIdle())
	{
		for (const FHyperAIStudioManagedConfigPendingTarget& Target : PreparedPendingEvidence.Targets)
		{
			const FString TargetPath = FPaths::Combine(ProjectRoot, Target.RelativePath);
			const bool bTargetExists = FPaths::FileExists(TargetPath);
			TArray<uint8> TargetBytes;
			if (!ReadFileBytes(TargetPath, TargetBytes)
				|| bTargetExists != FPaths::FileExists(TargetPath)
				|| FHyperAIStudioManagedConfigPendingCodec::Classify(
					Target,
					bTargetExists,
					bTargetExists
						? FHyperAIStudioManagedConfigPendingCodec::HashBytes(TargetBytes)
						: FString()) != EHyperAIStudioTransactionEvidenceMatch::ExactDesired)
			{
				OutMessage = FText::FromString(FString::Printf(
					TEXT("Managed target %s did not retain the exact transaction-bound desired bytes. Ownership ledger was not committed."),
					*Target.RelativePath));
				return false;
			}
		}
	}

	const bool bPendingExistsAfterPreparation = bPendingLedgerWriteNeeded || bPendingLedgerExisted;
	const TArray<uint8>& PendingBytesAfterPreparation = bPendingLedgerWriteNeeded
		? NextPendingLedgerBytes
		: ExistingPendingLedgerBytes;
	if (bLedgerChanged)
	{
		if (!WriteBytesAtomically(
			LedgerPath,
			NextLedgerBytes,
			&ExistingLedgerBytes,
			bLedgerExisted))
		{
			OutMessage = FText::FromString(TEXT("Could not atomically commit the ownership ledger. Migration remains pending."));
			return false;
		}
	}

	if (!PreparedPendingEvidence.IsIdle())
	{
		const TOptional<FHyperAIStudioOperationRecord> BoundRecord =
			ProjectMutationLock.Find(PendingOperationId);
		FHyperAIStudioOperationRecord FinalRecord;
		FString JournalError;
		bool bJournalFinalized = false;
		auto BuildTrustedManagedConfigEvidence = [&ProjectMutationLock, &NextLedgerBytes](
			const FHyperAIStudioOperationRecord& Record)
		{
			const FDateTime Now = FDateTime::UtcNow();
			const int64 IssuedUtcMs = Now.ToUnixTimestamp() * 1000 + Now.GetMillisecond();
			FHyperAIStudioOperationEvidence Evidence;
			Evidence.Version = FHyperAIStudioOperationEvidence::CurrentVersion;
			Evidence.CanonicalProjectId = ProjectMutationLock.GetCanonicalProjectId();
			Evidence.OperationId = Record.OperationId;
			Evidence.PlanHash = Record.PlanHash;
			Evidence.CapabilityHash = Record.CapabilityHash;
			// The sealed pending-ledger plan is the complete declared managed-config effect contract.
			Evidence.EffectFingerprint = Record.PlanHash;
			Evidence.ActionNonce = FString(TEXT("managed-"))
				+ FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
			Evidence.ValidatorId = TEXT("hyperai.managed_config_exact_readback");
			Evidence.ApprovedValidatorFingerprint = PendingRecoveryValidatorHash;
			Evidence.PostconditionHash = FHyperAIStudioManagedConfigPendingCodec::HashBytes(NextLedgerBytes);
			Evidence.IssuedUtcMs = IssuedUtcMs;
			Evidence.ExpiresUtcMs = IssuedUtcMs + 60000;

			FString ReceiptMaterial;
			auto AppendReceiptToken = [&ReceiptMaterial](const FString& Value)
			{
				ReceiptMaterial += FString::FromInt(Value.Len()) + TEXT(":") + Value + TEXT("|");
			};
			AppendReceiptToken(TEXT("hyperai.managed-config-receipt.v1"));
			AppendReceiptToken(Evidence.CanonicalProjectId);
			AppendReceiptToken(Evidence.OperationId);
			AppendReceiptToken(Evidence.PlanHash);
			AppendReceiptToken(Evidence.CapabilityHash);
			AppendReceiptToken(Evidence.EffectFingerprint);
			AppendReceiptToken(Evidence.ActionNonce);
			AppendReceiptToken(Evidence.ValidatorId);
			AppendReceiptToken(Evidence.ApprovedValidatorFingerprint);
			AppendReceiptToken(Evidence.PostconditionHash);
			AppendReceiptToken(FString::Printf(TEXT("%lld"), static_cast<long long>(Evidence.IssuedUtcMs)));
			AppendReceiptToken(FString::Printf(TEXT("%lld"), static_cast<long long>(Evidence.ExpiresUtcMs)));
			const FTCHARToUTF8 ReceiptUtf8(*ReceiptMaterial);
			TArray<uint8> ReceiptBytes;
			ReceiptBytes.Append(
				reinterpret_cast<const uint8*>(ReceiptUtf8.Get()),
				ReceiptUtf8.Length());
			Evidence.ReceiptFingerprint = FHyperAIStudioManagedConfigPendingCodec::HashBytes(ReceiptBytes);
			return Evidence;
		};
		if (BoundRecord.IsSet()
			&& BoundRecord->State == EHyperAIStudioOperationState::CommitStarted)
		{
			const FHyperAIStudioOperationEvidence Evidence = BuildTrustedManagedConfigEvidence(*BoundRecord);
			bJournalFinalized = ProjectMutationLock.Transition(
				PendingOperationId,
				EHyperAIStudioOperationState::CommitStarted,
				EHyperAIStudioOperationState::Completed,
				EHyperAIStudioRollbackState::NotNeeded,
				true,
				false,
				TEXT("managed_config_committed"),
				Evidence,
				FinalRecord,
				JournalError);
		}
		else if (BoundRecord.IsSet()
			&& BoundRecord->State == EHyperAIStudioOperationState::OutcomeUnknown)
		{
			const FHyperAIStudioOperationEvidence Evidence = BuildTrustedManagedConfigEvidence(*BoundRecord);
			bJournalFinalized = ProjectMutationLock.ResolveUnknownWithEvidence(
				PendingOperationId,
				EHyperAIStudioUnknownResolution::Completed,
				Evidence,
				EHyperAIStudioRollbackState::Unknown,
				TEXT("managed_config_recovered"),
				FinalRecord,
				JournalError);
		}
		else if (BoundRecord.IsSet()
			&& BoundRecord->State == EHyperAIStudioOperationState::Completed)
		{
			bJournalFinalized = true;
		}
		if (!bJournalFinalized)
		{
			OutMessage = FText::FromString(FString::Printf(
				TEXT("Managed targets and ledger were committed, but durable transaction finalization failed (%s). Setup stopped fail-closed."),
				*JournalError));
			return false;
		}
	}

	// Only after both the ownership ledger and its durable operation record are
	// complete may transaction-bound evidence become idle. A crash before this
	// point leaves exact old/new hashes and journal binding for deterministic recovery.
	bool bPendingEvidenceIsEmpty = !bPendingExistsAfterPreparation
		|| PendingBytesAfterPreparation == EmptyPendingLedgerBytes;
	if (!bPendingEvidenceIsEmpty)
	{
		bPendingEvidenceIsEmpty = WriteBytesAtomically(
			PendingLedgerPath,
			EmptyPendingLedgerBytes,
			&PendingBytesAfterPreparation,
			true);
		if (!bPendingEvidenceIsEmpty)
		{
			OutMessage = FText::FromString(TEXT("Ownership ledger committed, but transaction evidence could not be set idle. Setup stopped fail-closed."));
			return false;
		}
	}

	FText StepMessage;
	if (!GenerateHelperScripts(ProjectRoot, Endpoint, Settings, StepMessage))
	{
		OutMessage = StepMessage;
		return false;
	}

	ExactLegacySettingsIndices.Sort(TGreater<int32>());
	const TArray<FHyperAIStudioMCPServerEntry> OriginalExtraServers = Settings->ExtraServers;
	const bool bOriginalGenerateCodexConfig = Settings->bGenerateCodexConfig;
	const bool bOriginalGenerateClaudeConfig = Settings->bGenerateClaudeConfig;
	const bool bOriginalGenerateClaudeMarkdown = Settings->bGenerateClaudeMarkdown;
	const bool bOriginalGenerateCursorConfig = Settings->bGenerateCursorConfig;
	const bool bOriginalGenerateVSCodeConfig = Settings->bGenerateVSCodeConfig;
	const bool bOriginalGenerateGeminiConfig = Settings->bGenerateGeminiConfig;
	for (const int32 Index : ExactLegacySettingsIndices)
	{
		if (Settings->ExtraServers.IsValidIndex(Index))
		{
			Settings->ExtraServers.RemoveAt(Index);
		}
	}
	const int32 PreviousMigrationVersion = Settings->NativeToolArchitectureMigrationVersion;
	Settings->EnsureEnabledBuiltInAgentConfigsSelected();
	FString SettingsSaveReason;
	bool bSettingsCommitOutcomeUnknown = false;
	TArray<uint8> LastCommittedSettingsBytes = OriginalSettingsBytes;
	bool bLastCommittedSettingsExisted = bSettingsConfigExisted;
	if ((!ExactLegacySettingsIndices.IsEmpty() || bSelectionFlagsWillChange)
		&& !SaveSettingsConfigAtomically(
			Settings,
			SettingsConfigPath,
			SettingsEvidencePath,
			OriginalSettingsBytes,
			bSettingsConfigExisted,
			LastCommittedSettingsBytes,
			SettingsSaveReason,
			bSettingsCommitOutcomeUnknown))
	{
		if (!bSettingsCommitOutcomeUnknown)
		{
			Settings->ExtraServers = OriginalExtraServers;
			Settings->bGenerateCodexConfig = bOriginalGenerateCodexConfig;
			Settings->bGenerateClaudeConfig = bOriginalGenerateClaudeConfig;
			Settings->bGenerateClaudeMarkdown = bOriginalGenerateClaudeMarkdown;
			Settings->bGenerateCursorConfig = bOriginalGenerateCursorConfig;
			Settings->bGenerateVSCodeConfig = bOriginalGenerateVSCodeConfig;
			Settings->bGenerateGeminiConfig = bOriginalGenerateGeminiConfig;
			Settings->NativeToolArchitectureMigrationVersion = PreviousMigrationVersion;
		}
		OutMessage = FText::FromString(bSettingsCommitOutcomeUnknown
			? FString::Printf(
				TEXT("Managed targets were reconciled, but the settings commit outcome is unknown (%s). Setup stopped fail-closed."),
				*SettingsSaveReason)
			: FString::Printf(
				TEXT("Managed targets were reconciled, but settings were not durably saved (%s). Migration remains pending."),
				*SettingsSaveReason));
		return false;
	}
	if (!ExactLegacySettingsIndices.IsEmpty() || bSelectionFlagsWillChange)
	{
		bLastCommittedSettingsExisted = true;
	}

	if (Settings->NativeToolArchitectureMigrationVersion < ManagedMigrationVersion)
	{
		Settings->NativeToolArchitectureMigrationVersion = ManagedMigrationVersion;
		TArray<uint8> VersionCommittedBytes;
		bSettingsCommitOutcomeUnknown = false;
		if (!SaveSettingsConfigAtomically(
			Settings,
			SettingsConfigPath,
			SettingsEvidencePath,
			LastCommittedSettingsBytes,
			bLastCommittedSettingsExisted,
			VersionCommittedBytes,
			SettingsSaveReason,
			bSettingsCommitOutcomeUnknown))
		{
			if (!bSettingsCommitOutcomeUnknown)
			{
				Settings->NativeToolArchitectureMigrationVersion = PreviousMigrationVersion;
			}
			OutMessage = FText::FromString(bSettingsCommitOutcomeUnknown
				? FString::Printf(
					TEXT("The final migration-version commit outcome is unknown (%s). Setup stopped fail-closed."),
					*SettingsSaveReason)
				: FString::Printf(
					TEXT("Could not durably advance the managed-config migration version (%s). Migration remains pending."),
					*SettingsSaveReason));
			return false;
		}
	}

	FString Summary = FString::Printf(
		TEXT("Managed configs reconciled; migration version %d committed last."),
		Settings->NativeToolArchitectureMigrationVersion);
	if (!ExactLegacySettingsIndices.IsEmpty())
	{
		Summary += FString::Printf(TEXT(" Removed %d exact retired settings profile(s)."), ExactLegacySettingsIndices.Num());
	}
	if (!Warnings.IsEmpty())
	{
		Summary += TEXT(" Warnings: ") + JoinReadable(Warnings, TEXT("none"));
	}
	OutMessage = FText::FromString(Summary);
	return true;
}

bool FHyperAIStudioService::GenerateHelperScripts(const FString& ProjectRoot, const FString& Endpoint, const UHyperAIStudioSettings* Settings, FText& OutMessage)
{
	const FString HyperDir = FPaths::Combine(ProjectRoot, TEXT(".hyperai"));
	const FString ScriptsDir = FPaths::Combine(HyperDir, TEXT("scripts"));
	const FString DocsDir = FPaths::Combine(HyperDir, TEXT("docs"));
	const FString StateDir = FPaths::Combine(HyperDir, TEXT("state"));
	const FString RuntimeDir = FPaths::Combine(HyperDir, TEXT("runtime"));
	if (!IFileManager::Get().MakeDirectory(*ScriptsDir, true)
		|| !IFileManager::Get().MakeDirectory(*DocsDir, true)
		|| !IFileManager::Get().MakeDirectory(*StateDir, true)
		|| !IFileManager::Get().MakeDirectory(*RuntimeDir, true))
	{
		OutMessage = FText::FromString(TEXT("Could not create the organized .hyperai workspace folders."));
		return false;
	}

	const FString EngineExe = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::EngineDir(), TEXT("Binaries"), TEXT("Win64"), TEXT("UnrealEditor.exe")));
	const FString ProjectFile = FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());
	const FString UrlPath = HyperAIStudio::Private::CleanUrlPath(Settings->UnrealMCPUrlPath);

	FString StartScript;
	StartScript += TEXT("param([switch]$ForceRestart, [int]$TimeoutSeconds = 90)") LINE_TERMINATOR;
	StartScript += TEXT("$ErrorActionPreference = 'Stop'") LINE_TERMINATOR;
	StartScript += FString::Printf(TEXT("$EngineExe = \"%s\"%s"), *EngineExe.Replace(TEXT("\\"), TEXT("\\\\")), LINE_TERMINATOR);
	StartScript += FString::Printf(TEXT("$ProjectFile = \"%s\"%s"), *ProjectFile.Replace(TEXT("\\"), TEXT("\\\\")), LINE_TERMINATOR);
	StartScript += FString::Printf(TEXT("$Port = %u%s"), Settings->UnrealMCPPort, LINE_TERMINATOR);
	StartScript += FString::Printf(TEXT("$UrlPath = \"%s\"%s"), *HyperAIStudio::Private::EscapeJsonString(UrlPath), LINE_TERMINATOR);
	StartScript += TEXT("if (!(Test-Path -LiteralPath $EngineExe)) { throw \"UnrealEditor.exe not found at $EngineExe\" }") LINE_TERMINATOR;
	StartScript += TEXT("if (!(Test-Path -LiteralPath $ProjectFile)) { throw \"Project file not found at $ProjectFile\" }") LINE_TERMINATOR;
	StartScript += TEXT("$escapedProject = $ProjectFile.Replace('\\', '\\\\')") LINE_TERMINATOR;
	StartScript += TEXT("$running = Get-CimInstance Win32_Process -Filter \"Name = 'UnrealEditor.exe'\" | Where-Object { $_.CommandLine -like \"*$ProjectFile*\" }") LINE_TERMINATOR;
	StartScript += TEXT("if ($ForceRestart -and $running) { $running | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }; $running = $null }") LINE_TERMINATOR;
	StartScript += TEXT("if (-not $running) {") LINE_TERMINATOR;
	StartScript += TEXT("  $args = @($ProjectFile, '-ModelContextProtocolStartServer', \"-ModelContextProtocolPort=$Port\")") LINE_TERMINATOR;
	StartScript += TEXT("  Start-Process -FilePath $EngineExe -ArgumentList $args -WorkingDirectory (Split-Path -Parent $ProjectFile) | Out-Null") LINE_TERMINATOR;
	StartScript += TEXT("}") LINE_TERMINATOR;
	StartScript += TEXT("& (Join-Path $PSScriptRoot 'Wait-HyperAIStudioMCP.ps1') -Port $Port -UrlPath $UrlPath -TimeoutSeconds $TimeoutSeconds") LINE_TERMINATOR;

	FString WaitScript;
	WaitScript += TEXT("param([int]$Port = 8000, [string]$UrlPath = '/mcp', [int]$TimeoutSeconds = 90)") LINE_TERMINATOR;
	WaitScript += TEXT("$ErrorActionPreference = 'Stop'") LINE_TERMINATOR;
	WaitScript += TEXT("$Endpoint = \"http://127.0.0.1:$Port$UrlPath\"") LINE_TERMINATOR;
	WaitScript += TEXT("$HyperAIStudioRoot = Split-Path -Parent $PSScriptRoot") LINE_TERMINATOR;
	WaitScript += TEXT("$StatusPath = Join-Path $HyperAIStudioRoot 'runtime\\hyperai-status.json'") LINE_TERMINATOR;
	WaitScript += TEXT("$StatusDirectory = Split-Path -Parent $StatusPath") LINE_TERMINATOR;
	WaitScript += TEXT("New-Item -ItemType Directory -Force -Path $StatusDirectory | Out-Null") LINE_TERMINATOR;
	WaitScript += TEXT("function Write-HyperAIStatus([string]$State, [string]$Message, [int]$ToolCount = 0) {") LINE_TERMINATOR;
	WaitScript += TEXT("  [pscustomobject]@{ generatedAt = (Get-Date).ToString('o'); ready = ($State -eq 'Ready'); state = $State; endpoint = $Endpoint; tools = $ToolCount; message = $Message } | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $StatusPath -Encoding UTF8") LINE_TERMINATOR;
	WaitScript += TEXT("}") LINE_TERMINATOR;
	WaitScript += TEXT("function Convert-McpJson([string]$Content) {") LINE_TERMINATOR;
	WaitScript += TEXT("  $Text = $Content.Trim()") LINE_TERMINATOR;
	WaitScript += TEXT("  if ($Text.StartsWith('event:') -or $Text.StartsWith('data:')) {") LINE_TERMINATOR;
	WaitScript += TEXT("    $DataLines = @()") LINE_TERMINATOR;
	WaitScript += TEXT("    foreach ($Line in ($Text -split \"`r?`n\")) { if ($Line.StartsWith('data:')) { $DataLines += $Line.Substring(5).TrimStart() } }") LINE_TERMINATOR;
	WaitScript += TEXT("    $Text = ($DataLines -join \"`n\")") LINE_TERMINATOR;
	WaitScript += TEXT("  }") LINE_TERMINATOR;
	WaitScript += TEXT("  return $Text | ConvertFrom-Json") LINE_TERMINATOR;
	WaitScript += TEXT("}") LINE_TERMINATOR;
	WaitScript += TEXT("$Deadline = (Get-Date).AddSeconds($TimeoutSeconds)") LINE_TERMINATOR;
	WaitScript += TEXT("$LastError = ''") LINE_TERMINATOR;
	WaitScript += TEXT("while ((Get-Date) -lt $Deadline) {") LINE_TERMINATOR;
	WaitScript += TEXT("  try {") LINE_TERMINATOR;
	WaitScript += TEXT("    $Headers = $null") LINE_TERMINATOR;
	WaitScript += TEXT("    $InitBody = @{ jsonrpc = '2.0'; id = 1; method = 'initialize'; params = @{ protocolVersion = '2025-11-25'; capabilities = @{}; clientInfo = @{ name = 'HyperAIStudio.Wait'; version = '1.0.0' } } } | ConvertTo-Json -Depth 10") LINE_TERMINATOR;
	WaitScript += TEXT("    $Init = Invoke-WebRequest -Uri $Endpoint -Method Post -ContentType 'application/json' -Body $InitBody -UseBasicParsing -TimeoutSec 5") LINE_TERMINATOR;
	WaitScript += TEXT("    $SessionId = $Init.Headers['Mcp-Session-Id']") LINE_TERMINATOR;
	WaitScript += TEXT("    if ($SessionId -is [array]) { $SessionId = $SessionId[0] }") LINE_TERMINATOR;
	WaitScript += TEXT("    if ([string]::IsNullOrWhiteSpace($SessionId)) { throw 'Initialize did not return Mcp-Session-Id.' }") LINE_TERMINATOR;
	WaitScript += TEXT("    $Headers = @{ 'Mcp-Session-Id' = $SessionId }") LINE_TERMINATOR;
	WaitScript += TEXT("    Invoke-WebRequest -Uri $Endpoint -Method Post -ContentType 'application/json' -Headers $Headers -Body '{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}' -UseBasicParsing -TimeoutSec 5 | Out-Null") LINE_TERMINATOR;
	WaitScript += TEXT("    $List = Invoke-WebRequest -Uri $Endpoint -Method Post -ContentType 'application/json' -Headers $Headers -Body '{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}' -UseBasicParsing -TimeoutSec 10") LINE_TERMINATOR;
	WaitScript += TEXT("    $Json = Convert-McpJson $List.Content") LINE_TERMINATOR;
	WaitScript += TEXT("    $ToolNames = @($Json.result.tools | ForEach-Object { $_.name })") LINE_TERMINATOR;
	WaitScript += TEXT("    $ToolCount = @($ToolNames).Count") LINE_TERMINATOR;
	WaitScript += TEXT("    $DiscoveryDispatchers = @('list_toolsets', 'describe_toolset', 'call_tool')") LINE_TERMINATOR;
	WaitScript += TEXT("    $IsToolSearchMode = ($ToolCount -eq 3 -and @($DiscoveryDispatchers | Where-Object { $ToolNames -notcontains $_ }).Count -eq 0)") LINE_TERMINATOR;
	WaitScript += TEXT("    $ReadyMessage = if ($IsToolSearchMode) { 'tools/list reachable in tool-search mode with 3 discovery dispatchers. Epic and HyperAI tools are available behind those dispatchers. This verifies the editor endpoint only; an agent task must separately load its project MCP config.' } else { \"tools/list reachable with $ToolCount advertised top-level tools. This verifies the editor endpoint only; an agent task must separately load its project MCP config.\" }") LINE_TERMINATOR;
	WaitScript += TEXT("    try { Invoke-WebRequest -Uri $Endpoint -Method Delete -Headers $Headers -UseBasicParsing -TimeoutSec 5 | Out-Null } catch {}") LINE_TERMINATOR;
	WaitScript += TEXT("    Write-HyperAIStatus 'Ready' $ReadyMessage $ToolCount") LINE_TERMINATOR;
	WaitScript += TEXT("    exit 0") LINE_TERMINATOR;
	WaitScript += TEXT("  } catch {") LINE_TERMINATOR;
	WaitScript += TEXT("    $LastError = $_.Exception.Message") LINE_TERMINATOR;
	WaitScript += TEXT("    if ($Headers) { try { Invoke-WebRequest -Uri $Endpoint -Method Delete -Headers $Headers -UseBasicParsing -TimeoutSec 5 | Out-Null } catch {} }") LINE_TERMINATOR;
	WaitScript += TEXT("    Write-HyperAIStatus 'Waiting' $LastError 0") LINE_TERMINATOR;
	WaitScript += TEXT("    Start-Sleep -Seconds 2") LINE_TERMINATOR;
	WaitScript += TEXT("  }") LINE_TERMINATOR;
	WaitScript += TEXT("}") LINE_TERMINATOR;
	WaitScript += TEXT("Write-HyperAIStatus 'Timeout' \"Timed out waiting for $Endpoint. Last error: $LastError\" 0") LINE_TERMINATOR;
	WaitScript += TEXT("exit 1") LINE_TERMINATOR;

	FString SendScript;
	SendScript += TEXT("param([string]$Agent = 'Codex', [string]$PromptFile)") LINE_TERMINATOR;
	SendScript += TEXT("# HyperAIStudio handoff v2: wait for Unreal MCP, then start a fresh project-root agent process.") LINE_TERMINATOR;
	SendScript += TEXT("$ErrorActionPreference = 'Stop'") LINE_TERMINATOR;
	SendScript += TEXT("$HyperAIStudioRoot = Split-Path -Parent $PSScriptRoot") LINE_TERMINATOR;
	SendScript += TEXT("$ProjectRoot = Split-Path -Parent $HyperAIStudioRoot") LINE_TERMINATOR;
	SendScript += TEXT("Set-Location -LiteralPath $ProjectRoot") LINE_TERMINATOR;
	SendScript += TEXT("$env:TERM = 'xterm-256color'") LINE_TERMINATOR;
	SendScript += TEXT("function Find-HyperAIStudioCommand {") LINE_TERMINATOR;
	SendScript += TEXT("  param([string]$Name)") LINE_TERMINATOR;
	SendScript += TEXT("  $CandidateNames = @($Name)") LINE_TERMINATOR;
	SendScript += TEXT("  if ([IO.Path]::GetExtension($Name) -eq '') { $CandidateNames = @(\"$Name.exe\", \"$Name.cmd\", \"$Name.bat\", \"$Name.ps1\", $Name) }") LINE_TERMINATOR;
	SendScript += TEXT("  $Dirs = @()") LINE_TERMINATOR;
	SendScript += TEXT("  if (-not [string]::IsNullOrWhiteSpace($env:PATH)) { $Dirs += ($env:PATH -split ';') }") LINE_TERMINATOR;
	SendScript += TEXT("  if (-not [string]::IsNullOrWhiteSpace($env:APPDATA)) { $Dirs += (Join-Path $env:APPDATA 'npm') }") LINE_TERMINATOR;
	SendScript += TEXT("  if (-not [string]::IsNullOrWhiteSpace($env:LOCALAPPDATA)) {") LINE_TERMINATOR;
	SendScript += TEXT("    $Dirs += (Join-Path $env:LOCALAPPDATA 'npm')") LINE_TERMINATOR;
	SendScript += TEXT("    $Dirs += (Join-Path $env:LOCALAPPDATA 'Programs\\Microsoft VS Code\\bin')") LINE_TERMINATOR;
	SendScript += TEXT("    $Dirs += (Join-Path $env:LOCALAPPDATA 'Programs\\Cursor\\resources\\app\\bin')") LINE_TERMINATOR;
	SendScript += TEXT("    $Dirs += (Join-Path $env:LOCALAPPDATA 'Programs\\cursor\\resources\\app\\bin')") LINE_TERMINATOR;
	SendScript += TEXT("  }") LINE_TERMINATOR;
	SendScript += TEXT("  if (-not [string]::IsNullOrWhiteSpace($env:USERPROFILE)) {") LINE_TERMINATOR;
	SendScript += TEXT("    $Dirs += (Join-Path $env:USERPROFILE '.local\\bin')") LINE_TERMINATOR;
	SendScript += TEXT("    $Dirs += (Join-Path $env:USERPROFILE '.claude\\local')") LINE_TERMINATOR;
	SendScript += TEXT("    $Dirs += (Join-Path $env:USERPROFILE '.codex\\bin')") LINE_TERMINATOR;
	SendScript += TEXT("  }") LINE_TERMINATOR;
	SendScript += TEXT("  if (-not [string]::IsNullOrWhiteSpace(${env:ProgramFiles})) {") LINE_TERMINATOR;
	SendScript += TEXT("    $Dirs += (Join-Path ${env:ProgramFiles} 'Microsoft VS Code\\bin')") LINE_TERMINATOR;
	SendScript += TEXT("    $Dirs += (Join-Path ${env:ProgramFiles} 'Cursor\\resources\\app\\bin')") LINE_TERMINATOR;
	SendScript += TEXT("    $Dirs += (Join-Path ${env:ProgramFiles} 'nodejs')") LINE_TERMINATOR;
	SendScript += TEXT("  }") LINE_TERMINATOR;
	SendScript += TEXT("  if (-not [string]::IsNullOrWhiteSpace(${env:ProgramFiles(x86)})) {") LINE_TERMINATOR;
	SendScript += TEXT("    $Dirs += (Join-Path ${env:ProgramFiles(x86)} 'Microsoft VS Code\\bin')") LINE_TERMINATOR;
	SendScript += TEXT("    $Dirs += (Join-Path ${env:ProgramFiles(x86)} 'nodejs')") LINE_TERMINATOR;
	SendScript += TEXT("  }") LINE_TERMINATOR;
	SendScript += TEXT("  foreach ($Dir in ($Dirs | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } | Select-Object -Unique)) {") LINE_TERMINATOR;
	SendScript += TEXT("    foreach ($CandidateName in $CandidateNames) {") LINE_TERMINATOR;
	SendScript += TEXT("      $Candidate = Join-Path $Dir $CandidateName") LINE_TERMINATOR;
	SendScript += TEXT("      if (Test-Path -LiteralPath $Candidate) { return (Resolve-Path -LiteralPath $Candidate).Path }") LINE_TERMINATOR;
	SendScript += TEXT("    }") LINE_TERMINATOR;
	SendScript += TEXT("  }") LINE_TERMINATOR;
	SendScript += TEXT("  $Command = Get-Command $Name -ErrorAction SilentlyContinue") LINE_TERMINATOR;
	SendScript += TEXT("  if ($Command) { return $Command.Source }") LINE_TERMINATOR;
	SendScript += TEXT("  return $null") LINE_TERMINATOR;
	SendScript += TEXT("}") LINE_TERMINATOR;
	SendScript += TEXT("function Invoke-HyperAIStudioCommand {") LINE_TERMINATOR;
	SendScript += TEXT("  param([string]$CommandPath, [string[]]$CommandArgs = @())") LINE_TERMINATOR;
	SendScript += TEXT("  if ([IO.Path]::GetExtension($CommandPath) -eq '.ps1') { & powershell -NoProfile -ExecutionPolicy Bypass -File $CommandPath @CommandArgs }") LINE_TERMINATOR;
	SendScript += TEXT("  else { & $CommandPath @CommandArgs }") LINE_TERMINATOR;
	SendScript += TEXT("}") LINE_TERMINATOR;
	SendScript += TEXT("if ([string]::IsNullOrWhiteSpace($PromptFile)) { throw 'PromptFile is required.' }") LINE_TERMINATOR;
	SendScript += TEXT("if (!(Test-Path -LiteralPath $PromptFile)) { throw \"Prompt file not found: $PromptFile\" }") LINE_TERMINATOR;
	SendScript += TEXT("$WaitScript = Join-Path $PSScriptRoot 'Wait-HyperAIStudioMCP.ps1'") LINE_TERMINATOR;
	SendScript += TEXT("if (!(Test-Path -LiteralPath $WaitScript)) { throw \"MCP readiness helper not found: $WaitScript\" }") LINE_TERMINATOR;
	SendScript += FString::Printf(TEXT("$McpPort = %u%s"), Settings->UnrealMCPPort, LINE_TERMINATOR);
	SendScript += FString::Printf(TEXT("$McpUrlPath = \"%s\"%s"), *HyperAIStudio::Private::EscapeJsonString(UrlPath), LINE_TERMINATOR);
	SendScript += TEXT("$Prompt = Get-Content -Raw -LiteralPath $PromptFile") LINE_TERMINATOR;
	SendScript += TEXT("Set-Clipboard -Value $Prompt") LINE_TERMINATOR;
	SendScript += TEXT("Write-Host 'HyperAIStudio prompt copied to clipboard.' -ForegroundColor Green") LINE_TERMINATOR;
	SendScript += TEXT("Write-Host \"Prompt file: $PromptFile\"") LINE_TERMINATOR;
	SendScript += TEXT("Write-Host 'Waiting for the Unreal MCP editor endpoint before starting the agent...' -ForegroundColor Cyan") LINE_TERMINATOR;
	SendScript += TEXT("$PowerShellExe = Join-Path $PSHOME 'powershell.exe'") LINE_TERMINATOR;
	SendScript += TEXT("if (!(Test-Path -LiteralPath $PowerShellExe)) { $PowerShellExe = 'powershell.exe' }") LINE_TERMINATOR;
	SendScript += TEXT("& $PowerShellExe -NoProfile -ExecutionPolicy Bypass -File $WaitScript -Port $McpPort -UrlPath $McpUrlPath -TimeoutSeconds 90") LINE_TERMINATOR;
	SendScript += TEXT("if ($LASTEXITCODE -ne 0) { throw \"Unreal MCP did not become ready; the agent was not started. Run $WaitScript for details.\" }") LINE_TERMINATOR;
	SendScript += TEXT("Write-Host 'Unreal MCP editor endpoint is ready. Starting a fresh agent process from the exact project root.' -ForegroundColor Green") LINE_TERMINATOR;
	SendScript += TEXT("$AgentKey = $Agent.ToLowerInvariant()") LINE_TERMINATOR;
	SendScript += TEXT("if ($AgentKey -like '*codex*') {") LINE_TERMINATOR;
	SendScript += TEXT("  $CodexCommand = Find-HyperAIStudioCommand 'codex'") LINE_TERMINATOR;
	SendScript += TEXT("  if ($CodexCommand) {") LINE_TERMINATOR;
	SendScript += TEXT("    $CodexArgs = @()") LINE_TERMINATOR;
	SendScript += TEXT("    $CodexConfigCandidates = @()") LINE_TERMINATOR;
	SendScript += TEXT("    $CodexConfigCandidates += (Join-Path $ProjectRoot '.codex\\config.toml')") LINE_TERMINATOR;
	SendScript += TEXT("    if (-not [string]::IsNullOrWhiteSpace($env:CODEX_HOME)) { $CodexConfigCandidates += (Join-Path $env:CODEX_HOME 'config.toml') }") LINE_TERMINATOR;
	SendScript += TEXT("    if (-not [string]::IsNullOrWhiteSpace($env:USERPROFILE)) { $CodexConfigCandidates += (Join-Path $env:USERPROFILE '.codex\\config.toml') }") LINE_TERMINATOR;
	SendScript += TEXT("    if (-not [string]::IsNullOrWhiteSpace($HOME)) { $CodexConfigCandidates += (Join-Path $HOME '.codex\\config.toml') }") LINE_TERMINATOR;
	SendScript += TEXT("    foreach ($CodexConfig in ($CodexConfigCandidates | Select-Object -Unique)) {") LINE_TERMINATOR;
	SendScript += TEXT("      if ((Test-Path -LiteralPath $CodexConfig) -and ((Get-Content -Raw -LiteralPath $CodexConfig) -match \"(?im)^\\s*service_tier\\s*=\\s*['`\"](?!fast['`\"]|flex['`\"])[^'`\"]+['`\"]\")) {") LINE_TERMINATOR;
	SendScript += TEXT("        $CodexArgs += @('-c', 'service_tier=fast')") LINE_TERMINATOR;
	SendScript += TEXT("        Write-Host 'Codex config contains an unsupported service_tier; launching with service_tier=fast override.' -ForegroundColor Yellow") LINE_TERMINATOR;
	SendScript += TEXT("        break") LINE_TERMINATOR;
	SendScript += TEXT("      }") LINE_TERMINATOR;
	SendScript += TEXT("    }") LINE_TERMINATOR;
	SendScript += TEXT("    Write-Host \"Starting a fresh Codex CLI session from the project root: $CodexCommand\" -ForegroundColor Cyan; Invoke-HyperAIStudioCommand $CodexCommand $CodexArgs") LINE_TERMINATOR;
	SendScript += TEXT("  }") LINE_TERMINATOR;
	SendScript += TEXT("  else { Write-Host 'Codex CLI was not found in PATH or common user install folders.' -ForegroundColor Yellow }") LINE_TERMINATOR;
	SendScript += TEXT("} elseif ($AgentKey -like '*claude*') {") LINE_TERMINATOR;
	SendScript += TEXT("  $ClaudeCommand = Find-HyperAIStudioCommand 'claude'") LINE_TERMINATOR;
	SendScript += TEXT("  if ($ClaudeCommand) { Write-Host \"Opening Claude Code from the project root: $ClaudeCommand\" -ForegroundColor Cyan; Invoke-HyperAIStudioCommand $ClaudeCommand }") LINE_TERMINATOR;
	SendScript += TEXT("  else { Write-Host 'Claude Code CLI was not found in PATH or common user install folders.' -ForegroundColor Yellow }") LINE_TERMINATOR;
	SendScript += TEXT("} elseif ($AgentKey -like '*gemini*') {") LINE_TERMINATOR;
	SendScript += TEXT("  $GeminiCommand = Find-HyperAIStudioCommand 'gemini'") LINE_TERMINATOR;
	SendScript += TEXT("  if ($GeminiCommand) { Write-Host \"Opening Gemini from the project root: $GeminiCommand\" -ForegroundColor Cyan; Invoke-HyperAIStudioCommand $GeminiCommand }") LINE_TERMINATOR;
	SendScript += TEXT("  else { Write-Host 'Gemini CLI was not found in PATH or common user install folders.' -ForegroundColor Yellow }") LINE_TERMINATOR;
	SendScript += TEXT("} elseif ($AgentKey -like '*cursor*') {") LINE_TERMINATOR;
	SendScript += TEXT("  $CursorCommand = Find-HyperAIStudioCommand 'cursor'") LINE_TERMINATOR;
	SendScript += TEXT("  if ($CursorCommand) { Write-Host \"Opening Cursor from the project root: $CursorCommand\" -ForegroundColor Cyan; Invoke-HyperAIStudioCommand $CursorCommand @('.') }") LINE_TERMINATOR;
	SendScript += TEXT("  else { Write-Host 'Cursor CLI was not found in PATH or common user install folders. Open Cursor manually from this project root and paste the copied prompt.' -ForegroundColor Yellow }") LINE_TERMINATOR;
	SendScript += TEXT("} elseif ($AgentKey -like '*vscode*' -or $AgentKey -like '*vs code*' -or $AgentKey -like '*copilot*') {") LINE_TERMINATOR;
	SendScript += TEXT("  $CodeCommand = Find-HyperAIStudioCommand 'code'") LINE_TERMINATOR;
	SendScript += TEXT("  if ($CodeCommand) { Write-Host \"Opening VS Code from the project root: $CodeCommand\" -ForegroundColor Cyan; Invoke-HyperAIStudioCommand $CodeCommand @('.') }") LINE_TERMINATOR;
	SendScript += TEXT("  else { Write-Host 'VS Code CLI was not found in PATH or common user install folders. Open VS Code manually from this project root and paste the copied prompt into Copilot Agent mode.' -ForegroundColor Yellow }") LINE_TERMINATOR;
	SendScript += TEXT("}") LINE_TERMINATOR;

	FString TestCommandsMarkdown;
	TestCommandsMarkdown += TEXT("# HyperAIStudio Test Commands") LINE_TERMINATOR LINE_TERMINATOR;
	TestCommandsMarkdown += TEXT("Run these from the project root or through the HyperAIStudio panel.") LINE_TERMINATOR LINE_TERMINATOR;
	const FHyperAIStudioService Service;
	for (const FHyperAIStudioTestCommand& Command : Service.GetTestCommands())
	{
		TestCommandsMarkdown += FString::Printf(TEXT("## %s%s%s%s%s%s"),
			*Command.Title,
			LINE_TERMINATOR,
			*Command.Description,
			LINE_TERMINATOR,
			LINE_TERMINATOR,
			LINE_TERMINATOR);
		if (!Command.TerminalCommand.IsEmpty())
		{
			TestCommandsMarkdown += TEXT("```powershell") LINE_TERMINATOR;
			TestCommandsMarkdown += Command.TerminalCommand + LINE_TERMINATOR;
			TestCommandsMarkdown += TEXT("```") LINE_TERMINATOR LINE_TERMINATOR;
		}
		TestCommandsMarkdown += TEXT("Prompt intent:") LINE_TERMINATOR LINE_TERMINATOR;
		TestCommandsMarkdown += Command.Intent + LINE_TERMINATOR LINE_TERMINATOR;
	}

	const FString WorkspaceGuide =
		FString(TEXT("# HyperAIStudio Shared Workspace") LINE_TERMINATOR LINE_TERMINATOR) +
		TEXT("`.hyperai/` is the project-local, Git-trackable workspace for durable collaboration between people and external agents.") LINE_TERMINATOR LINE_TERMINATOR +
		TEXT("Keep `README.md` and `INDEX.md` at the workspace root. Generated launch helpers live in `scripts/`, test guidance in `docs/`, durable HyperAIStudio ownership metadata in `state/`, and volatile status, pending transactions, journals, and temporary files in `runtime/`.") LINE_TERMINATOR LINE_TERMINATOR +
		TEXT("Track `context/`, `prompts/`, `work/`, `scripts/`, `docs/`, and `state/` in Git. Do not commit `runtime/`; HyperAIStudio adds a managed `.gitignore` rule for it.") LINE_TERMINATOR LINE_TERMINATOR +
		TEXT("## Work packages") LINE_TERMINATOR LINE_TERMINATOR +
		TEXT("Put substantial planning, research, decisions, logs, prompts, context, and task-local scratch files under:") LINE_TERMINATOR LINE_TERMINATOR +
		TEXT("`.hyperai/work/YYYY-MM-DD-<clear-context-name>/`") LINE_TERMINATOR LINE_TERMINATOR +
		TEXT("Use a clear lowercase kebab-case context name. A package may contain `README.md`, `plan.md`, `context/`, `prompts/`, `research/`, `decisions/`, `log/`, and `temp/`, but create only what the task needs.") LINE_TERMINATOR LINE_TERMINATOR +
		TEXT("Name durable architecture or product decisions `decisions/YYYY-MM-DD-<system>-<decision>.md`. Add concise dated log entries after meaningful work, not after simple questions or read-only checks.") LINE_TERMINATOR LINE_TERMINATOR +
		TEXT("Do not create generic agent-work folders in the project root, move real project folders into `.hyperai/`, or store secrets, binaries, and raw terminal dumps here.") LINE_TERMINATOR;

	const FString WorkspaceIndexGuide =
		FString(TEXT("# HyperAIStudio Project Knowledge Index") LINE_TERMINATOR LINE_TERMINATOR) +
		TEXT("Keep this as a lightweight index to useful workpackages and durable decisions. Add human- or agent-authored links and short summaries outside this managed block so HyperAIStudio preserves them.") LINE_TERMINATOR LINE_TERMINATOR +
		TEXT("Agents should consult this index only when prior project context is relevant, then read only the linked material needed for the task.") LINE_TERMINATOR;

	if (!HyperAIStudio::Private::UpsertManagedBlock(
		FPaths::Combine(HyperDir, TEXT("README.md")),
		WorkspaceGuide,
		HyperAIStudio::Private::ManagedWorkspaceGuideBegin,
		HyperAIStudio::Private::ManagedWorkspaceGuideEnd))
	{
		OutMessage = FText::FromString(TEXT("Could not safely update .hyperai/README.md."));
		return false;
	}

	if (!HyperAIStudio::Private::UpsertManagedBlock(
		FPaths::Combine(HyperDir, TEXT("INDEX.md")),
		WorkspaceIndexGuide,
		HyperAIStudio::Private::ManagedWorkspaceIndexBegin,
		HyperAIStudio::Private::ManagedWorkspaceIndexEnd))
	{
		OutMessage = FText::FromString(TEXT("Could not safely update .hyperai/INDEX.md."));
		return false;
	}

	if (!HyperAIStudio::Private::UpsertManagedBlock(
		FPaths::Combine(ProjectRoot, TEXT(".gitignore")),
		TEXT(".hyperai/runtime/"),
		HyperAIStudio::Private::ManagedGitIgnoreBegin,
		HyperAIStudio::Private::ManagedGitIgnoreEnd))
	{
		OutMessage = FText::FromString(TEXT("Could not safely update the HyperAIStudio runtime rule in .gitignore."));
		return false;
	}

	if (!HyperAIStudio::Private::WriteFileIfChanged(FPaths::Combine(ScriptsDir, TEXT("Start-HyperAIStudioEditor.ps1")), StartScript))
	{
		OutMessage = FText::FromString(TEXT("Could not write scripts/Start-HyperAIStudioEditor.ps1."));
		return false;
	}

	if (!HyperAIStudio::Private::WriteFileIfChanged(FPaths::Combine(ScriptsDir, TEXT("Wait-HyperAIStudioMCP.ps1")), WaitScript))
	{
		OutMessage = FText::FromString(TEXT("Could not write scripts/Wait-HyperAIStudioMCP.ps1."));
		return false;
	}

	if (!HyperAIStudio::Private::WriteFileIfChanged(FPaths::Combine(ScriptsDir, TEXT("Send-HyperAIStudioPrompt.ps1")), SendScript))
	{
		OutMessage = FText::FromString(TEXT("Could not write scripts/Send-HyperAIStudioPrompt.ps1."));
		return false;
	}

	if (!HyperAIStudio::Private::WriteFileIfChanged(FPaths::Combine(DocsDir, TEXT("HyperAIStudio-TestCommands.md")), TestCommandsMarkdown))
	{
		OutMessage = FText::FromString(TEXT("Could not write docs/HyperAIStudio-TestCommands.md."));
		return false;
	}

	const TArray<FString> LegacyGeneratedFiles = {
		FPaths::Combine(HyperDir, TEXT("Start-HyperAIStudioEditor.ps1")),
		FPaths::Combine(HyperDir, TEXT("Wait-HyperAIStudioMCP.ps1")),
		FPaths::Combine(HyperDir, TEXT("Send-HyperAIStudioPrompt.ps1")),
		FPaths::Combine(HyperDir, TEXT("HyperAIStudio-TestCommands.md"))
	};
	for (const FString& LegacyGeneratedFile : LegacyGeneratedFiles)
	{
		if (FPaths::FileExists(LegacyGeneratedFile)
			&& !IFileManager::Get().Delete(*LegacyGeneratedFile, false, true, true))
		{
			OutMessage = FText::FromString(FString::Printf(
				TEXT("Generated the organized workspace but could not remove legacy file %s."),
				*LegacyGeneratedFile));
			return false;
		}
	}
	const FString LegacyTmpDir = FPaths::Combine(HyperDir, TEXT("tmp"));
	if (IFileManager::Get().DirectoryExists(*LegacyTmpDir))
	{
		bool bContainsEntries = false;
		IFileManager::Get().IterateDirectory(*LegacyTmpDir, [&bContainsEntries](const TCHAR*, bool)
		{
			bContainsEntries = true;
			return false;
		});
		if (!bContainsEntries)
		{
			IFileManager::Get().DeleteDirectory(*LegacyTmpDir, false, false);
		}
	}

	OutMessage = FText::FromString(TEXT("Helper scripts generated."));
	return true;
}

bool FHyperAIStudioService::WriteStatusJson(const FString& ProjectRoot, const FHyperAIStudioStatus& Status)
{
	TSharedRef<FJsonObject> RootObject = MakeShared<FJsonObject>();
	RootObject->SetStringField(TEXT("generatedAt"), FDateTime::UtcNow().ToIso8601());
	RootObject->SetBoolField(TEXT("ready"), Status.IsReady());
	RootObject->SetStringField(TEXT("state"), Status.IsReady() ? TEXT("Ready") : TEXT("Setup needed"));
	RootObject->SetStringField(TEXT("endpoint"), Status.Endpoint);
	RootObject->SetBoolField(TEXT("unrealMcpModuleAvailable"), Status.bUnrealMCPModuleAvailable);
	RootObject->SetBoolField(TEXT("unrealMcpSettingsConfigured"), Status.bUnrealMCPSettingsConfigured);
	RootObject->SetBoolField(TEXT("requiredPluginsReady"), Status.bRequiredPluginsReady);
	RootObject->SetStringField(TEXT("capabilityPreset"), Status.CapabilityPresetName);
	RootObject->SetBoolField(TEXT("desiredCapabilityPluginsReady"), Status.bDesiredCapabilityPluginsReady);
	RootObject->SetNumberField(TEXT("desiredCapabilityPluginCount"), Status.DesiredCapabilityPluginCount);
	RootObject->SetNumberField(TEXT("enabledCapabilityPluginCount"), Status.EnabledCapabilityPluginCount);
	RootObject->SetBoolField(TEXT("serverRunning"), Status.bServerRunning);
	RootObject->SetBoolField(TEXT("portListening"), Status.bPortListening);
	RootObject->SetBoolField(TEXT("toolsListReachable"), Status.bToolsListReachable);
	RootObject->SetBoolField(TEXT("agentFilesReady"), Status.bAgentFilesReady);
	RootObject->SetNumberField(TEXT("configuredPort"), Status.ConfiguredPort);
	RootObject->SetNumberField(TEXT("activePort"), Status.ActivePort);
	RootObject->SetNumberField(TEXT("registeredToolCount"), Status.RegisteredToolCount);
	RootObject->SetNumberField(TEXT("probeToolCount"), Status.ProbeToolCount);
	RootObject->SetBoolField(TEXT("capabilityInventoryAvailable"), Status.bCapabilityInventoryAvailable);
	RootObject->SetBoolField(TEXT("capabilityInventoryInProgress"), Status.bCapabilityInventoryInProgress);
	RootObject->SetBoolField(TEXT("toolSearchMode"), Status.bToolSearchMode);
	RootObject->SetBoolField(TEXT("capabilityInventoryTruncated"), Status.bCapabilityInventoryTruncated);
	RootObject->SetNumberField(TEXT("discoverableToolsetCount"), Status.DiscoverableToolsetCount);
	RootObject->SetNumberField(TEXT("describedToolsetCount"), Status.DescribedToolsetCount);
	RootObject->SetNumberField(TEXT("inventoryToolCount"), Status.InventoryToolCount);
	RootObject->SetStringField(TEXT("capabilityInventoryMessage"), Status.CapabilityInventoryMessage);
	RootObject->SetStringField(TEXT("capabilityInventoryFingerprint"), Status.CapabilityInventoryFingerprint);
	RootObject->SetNumberField(TEXT("configuredAgentCount"), Status.ConfiguredAgentCount);
	RootObject->SetNumberField(TEXT("supportedAgentCount"), Status.SupportedAgentCount);
	RootObject->SetStringField(TEXT("probeMessage"), Status.ProbeMessage);
	const FString HyperDir = FPaths::Combine(ProjectRoot, TEXT(".hyperai"));
	const FString StatusPath = FPaths::Combine(HyperDir, TEXT("runtime"), TEXT("hyperai-status.json"));
	if (!HyperAIStudio::Private::SerializeJsonToFile(StatusPath, RootObject))
	{
		return false;
	}
	const FString LegacyStatusPath = FPaths::Combine(HyperDir, TEXT("hyperai-status.json"));
	return !FPaths::FileExists(LegacyStatusPath)
		|| IFileManager::Get().Delete(*LegacyStatusPath, false, true, true);
}

bool FHyperAIStudioService::AreAgentFilesReady(const FString& ProjectRoot, const UHyperAIStudioSettings* Settings)
{
	if (!Settings)
	{
		return false;
	}

	TArray<FString> RequiredFiles;
	RequiredFiles.Add(FPaths::Combine(ProjectRoot, TEXT("AGENTS.md")));
	RequiredFiles.Add(FPaths::Combine(ProjectRoot, TEXT(".hyperai"), TEXT("scripts"), TEXT("Start-HyperAIStudioEditor.ps1")));
	RequiredFiles.Add(FPaths::Combine(ProjectRoot, TEXT(".hyperai"), TEXT("scripts"), TEXT("Wait-HyperAIStudioMCP.ps1")));
	RequiredFiles.Add(FPaths::Combine(ProjectRoot, TEXT(".hyperai"), TEXT("scripts"), TEXT("Send-HyperAIStudioPrompt.ps1")));
	RequiredFiles.Add(FPaths::Combine(ProjectRoot, TEXT(".hyperai"), TEXT("docs"), TEXT("HyperAIStudio-TestCommands.md")));
	RequiredFiles.Add(FPaths::Combine(ProjectRoot, TEXT(".hyperai"), TEXT("README.md")));
	RequiredFiles.Add(FPaths::Combine(ProjectRoot, TEXT(".hyperai"), TEXT("INDEX.md")));

	if (Settings->bGenerateCodexConfig && Settings->bEnableCodexAgent)
	{
		RequiredFiles.Add(FPaths::Combine(ProjectRoot, TEXT(".codex"), TEXT("config.toml")));
	}
	if (Settings->bGenerateClaudeConfig && Settings->bEnableClaudeAgent)
	{
		RequiredFiles.Add(FPaths::Combine(ProjectRoot, TEXT(".mcp.json")));
	}
	if (Settings->bGenerateClaudeMarkdown && Settings->bEnableClaudeAgent)
	{
		RequiredFiles.Add(FPaths::Combine(ProjectRoot, TEXT("CLAUDE.md")));
	}
	if (Settings->bGenerateGeminiConfig && Settings->bEnableGeminiAgent)
	{
		RequiredFiles.Add(FPaths::Combine(ProjectRoot, TEXT(".gemini"), TEXT("settings.json")));
	}
	if (Settings->bGenerateCursorConfig && Settings->bEnableCursorAgent)
	{
		RequiredFiles.Add(FPaths::Combine(ProjectRoot, TEXT(".cursor"), TEXT("mcp.json")));
	}
	if (Settings->bGenerateVSCodeConfig && Settings->bEnableVSCodeAgent)
	{
		RequiredFiles.Add(FPaths::Combine(ProjectRoot, TEXT(".vscode"), TEXT("mcp.json")));
	}

	for (const FString& RequiredFile : RequiredFiles)
	{
		if (!FPaths::FileExists(RequiredFile))
		{
			return false;
		}
	}

	return true;
}

int32 FHyperAIStudioService::CountConfiguredAgents(const FString& ProjectRoot, const UHyperAIStudioSettings* Settings)
{
	if (!Settings)
	{
		return 0;
	}

	int32 Count = 0;
	if (Settings->bEnableCodexAgent
		&& Settings->bGenerateCodexConfig
		&& FPaths::FileExists(FPaths::Combine(ProjectRoot, TEXT(".codex"), TEXT("config.toml")))
		&& IsExecutableOnPath(TEXT("codex")))
	{
		++Count;
	}
	if (Settings->bEnableClaudeAgent
		&& Settings->bGenerateClaudeConfig
		&& FPaths::FileExists(FPaths::Combine(ProjectRoot, TEXT(".mcp.json")))
		&& IsExecutableOnPath(TEXT("claude")))
	{
		++Count;
	}
	if (Settings->bEnableGeminiAgent
		&& Settings->bGenerateGeminiConfig
		&& FPaths::FileExists(FPaths::Combine(ProjectRoot, TEXT(".gemini"), TEXT("settings.json")))
		&& IsExecutableOnPath(TEXT("gemini")))
	{
		++Count;
	}
	if (Settings->bEnableCursorAgent
		&& Settings->bGenerateCursorConfig
		&& FPaths::FileExists(FPaths::Combine(ProjectRoot, TEXT(".cursor"), TEXT("mcp.json")))
		&& IsExecutableOnPath(TEXT("cursor")))
	{
		++Count;
	}
	if (Settings->bEnableVSCodeAgent
		&& Settings->bGenerateVSCodeConfig
		&& FPaths::FileExists(FPaths::Combine(ProjectRoot, TEXT(".vscode"), TEXT("mcp.json")))
		&& IsExecutableOnPath(TEXT("code")))
	{
		++Count;
	}
	return Count;
}

bool FHyperAIStudioService::IsPortListening(uint32 Port)
{
	if (Port == 0)
	{
		return false;
	}

	ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (!SocketSubsystem)
	{
		return false;
	}

	bool bIsValidIp = false;
	TSharedRef<FInternetAddr> Address = SocketSubsystem->CreateInternetAddr();
	Address->SetIp(TEXT("127.0.0.1"), bIsValidIp);
	Address->SetPort(static_cast<int32>(Port));
	if (!bIsValidIp)
	{
		return false;
	}

	FSocket* Socket = SocketSubsystem->CreateSocket(NAME_Stream, TEXT("HyperAIStudioMCPPortCheck"), false);
	if (!Socket)
	{
		return false;
	}

	Socket->SetNonBlocking(false);
	const bool bConnected = Socket->Connect(*Address);
	SocketSubsystem->DestroySocket(Socket);
	return bConnected;
}

void FHyperAIStudioService::DescribeUnrealMCPToolsAsync(TFunction<void(const FString& Details)> OnComplete) const
{
	const FHyperAIStudioStatus CurrentStatus = GetMcpRuntimeStatusFast();
	const FString HyperAIDetails = HyperAIStudio::Private::BuildHyperAIToolDialogDetails();
	const FString Endpoint = CurrentStatus.Endpoint.IsEmpty()
		? GetEndpoint(CurrentStatus.ConfiguredPort > 0 ? CurrentStatus.ConfiguredPort : 8000, CurrentStatus.UrlPath.IsEmpty() ? TEXT("/mcp") : CurrentStatus.UrlPath)
		: CurrentStatus.Endpoint;
	FHyperAIStudioCapabilityInventoryClient::RefreshAsync(Endpoint, false,
		[OnComplete, HyperAIDetails](const FHyperAIStudioCapabilityInventoryResult& Inventory)
		{
			if (!Inventory.bSuccess)
			{
				OnComplete(HyperAIStudio::Private::LimitToolDialogText(
					HyperAIDetails
					+ TEXT("Live Unreal MCP inventory") LINE_TERMINATOR
					+ FString::Printf(TEXT("Inventory unavailable: %s"), *Inventory.Message)));
				return;
			}

			const TCHAR* Mode = TEXT("unknown");
			switch (Inventory.Snapshot.DiscoveryMode)
			{
			case EHyperAIStudioToolDiscoveryMode::ToolSearch: Mode = TEXT("tool-search"); break;
			case EHyperAIStudioToolDiscoveryMode::Eager: Mode = TEXT("eager"); break;
			case EHyperAIStudioToolDiscoveryMode::Degraded: Mode = TEXT("degraded"); break;
			default: break;
			}
			const TCHAR* TopLevelLabel = Inventory.Snapshot.DiscoveryMode == EHyperAIStudioToolDiscoveryMode::ToolSearch
				? TEXT("Top-level discovery dispatchers")
				: TEXT("Top-level advertised tools");
			FString Details = HyperAIDetails + TEXT("Live Unreal MCP inventory") LINE_TERMINATOR;
			Details += FString::Printf(
				TEXT("%s%sMode: %s%s%s: %d%sDiscoverable toolsets: %d%sDescribed tools: %d%sFingerprint: %s%s"),
				*Inventory.Message, LINE_TERMINATOR,
				Mode, LINE_TERMINATOR,
				TopLevelLabel,
				Inventory.Snapshot.TopLevelToolCount, LINE_TERMINATOR,
				Inventory.Snapshot.DiscoverableToolsetCount, LINE_TERMINATOR,
				Inventory.Snapshot.DescribedToolCount, LINE_TERMINATOR,
				*Inventory.Snapshot.InventoryFingerprint, LINE_TERMINATOR);
			if (!Inventory.Snapshot.DescribedToolsets.IsEmpty())
			{
				Details += LINE_TERMINATOR TEXT("Live toolsets (Epic and HyperAI):") LINE_TERMINATOR;
				for (const FHyperAIStudioDescribedToolset& Toolset : Inventory.Snapshot.DescribedToolsets)
				{
					Details += FString::Printf(TEXT("- %s — %d tools — %s%s"),
						*Toolset.Name, Toolset.ToolCount, *Toolset.SchemaHash, LINE_TERMINATOR);
				}
			}
			OnComplete(HyperAIStudio::Private::LimitToolDialogText(Details));
		});
	return;

#if 0 // Superseded by the coalesced serial capability-inventory client above.

	const FString InitializePayload = FString::Printf(
		TEXT("{\"jsonrpc\":\"%s\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"%s\",\"capabilities\":{},\"clientInfo\":{\"name\":\"HyperAIStudio Tools Dialog\",\"version\":\"1.0.0\"}}}"),
		UE::ModelContextProtocol::JsonRpcVersion,
		UE::ModelContextProtocol::ProtocolVersion);

	TSharedRef<IHttpRequest> InitRequest = FHttpModule::Get().CreateRequest();
	InitRequest->SetURL(Endpoint);
		InitRequest->SetVerb(TEXT("POST"));
		InitRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
		InitRequest->SetHeader(TEXT("Accept"), TEXT("application/json, text/event-stream"));
		InitRequest->SetTimeout(5.0f);
	InitRequest->SetHeader(TEXT("Accept"), TEXT("application/json, text/event-stream"));
	InitRequest->SetContentAsString(InitializePayload);
	InitRequest->OnProcessRequestComplete().BindLambda([Endpoint, OnComplete](FHttpRequestPtr, FHttpResponsePtr InitResponse, bool bInitConnected)
	{
		if (!bInitConnected || !InitResponse.IsValid())
		{
			OnComplete(FString::Printf(TEXT("Could not connect to Unreal MCP initialize endpoint: %s"), *Endpoint));
			return;
		}

		const int32 InitCode = InitResponse->GetResponseCode();
		if (InitCode < 200 || InitCode >= 300)
		{
			OnComplete(FString::Printf(TEXT("Initialize returned HTTP %d."), InitCode));
			return;
		}

		const FString SessionId = InitResponse->GetHeader(TEXT("Mcp-Session-Id"));
		if (SessionId.IsEmpty())
		{
			OnComplete(TEXT("Initialize did not return Mcp-Session-Id."));
			return;
		}

		FString ProtocolVersion = HyperAIStudio::Private::ExtractNegotiatedProtocolVersion(InitResponse->GetContentAsString());
		if (ProtocolVersion.IsEmpty())
		{
			ProtocolVersion = UE::ModelContextProtocol::ProtocolVersion;
		}

		TSharedRef<IHttpRequest> InitializedRequest = FHttpModule::Get().CreateRequest();
		InitializedRequest->SetURL(Endpoint);
		InitializedRequest->SetVerb(TEXT("POST"));
		InitializedRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
		InitializedRequest->SetHeader(TEXT("Accept"), TEXT("application/json, text/event-stream"));
		InitializedRequest->SetHeader(TEXT("Accept"), TEXT("application/json, text/event-stream"));
		InitializedRequest->SetHeader(TEXT("Mcp-Session-Id"), SessionId);
		InitializedRequest->SetHeader(TEXT("Mcp-Protocol-Version"), ProtocolVersion);
		InitializedRequest->SetContentAsString(TEXT("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\",\"params\":{}}"));
		InitializedRequest->OnProcessRequestComplete().BindLambda([Endpoint, SessionId, ProtocolVersion, OnComplete](FHttpRequestPtr, FHttpResponsePtr InitializedResponse, bool bInitializedConnected)
		{
			if (!bInitializedConnected || !InitializedResponse.IsValid())
			{
				HyperAIStudio::Private::DeleteMcpSession(Endpoint, SessionId);
				OnComplete(TEXT("Could not send notifications/initialized."));
				return;
			}

			const int32 InitializedCode = InitializedResponse->GetResponseCode();
			if (InitializedCode < 200 || InitializedCode >= 300)
			{
				HyperAIStudio::Private::DeleteMcpSession(Endpoint, SessionId);
				OnComplete(FString::Printf(TEXT("notifications/initialized returned HTTP %d."), InitializedCode));
				return;
			}

			TSharedRef<IHttpRequest> ListRequest = FHttpModule::Get().CreateRequest();
			ListRequest->SetURL(Endpoint);
			ListRequest->SetVerb(TEXT("POST"));
			ListRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
			ListRequest->SetHeader(TEXT("Accept"), TEXT("application/json, text/event-stream"));
			ListRequest->SetHeader(TEXT("Mcp-Session-Id"), SessionId);
			ListRequest->SetHeader(TEXT("Mcp-Protocol-Version"), ProtocolVersion);
			ListRequest->SetContentAsString(TEXT("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}"));
			ListRequest->OnProcessRequestComplete().BindLambda([Endpoint, SessionId, ProtocolVersion, OnComplete](FHttpRequestPtr, FHttpResponsePtr ListResponse, bool bListConnected)
			{
				if (!bListConnected || !ListResponse.IsValid())
				{
					HyperAIStudio::Private::DeleteMcpSession(Endpoint, SessionId);
					OnComplete(TEXT("Could not connect to tools/list."));
					return;
				}

				const int32 ListCode = ListResponse->GetResponseCode();
				if (ListCode < 200 || ListCode >= 300)
				{
					HyperAIStudio::Private::DeleteMcpSession(Endpoint, SessionId);
					OnComplete(FString::Printf(TEXT("tools/list returned HTTP %d."), ListCode));
					return;
				}

					TArray<FString> ToolNames;
					const FString ToolsListDetails = HyperAIStudio::Private::BuildToolsListDetails(ListResponse->GetContentAsString(), ToolNames);
					HyperAIStudio::Private::DeleteMcpSession(Endpoint, SessionId);
					OnComplete(HyperAIStudio::Private::LimitToolDialogText(ToolsListDetails));
				});
				ListRequest->ProcessRequest();
			});
		InitializedRequest->ProcessRequest();
		});
	InitRequest->ProcessRequest();
#endif
}

void FHyperAIStudioService::ProbeToolsListAsync(const FString& Endpoint, TFunction<void(bool bReachable, const FString& Message, int32 ToolCount)> OnComplete)
{
	FHyperAIStudioMcpSerialQueue::Enqueue(Endpoint,
		[Endpoint, OnComplete](FHyperAIStudioMcpSerialQueue::FComplete QueueComplete)
		{
			const TSharedRef<HyperAIStudio::Private::FShallowMcpProbeCompletion> Completion =
				MakeShared<HyperAIStudio::Private::FShallowMcpProbeCompletion>(Endpoint, MoveTemp(QueueComplete), OnComplete);
		const FString InitializePayload = FString::Printf(
			TEXT("{\"jsonrpc\":\"%s\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"%s\",\"capabilities\":{},\"clientInfo\":{\"name\":\"HyperAIStudio\",\"version\":\"1.0.0\"}}}"),
			UE::ModelContextProtocol::JsonRpcVersion,
			UE::ModelContextProtocol::ProtocolVersion);

		TSharedRef<IHttpRequest> InitRequest = FHttpModule::Get().CreateRequest();
		InitRequest->SetURL(Endpoint);
		InitRequest->SetVerb(TEXT("POST"));
		InitRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
		InitRequest->SetHeader(TEXT("Accept"), TEXT("application/json, text/event-stream"));
		InitRequest->SetContentAsString(InitializePayload);
		InitRequest->SetTimeout(5.0f);
			InitRequest->OnProcessRequestComplete().BindLambda(
				[Endpoint, Completion](FHttpRequestPtr CompletedRequest, FHttpResponsePtr InitResponse, const bool bInitConnected)
				{
					if (!bInitConnected || !InitResponse.IsValid())
					{
						const bool bAmbiguous = !HyperAIStudio::Private::IsMcpRequestProvablyNotSent(CompletedRequest);
						Completion->Complete(
							false,
							bAmbiguous
								? TEXT("Unreal MCP initialize outcome is unknown; calls are locked until a confirmed server restart.")
								: TEXT("Could not connect to Unreal MCP initialize endpoint; the request was not sent."),
							0,
							false,
							bAmbiguous);
					return;
				}

				FString SessionId = InitResponse->GetHeader(TEXT("Mcp-Session-Id")).TrimStartAndEnd();
				FString ProtocolVersion = UE::ModelContextProtocol::ProtocolVersion;
				Completion->SetSession(SessionId, ProtocolVersion);
				const int32 InitCode = InitResponse->GetResponseCode();
				if (InitCode < 200 || InitCode >= 300)
				{
					Completion->Complete(false, FString::Printf(TEXT("Initialize returned HTTP %d."), InitCode), 0, true);
					return;
				}

				FString InitBody;
				FString ParseError;
				if (!HyperAIStudio::Private::ReadBoundedMcpResponseBody(InitResponse, InitBody, ParseError))
				{
					Completion->Complete(false, ParseError, 0, true);
					return;
				}
				TSharedPtr<FJsonObject> InitializeResult;
				if (!FHyperAIStudioCapabilityInventoryParser::ParseJsonRpcResult(
					InitBody, 1, InitializeResult, ParseError))
				{
					Completion->Complete(false, ParseError, 0, true);
					return;
				}
				if (InitializeResult->HasField(TEXT("protocolVersion")))
				{
					if (!InitializeResult->TryGetStringField(TEXT("protocolVersion"), ProtocolVersion)
						|| ProtocolVersion.IsEmpty())
					{
						Completion->Complete(false, TEXT("initialize result.protocolVersion is not a non-empty string."), 0, true);
						return;
					}
				}
				Completion->SetSession(SessionId, ProtocolVersion);

				TSharedRef<IHttpRequest> InitializedRequest = FHttpModule::Get().CreateRequest();
				InitializedRequest->SetURL(Endpoint);
				InitializedRequest->SetVerb(TEXT("POST"));
				InitializedRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
				InitializedRequest->SetHeader(TEXT("Accept"), TEXT("application/json, text/event-stream"));
				InitializedRequest->SetHeader(TEXT("Mcp-Protocol-Version"), ProtocolVersion);
				if (!SessionId.IsEmpty())
				{
					InitializedRequest->SetHeader(TEXT("Mcp-Session-Id"), SessionId);
				}
				InitializedRequest->SetContentAsString(TEXT("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\",\"params\":{}}"));
				InitializedRequest->SetTimeout(5.0f);
					InitializedRequest->OnProcessRequestComplete().BindLambda(
						[Endpoint, SessionId, ProtocolVersion, Completion](FHttpRequestPtr CompletedRequest, FHttpResponsePtr InitializedResponse, const bool bInitializedConnected)
						{
							if (!bInitializedConnected || !InitializedResponse.IsValid())
							{
								const bool bAmbiguous = !HyperAIStudio::Private::IsMcpRequestProvablyNotSent(CompletedRequest);
								Completion->Complete(
									false,
									bAmbiguous
										? TEXT("notifications/initialized outcome is unknown; calls are locked until a confirmed server restart.")
										: TEXT("Could not connect for notifications/initialized; the request was not sent."),
									0,
									false,
									bAmbiguous);
							return;
						}
						const int32 InitializedCode = InitializedResponse->GetResponseCode();
						if (InitializedCode < 200 || InitializedCode >= 300)
						{
							Completion->Complete(false, FString::Printf(
								TEXT("notifications/initialized returned HTTP %d."), InitializedCode), 0, true);
							return;
						}

						FString InitializedBody;
						FString ParseError;
						if (!HyperAIStudio::Private::ReadBoundedMcpResponseBody(
							InitializedResponse, InitializedBody, ParseError))
						{
							Completion->Complete(false, ParseError, 0, true);
							return;
						}
						if (!InitializedBody.TrimStartAndEnd().IsEmpty())
						{
							Completion->Complete(false, TEXT("notifications/initialized returned an unexpected response body."), 0, true);
							return;
						}

						TSharedRef<IHttpRequest> ListRequest = FHttpModule::Get().CreateRequest();
						ListRequest->SetURL(Endpoint);
						ListRequest->SetVerb(TEXT("POST"));
						ListRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
						ListRequest->SetHeader(TEXT("Accept"), TEXT("application/json, text/event-stream"));
						ListRequest->SetHeader(TEXT("Mcp-Protocol-Version"), ProtocolVersion);
						if (!SessionId.IsEmpty())
						{
							ListRequest->SetHeader(TEXT("Mcp-Session-Id"), SessionId);
						}
						ListRequest->SetContentAsString(TEXT("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}"));
						ListRequest->SetTimeout(5.0f);
							ListRequest->OnProcessRequestComplete().BindLambda(
								[Completion](FHttpRequestPtr CompletedRequest, FHttpResponsePtr ListResponse, const bool bListConnected)
								{
									if (!bListConnected || !ListResponse.IsValid())
									{
										const bool bAmbiguous = !HyperAIStudio::Private::IsMcpRequestProvablyNotSent(CompletedRequest);
										Completion->Complete(
											false,
											bAmbiguous
												? TEXT("tools/list outcome is unknown; calls are locked until a confirmed server restart.")
												: TEXT("Could not connect for tools/list; the request was not sent."),
											0,
											false,
											bAmbiguous);
									return;
								}
								const int32 ListCode = ListResponse->GetResponseCode();
								if (ListCode < 200 || ListCode >= 300)
								{
									Completion->Complete(false, FString::Printf(TEXT("tools/list returned HTTP %d."), ListCode), 0, true);
									return;
								}

								FString ListBody;
								FString ParseError;
								if (!HyperAIStudio::Private::ReadBoundedMcpResponseBody(ListResponse, ListBody, ParseError))
								{
									Completion->Complete(false, ParseError, 0, true);
									return;
								}
								FHyperAIStudioCapabilitySnapshot Snapshot;
								if (!FHyperAIStudioCapabilityInventoryParser::ParseToolsListResponse(
									ListBody, 2, Snapshot, ParseError))
								{
									Completion->Complete(false, ParseError, 0, true);
									return;
								}

								FString Message;
								switch (Snapshot.DiscoveryMode)
								{
								case EHyperAIStudioToolDiscoveryMode::ToolSearch:
									Message = FString::Printf(TEXT("tools/list reachable with %d dispatcher tools."), Snapshot.TopLevelToolCount);
									break;
								case EHyperAIStudioToolDiscoveryMode::Eager:
									Message = FString::Printf(TEXT("tools/list reachable with %d eager top-level tools."), Snapshot.TopLevelToolCount);
									break;
								case EHyperAIStudioToolDiscoveryMode::Degraded:
									Message = FString::Printf(TEXT("tools/list reachable, but discovery is degraded: %s"), *Snapshot.ProbeError);
									break;
								default:
									Message = TEXT("tools/list reachable, but discovery mode is unknown.");
									break;
								}
								if (Snapshot.bTruncated)
								{
									Message += TEXT(" Response is incomplete or bounded.");
								}
								Completion->Complete(true, MoveTemp(Message), Snapshot.TopLevelToolCount, true);
							});
						if (!ListRequest->ProcessRequest())
						{
							Completion->Complete(false, TEXT("tools/list request could not be queued."), 0, true);
						}
					});
				if (!InitializedRequest->ProcessRequest())
				{
					Completion->Complete(false, TEXT("notifications/initialized request could not be queued."), 0, true);
				}
			});
		if (!InitRequest->ProcessRequest())
		{
			Completion->Complete(false, TEXT("Initialize request could not be queued."), 0, false);
		}
		},
		[OnComplete](const FString& Reason)
		{
			if (OnComplete)
			{
				OnComplete(false, Reason, 0);
			}
		});
}
