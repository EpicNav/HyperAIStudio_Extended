// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"

struct FHyperAIStudioCapabilityInventoryResult;

enum class EHyperAIStudioToolInventorySource : uint8
{
	Epic,
	HyperAI
};

struct FHyperAIStudioToolInventoryRow
{
	FString Name;
	FString QualifiedName;
	FString Toolset;
	FString Description;
	FString Capability;
	EHyperAIStudioToolInventorySource Source = EHyperAIStudioToolInventorySource::Epic;
	bool bAvailableNow = false;
};

struct FHyperAIStudioToolInventoryView
{
	static constexpr int32 KnownEpicUE58DeclarationCount = 876;

	bool bSuccess = false;
	bool bExactLiveInventory = false;
	bool bExtendedHyperToolsEnabled = false;
	int32 DispatcherCount = 0;
	int32 LiveToolsetCount = 0;
	int32 LiveEpicToolsetCount = 0;
	int32 LiveHyperToolsetCount = 0;
	int32 LiveEpicToolCount = 0;
	int32 AvailableHyperToolCount = 0;
	FString Message;
	TArray<FHyperAIStudioToolInventoryRow> Rows;

	int32 GetAvailableToolCount() const
	{
		return LiveEpicToolCount + AvailableHyperToolCount;
	}
};

/** Pure presentation model for the explicit Tools dialog. Never participates in the fast Ready path. */
class FHyperAIStudioToolInventoryViewModel final
{
public:
	static FHyperAIStudioToolInventoryView Build(const FHyperAIStudioCapabilityInventoryResult& EpicInventory);

	static bool Matches(
		const FHyperAIStudioToolInventoryRow& Row,
		const FString& Query,
		TOptional<EHyperAIStudioToolInventorySource> SourceFilter);
};
