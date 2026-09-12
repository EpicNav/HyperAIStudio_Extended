// Games by Hyper 2026.

#include "HyperAIStudioToolInventoryViewModel.h"

#include "HyperAIStudioCapabilityInventoryClient.h"
#include "HyperAIStudioCapabilityPackRegistry.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioSettings.h"

namespace HyperAIStudio::ToolInventoryViewModel::Private
{
	bool IsHyperAIToolset(const FString& Toolset)
	{
		return Toolset.StartsWith(TEXT("HyperAIStudio"), ESearchCase::CaseSensitive);
	}

	FString CleanOneLine(FString Value, const int32 MaxChars = 240)
	{
		Value.ReplaceInline(TEXT("\r"), TEXT(" "));
		Value.ReplaceInline(TEXT("\n"), TEXT(" "));
		Value.TrimStartAndEndInline();
		return Value.Left(MaxChars);
	}

	FString ShortName(const FString& QualifiedName)
	{
		int32 Separator = INDEX_NONE;
		return QualifiedName.FindLastChar(TEXT('.'), Separator)
			? QualifiedName.Mid(Separator + 1)
			: QualifiedName;
	}

	FString Humanize(FString Value)
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

	FString HyperCapability(const FString& ToolName)
	{
		static const TSet<FString> DirectFastEdits = {
			TEXT("hyper_data_apply_plan"),
			TEXT("hyper_input_apply_plan"),
			TEXT("hyper_paper2d_apply_plan")};
		if (DirectFastEdits.Contains(ToolName))
		{
			return TEXT("Direct edit (limited)");
		}
		if (ToolName.EndsWith(TEXT("_apply_plan"), ESearchCase::CaseSensitive))
		{
			return TEXT("Plan / preflight");
		}
		if (ToolName.Contains(TEXT("validate"), ESearchCase::IgnoreCase))
		{
			return TEXT("Validate");
		}
		if (ToolName.Contains(TEXT("inspect"), ESearchCase::IgnoreCase)
			|| ToolName.Contains(TEXT("read"), ESearchCase::IgnoreCase)
			|| ToolName.Contains(TEXT("status"), ESearchCase::IgnoreCase)
			|| ToolName.Contains(TEXT("report"), ESearchCase::IgnoreCase)
			|| ToolName.Contains(TEXT("search"), ESearchCase::IgnoreCase))
		{
			return TEXT("Read");
		}
		return TEXT("Action");
	}
}

FHyperAIStudioToolInventoryView FHyperAIStudioToolInventoryViewModel::Build(
	const FHyperAIStudioCapabilityInventoryResult& EpicInventory)
{
	using namespace HyperAIStudio::ToolInventoryViewModel::Private;

	FHyperAIStudioToolInventoryView View;
	View.bSuccess = EpicInventory.bSuccess;
	View.bExactLiveInventory = EpicInventory.bSuccess
		&& EpicInventory.Snapshot.bDetailedInventoryComplete;
	View.DispatcherCount = EpicInventory.Snapshot.TopLevelToolCount;
	View.LiveToolsetCount = EpicInventory.Snapshot.DiscoverableToolsetCount;
	View.Message = EpicInventory.Message;

	for (const FHyperAIStudioDiscoveredToolset& Toolset : EpicInventory.Snapshot.Toolsets)
	{
		if (IsHyperAIToolset(Toolset.Name))
		{
			++View.LiveHyperToolsetCount;
		}
		else
		{
			++View.LiveEpicToolsetCount;
		}
	}

	for (const FHyperAIStudioDescribedToolset& Toolset : EpicInventory.Snapshot.DescribedToolsets)
	{
		if (IsHyperAIToolset(Toolset.Name))
		{
			continue;
		}
		View.LiveEpicToolCount += Toolset.Tools.Num();
		for (const FHyperAIStudioDescribedTool& Tool : Toolset.Tools)
		{
			FHyperAIStudioToolInventoryRow Row;
			Row.Name = ShortName(Tool.Name);
			Row.QualifiedName = Tool.Name;
			Row.Toolset = Toolset.Name;
			Row.Description = CleanOneLine(Tool.Description);
			Row.Capability = TEXT("Epic tool");
			Row.Source = EHyperAIStudioToolInventorySource::Epic;
			Row.bAvailableNow = true;
			View.Rows.Add(MoveTemp(Row));
		}
	}

	const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
	View.bExtendedHyperToolsEnabled = !Settings
		|| Settings->NativeToolChannel == EHyperAIStudioNativeToolChannel::Preview;
	const FHyperAIStudioCapabilityCatalog& Catalog = FHyperAIStudioCapabilityPackRegistry::GetCatalog();
	TArray<FHyperAIStudioRuntimeToolBinding> RuntimeBindings;
	TArray<FString> RuntimeDiagnostics;
	FHyperAIStudioCapabilityRuntimeIndex::BuildSnapshot(RuntimeBindings, RuntimeDiagnostics);
	TMap<FString, const FHyperAIStudioRuntimeToolBinding*> RuntimeByName;
	for (const FHyperAIStudioRuntimeToolBinding& Binding : RuntimeBindings)
	{
		RuntimeByName.Add(Binding.ToolName, &Binding);
	}

	for (const FHyperAIStudioCapabilityToolDefinition& Tool : Catalog.Tools)
	{
		const FHyperAIStudioRuntimeToolBinding* Binding = RuntimeByName.FindRef(Tool.Name);
		const bool bAvailable = View.bExtendedHyperToolsEnabled
			&& Binding
			&& Binding->bToolEnabled;
		View.AvailableHyperToolCount += bAvailable ? 1 : 0;

		FHyperAIStudioToolInventoryRow Row;
		Row.Name = Tool.Name;
		Row.QualifiedName = Tool.Name;
		Row.Toolset = Binding ? Binding->QualifiedToolset : Tool.PackId;
		Row.Description = Binding ? CleanOneLine(Binding->Description) : FString();
		if (Row.Description.IsEmpty())
		{
			Row.Description = FString::Printf(TEXT("HyperAI %s capability."), *Humanize(Tool.PackId));
		}
		Row.Capability = HyperCapability(Tool.Name);
		Row.Source = EHyperAIStudioToolInventorySource::HyperAI;
		Row.bAvailableNow = bAvailable;
		View.Rows.Add(MoveTemp(Row));
	}

	View.Rows.Sort([](const FHyperAIStudioToolInventoryRow& Left, const FHyperAIStudioToolInventoryRow& Right)
	{
		if (Left.Source != Right.Source)
		{
			return Left.Source == EHyperAIStudioToolInventorySource::Epic;
		}
		const int32 ToolsetOrder = Left.Toolset.Compare(Right.Toolset, ESearchCase::CaseSensitive);
		return ToolsetOrder == 0
			? Left.Name.Compare(Right.Name, ESearchCase::CaseSensitive) < 0
			: ToolsetOrder < 0;
	});
	return View;
}

bool FHyperAIStudioToolInventoryViewModel::Matches(
	const FHyperAIStudioToolInventoryRow& Row,
	const FString& Query,
	const TOptional<EHyperAIStudioToolInventorySource> SourceFilter)
{
	if (SourceFilter.IsSet() && Row.Source != SourceFilter.GetValue())
	{
		return false;
	}
	const FString CleanQuery = Query.TrimStartAndEnd();
	return CleanQuery.IsEmpty()
		|| Row.Name.Contains(CleanQuery, ESearchCase::IgnoreCase)
		|| Row.QualifiedName.Contains(CleanQuery, ESearchCase::IgnoreCase)
		|| Row.Toolset.Contains(CleanQuery, ESearchCase::IgnoreCase)
		|| Row.Description.Contains(CleanQuery, ESearchCase::IgnoreCase)
		|| Row.Capability.Contains(CleanQuery, ESearchCase::IgnoreCase);
}
