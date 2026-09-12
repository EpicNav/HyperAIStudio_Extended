// Games by Hyper 2026.

#include "HyperAIStudioLegacyMigration.h"

namespace HyperAIStudio::LegacyMigration::Private
{
	constexpr const TCHAR* UnrealServerId = TEXT("hyper-ue-mcp");
	constexpr const TCHAR* UnrealFullServerId = TEXT("hyper-ue-mcp-full");
	constexpr const TCHAR* KnowledgeServerId = TEXT("hyper-knowledge-mcp");
	constexpr uint64 HistoricalDirectExecutableTailFingerprint = 0x3406592f6925ddb1ull;

	bool HasEmbeddedNull(const FString& Value)
	{
		for (const TCHAR Character : Value)
		{
			if (Character == TEXT('\0'))
			{
				return true;
			}
		}
		return false;
	}

	bool ValidateBoundedInput(const FHyperAIStudioLegacyProfileInput& Input, FString& OutReasonCode)
	{
		if (Input.bStructurallyMalformed)
		{
			OutReasonCode = TEXT("source-marked-malformed");
			return false;
		}
		if (Input.ServerId.Len() > FHyperAIStudioLegacyProfileRecognizer::MaxServerIdCharacters)
		{
			OutReasonCode = TEXT("server-id-too-large");
			return false;
		}
		if (Input.ServerId.TrimStartAndEnd().IsEmpty())
		{
			OutReasonCode = TEXT("server-id-empty");
			return false;
		}
		if (Input.Transport.Len() > FHyperAIStudioLegacyProfileRecognizer::MaxTransportCharacters)
		{
			OutReasonCode = TEXT("transport-too-large");
			return false;
		}
		if (Input.Command.Len() > FHyperAIStudioLegacyProfileRecognizer::MaxCommandCharacters)
		{
			OutReasonCode = TEXT("command-too-large");
			return false;
		}
		if (Input.Url.Len() > FHyperAIStudioLegacyProfileRecognizer::MaxUrlCharacters)
		{
			OutReasonCode = TEXT("url-too-large");
			return false;
		}
		if (Input.Arguments.Num() > FHyperAIStudioLegacyProfileRecognizer::MaxArguments)
		{
			OutReasonCode = TEXT("too-many-arguments");
			return false;
		}

		if (HasEmbeddedNull(Input.ServerId)
			|| HasEmbeddedNull(Input.Transport)
			|| HasEmbeddedNull(Input.Command)
			|| HasEmbeddedNull(Input.Url))
		{
			OutReasonCode = TEXT("embedded-null");
			return false;
		}

		int64 TotalArgumentCharacters = 0;
		for (const FString& Argument : Input.Arguments)
		{
			if (Argument.Len() > FHyperAIStudioLegacyProfileRecognizer::MaxArgumentCharacters)
			{
				OutReasonCode = TEXT("argument-too-large");
				return false;
			}
			if (HasEmbeddedNull(Argument))
			{
				OutReasonCode = TEXT("embedded-null");
				return false;
			}
			TotalArgumentCharacters += Argument.Len();
			if (TotalArgumentCharacters > FHyperAIStudioLegacyProfileRecognizer::MaxTotalArgumentCharacters)
			{
				OutReasonCode = TEXT("arguments-total-too-large");
				return false;
			}
		}

		return true;
	}

	FString NormalizeServerIdForComparison(const FString& RawId)
	{
		FString Result;
		const FString Source = RawId.TrimStartAndEnd().ToLower();
		Result.Reserve(Source.Len());
		for (const TCHAR Character : Source)
		{
			if ((Character >= TEXT('a') && Character <= TEXT('z'))
				|| (Character >= TEXT('0') && Character <= TEXT('9'))
				|| Character == TEXT('-')
				|| Character == TEXT('_'))
			{
				Result.AppendChar(Character);
			}
			else if (Character == TEXT(' ') || Character == TEXT('.') || Character == TEXT('/'))
			{
				Result.AppendChar(TEXT('-'));
			}
		}
		return Result.IsEmpty() ? TEXT("custom-mcp") : Result;
	}

	bool IsReservedServerId(const FString& NormalizedServerId)
	{
		return NormalizedServerId == UnrealServerId
			|| NormalizedServerId == UnrealFullServerId
			|| NormalizedServerId == KnowledgeServerId;
	}

	FString NormalizePathForComparison(const FString& RawPath)
	{
		FString Result = RawPath.TrimStartAndEnd();
		Result.ReplaceInline(TEXT("\\"), TEXT("/"), ESearchCase::CaseSensitive);
		return Result;
	}

	bool PathEndsWithSegments(const FString& RawPath, const TCHAR* RawSuffix)
	{
		const FString Path = NormalizePathForComparison(RawPath);
		FString Suffix = NormalizePathForComparison(RawSuffix);
		while (Suffix.StartsWith(TEXT("/")))
		{
			Suffix.RightChopInline(1, EAllowShrinking::No);
		}

		return Path.Equals(Suffix, ESearchCase::IgnoreCase)
			|| Path.EndsWith(TEXT("/") + Suffix, ESearchCase::IgnoreCase);
	}

	uint64 StableComparisonFingerprint(const FString& RawValue)
	{
		const FString Value = NormalizePathForComparison(RawValue).ToLower();
		uint64 Fingerprint = 1469598103934665603ull;
		for (const TCHAR Character : Value)
		{
			Fingerprint ^= static_cast<uint64>(Character);
			Fingerprint *= 1099511628211ull;
		}
		return Fingerprint;
	}

	bool PathTailMatchesFingerprint(
		const FString& RawPath,
		const int32 SegmentCount,
		const uint64 ExpectedFingerprint)
	{
		TArray<FString> Segments;
		NormalizePathForComparison(RawPath).ParseIntoArray(Segments, TEXT("/"), true);
		if (SegmentCount <= 0 || Segments.Num() < SegmentCount)
		{
			return false;
		}
		TArray<FString> TailSegments;
		TailSegments.Reserve(SegmentCount);
		for (int32 Index = Segments.Num() - SegmentCount; Index < Segments.Num(); ++Index)
		{
			TailSegments.Add(Segments[Index]);
		}
		return StableComparisonFingerprint(FString::Join(TailSegments, TEXT("/")))
			== ExpectedFingerprint;
	}

	FString CommandBasename(const FString& RawCommand)
	{
		const FString Command = NormalizePathForComparison(RawCommand);
		int32 SeparatorIndex = INDEX_NONE;
		return Command.FindLastChar(TEXT('/'), SeparatorIndex)
			? Command.Mid(SeparatorIndex + 1)
			: Command;
	}

	bool IsPowerShellCommand(const FString& Command)
	{
		const FString Basename = CommandBasename(Command);
		return Basename.Equals(TEXT("powershell"), ESearchCase::IgnoreCase)
			|| Basename.Equals(TEXT("powershell.exe"), ESearchCase::IgnoreCase);
	}

	bool IsPythonCommand(const FString& Command)
	{
		const FString Basename = CommandBasename(Command);
		return Basename.Equals(TEXT("python"), ESearchCase::IgnoreCase)
			|| Basename.Equals(TEXT("python.exe"), ESearchCase::IgnoreCase);
	}

	bool IsCommandTransport(const FString& Transport)
	{
		const FString Normalized = Transport.TrimStartAndEnd().ToLower();
		return Normalized == TEXT("stdio") || Normalized == TEXT("command");
	}

	bool MatchesPowerShellLauncher(const FHyperAIStudioLegacyProfileInput& Input, const TCHAR* ScriptSuffix)
	{
		return IsPowerShellCommand(Input.Command)
			&& Input.Arguments.Num() == 5
			&& Input.Arguments[0].TrimStartAndEnd().Equals(TEXT("-NoProfile"), ESearchCase::IgnoreCase)
			&& Input.Arguments[1].TrimStartAndEnd().Equals(TEXT("-ExecutionPolicy"), ESearchCase::IgnoreCase)
			&& Input.Arguments[2].TrimStartAndEnd().Equals(TEXT("Bypass"), ESearchCase::IgnoreCase)
			&& Input.Arguments[3].TrimStartAndEnd().Equals(TEXT("-File"), ESearchCase::IgnoreCase)
			&& PathEndsWithSegments(Input.Arguments[4], ScriptSuffix);
	}

	bool MatchesSharedPythonWrapper(
		const FHyperAIStudioLegacyProfileInput& Input,
		const TCHAR* ManifestSuffix)
	{
		return IsPythonCommand(Input.Command)
			&& Input.Arguments.Num() == 3
			&& PathEndsWithSegments(
				Input.Arguments[0],
				TEXT("Resources/MCPServers/Shared/mcp_stdio_server.py"))
			&& Input.Arguments[1].TrimStartAndEnd().Equals(TEXT("--manifest"), ESearchCase::CaseSensitive)
			&& PathEndsWithSegments(Input.Arguments[2], ManifestSuffix);
	}

	bool MatchesFullManifestPythonWrapper(const FHyperAIStudioLegacyProfileInput& Input)
	{
		return IsPythonCommand(Input.Command)
			&& Input.Arguments.Num() == 5
			&& PathEndsWithSegments(
				Input.Arguments[0],
				TEXT("Resources/MCPServers/HyperUEMCPFull/RuntimeSource/hyper_ue_mcp_stdio.py"))
			&& Input.Arguments[1].TrimStartAndEnd().Equals(TEXT("--manifest"), ESearchCase::CaseSensitive)
			&& PathEndsWithSegments(
				Input.Arguments[2],
				TEXT("Resources/MCPServers/HyperUEMCPFull/RuntimeSource/tool_manifest.json"))
			&& Input.Arguments[3].TrimStartAndEnd().Equals(TEXT("--config"), ESearchCase::CaseSensitive)
			&& PathEndsWithSegments(
				Input.Arguments[4],
				TEXT("Resources/MCPServers/HyperUEMCPFull/RuntimeSource/config.json"));
	}

	FHyperAIStudioLegacyProfileMatch ExactMatch(
		const FString& NormalizedServerId,
		const EHyperAIStudioLegacyProfileKind Kind,
		const EHyperAIStudioLegacyProfileFamily Family,
		const TCHAR* Fingerprint)
	{
		FHyperAIStudioLegacyProfileMatch Match;
		Match.Classification = EHyperAIStudioLegacyProfileClassification::ExactOwned;
		Match.Kind = Kind;
		Match.Family = Family;
		Match.NormalizedServerId = NormalizedServerId;
		Match.Fingerprint = Fingerprint;
		Match.ReasonCode = TEXT("exact-owned-historical-fingerprint");
		Match.bUsesReservedId = true;
		Match.bExactBuiltInMatch = true;
		return Match;
	}
}

FHyperAIStudioLegacyProfileMatch FHyperAIStudioLegacyProfileRecognizer::Recognize(
	const FHyperAIStudioLegacyProfileInput& Input)
{
	using namespace HyperAIStudio::LegacyMigration::Private;

	FHyperAIStudioLegacyProfileMatch Match;
	if (!ValidateBoundedInput(Input, Match.ReasonCode))
	{
		Match.Classification = EHyperAIStudioLegacyProfileClassification::Malformed;
		return Match;
	}

	Match.NormalizedServerId = NormalizeServerIdForComparison(Input.ServerId);
	Match.bUsesReservedId = IsReservedServerId(Match.NormalizedServerId);
	if (!Match.bUsesReservedId)
	{
		Match.Classification = EHyperAIStudioLegacyProfileClassification::Foreign;
		Match.ReasonCode = TEXT("foreign-server-id");
		return Match;
	}

	if (!IsCommandTransport(Input.Transport)
		|| !Input.Url.TrimStartAndEnd().IsEmpty()
		|| Input.bHasEnvironment
		|| Input.bHasUnrecognizedFields)
	{
		Match.Classification = EHyperAIStudioLegacyProfileClassification::CustomizedReservedId;
		Match.ReasonCode = TEXT("reserved-id-execution-shape-mismatch");
		return Match;
	}

	if (Match.NormalizedServerId == UnrealServerId)
	{
		if (Input.Arguments.IsEmpty()
			&& PathTailMatchesFingerprint(
				Input.Command, 3, HistoricalDirectExecutableTailFingerprint))
		{
			return ExactMatch(
				Match.NormalizedServerId,
				EHyperAIStudioLegacyProfileKind::DirectHistoricalExecutable,
				EHyperAIStudioLegacyProfileFamily::Unreal,
				TEXT("hyperai.legacy.unreal.direct-executable.v2"));
		}

		if (MatchesPowerShellLauncher(
			Input,
			TEXT("Resources/MCPServers/HyperUEMCP/Start-HyperUEMCP.ps1")))
		{
			return ExactMatch(
				Match.NormalizedServerId,
				EHyperAIStudioLegacyProfileKind::PowerShellHyperUEMCPLauncher,
				EHyperAIStudioLegacyProfileFamily::Unreal,
				TEXT("hyperai.legacy.unreal.powershell-wrapper.v1"));
		}

		if (MatchesPowerShellLauncher(
			Input,
			TEXT("Resources/MCPServers/HyperUEMCPFull/Start-HyperUEMCPFull.ps1")))
		{
			return ExactMatch(
				Match.NormalizedServerId,
				EHyperAIStudioLegacyProfileKind::PowerShellHyperUEMCPFullLauncher,
				EHyperAIStudioLegacyProfileFamily::Unreal,
				TEXT("hyperai.legacy.unreal.powershell-full-wrapper.v1"));
		}

		if (MatchesSharedPythonWrapper(
			Input,
			TEXT("Resources/MCPServers/HyperUEMCP/manifest.json")))
		{
			return ExactMatch(
				Match.NormalizedServerId,
				EHyperAIStudioLegacyProfileKind::SharedPythonUnrealWrapper,
				EHyperAIStudioLegacyProfileFamily::Unreal,
				TEXT("hyperai.legacy.unreal.shared-python-wrapper.v1"));
		}

		if (MatchesFullManifestPythonWrapper(Input))
		{
			return ExactMatch(
				Match.NormalizedServerId,
				EHyperAIStudioLegacyProfileKind::FullManifestPythonWrapper,
				EHyperAIStudioLegacyProfileFamily::Unreal,
				TEXT("hyperai.legacy.unreal.full-manifest-wrapper.v1"));
		}
	}
	else if (Match.NormalizedServerId == UnrealFullServerId)
	{
		if (MatchesFullManifestPythonWrapper(Input))
		{
			return ExactMatch(
				Match.NormalizedServerId,
				EHyperAIStudioLegacyProfileKind::FullManifestPythonWrapper,
				EHyperAIStudioLegacyProfileFamily::Unreal,
				TEXT("hyperai.legacy.unreal.full-manifest-wrapper.v1"));
		}
	}
	else if (Match.NormalizedServerId == KnowledgeServerId)
	{
		if (MatchesPowerShellLauncher(
			Input,
			TEXT("Resources/MCPServers/HyperKnowledgeMCP/Start-HyperKnowledgeMCP.ps1")))
		{
			return ExactMatch(
				Match.NormalizedServerId,
				EHyperAIStudioLegacyProfileKind::PowerShellHyperKnowledgeLauncher,
				EHyperAIStudioLegacyProfileFamily::Knowledge,
				TEXT("hyperai.legacy.knowledge.powershell-wrapper.v1"));
		}

		if (MatchesSharedPythonWrapper(
			Input,
			TEXT("Resources/MCPServers/HyperKnowledgeMCP/manifest.json")))
		{
			return ExactMatch(
				Match.NormalizedServerId,
				EHyperAIStudioLegacyProfileKind::SharedPythonKnowledgeWrapper,
				EHyperAIStudioLegacyProfileFamily::Knowledge,
				TEXT("hyperai.legacy.knowledge.shared-python-wrapper.v1"));
		}
	}

	Match.Classification = EHyperAIStudioLegacyProfileClassification::CustomizedReservedId;
	Match.ReasonCode = TEXT("reserved-id-fingerprint-mismatch");
	return Match;
}

bool FHyperAIStudioLegacyProfileRecognizer::IsExactOwnedRetiredProfile(
	const FHyperAIStudioLegacyProfileMatch& Match)
{
	return Match.Classification == EHyperAIStudioLegacyProfileClassification::ExactOwned
		&& Match.bExactBuiltInMatch
		&& (Match.Family == EHyperAIStudioLegacyProfileFamily::Unreal
			|| Match.Family == EHyperAIStudioLegacyProfileFamily::Knowledge);
}

TArray<FString> FHyperAIStudioLegacyProfileRecognizer::GetHistoricalJsonClientIdsForCleanup(
	const FHyperAIStudioLegacyProfileMatch& Match)
{
	if (!IsExactOwnedRetiredProfile(Match))
	{
		return {};
	}

	// The retired writer exported every enabled extra server to every enabled JSON
	// client and did not apply TargetClients. Codex is intentionally absent because
	// its historical entries are retired by rebuilding the owned TOML managed block.
	return {
		TEXT("claude"),
		TEXT("cursor"),
		TEXT("vscode"),
		TEXT("gemini")
	};
}
