// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"

/**
 * Historical HyperAIStudio command-profile implementations. A kind is assigned
 * only after the complete, bounded execution fingerprint matches.
 */
enum class EHyperAIStudioLegacyProfileKind : uint8
{
	None,
	DirectHistoricalExecutable,
	PowerShellHyperUEMCPLauncher,
	PowerShellHyperUEMCPFullLauncher,
	SharedPythonUnrealWrapper,
	FullManifestPythonWrapper,
	PowerShellHyperKnowledgeLauncher,
	SharedPythonKnowledgeWrapper
};

enum class EHyperAIStudioLegacyProfileFamily : uint8
{
	None,
	Unreal,
	Knowledge
};

/**
 * Ownership classification is deliberately separate from the historical kind.
 * A reserved id by itself produces CustomizedReservedId, never ExactOwned.
 */
enum class EHyperAIStudioLegacyProfileClassification : uint8
{
	ExactOwned,
	CustomizedReservedId,
	Foreign,
	Malformed
};

/**
 * Pure projection of the executable portion of an MCP server profile.
 *
 * Callers parsing a richer representation must set bHasEnvironment when an
 * environment field exists (including an empty object), and must set
 * bHasUnrecognizedFields for fields outside the historical schema being parsed.
 * Metadata such as display labels and documentation is intentionally not
 * ownership evidence.
 */
struct FHyperAIStudioLegacyProfileInput
{
	FString ServerId;
	FString Transport;
	FString Command;
	TArray<FString> Arguments;
	FString Url;
	bool bHasEnvironment = false;
	bool bHasUnrecognizedFields = false;
	bool bStructurallyMalformed = false;
};

struct FHyperAIStudioLegacyProfileMatch
{
	EHyperAIStudioLegacyProfileClassification Classification = EHyperAIStudioLegacyProfileClassification::Foreign;
	EHyperAIStudioLegacyProfileKind Kind = EHyperAIStudioLegacyProfileKind::None;
	EHyperAIStudioLegacyProfileFamily Family = EHyperAIStudioLegacyProfileFamily::None;
	FString NormalizedServerId;
	FString Fingerprint;
	FString ReasonCode;
	bool bUsesReservedId = false;
	bool bExactBuiltInMatch = false;
};

/**
 * Read-only, deterministic recognizer for historical HyperAIStudio profiles.
 * It performs no filesystem, settings, process, or configuration work.
 */
class FHyperAIStudioLegacyProfileRecognizer final
{
public:
	static constexpr int32 MaxServerIdCharacters = 256;
	static constexpr int32 MaxTransportCharacters = 64;
	static constexpr int32 MaxCommandCharacters = 8192;
	static constexpr int32 MaxUrlCharacters = 8192;
	static constexpr int32 MaxArgumentCharacters = 8192;
	static constexpr int32 MaxArguments = 16;
	static constexpr int32 MaxTotalArgumentCharacters = 32768;

	static FHyperAIStudioLegacyProfileMatch Recognize(const FHyperAIStudioLegacyProfileInput& Input);
	static bool IsExactOwnedRetiredProfile(const FHyperAIStudioLegacyProfileMatch& Match);
	static TArray<FString> GetHistoricalJsonClientIdsForCleanup(
		const FHyperAIStudioLegacyProfileMatch& Match);
};
