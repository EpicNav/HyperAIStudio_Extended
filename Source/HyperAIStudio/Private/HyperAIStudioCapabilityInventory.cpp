// Games by Hyper 2026.

#include "HyperAIStudioCapabilityInventory.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/SecureHash.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace HyperAIStudio::CapabilityInventory::Private
{
	const TArray<FString>& RequiredDispatchers()
	{
		static const TArray<FString> Names = {
			TEXT("list_toolsets"),
			TEXT("describe_toolset"),
			TEXT("call_tool")
		};
		return Names;
	}

	bool IsQualifiedToolsetName(const FString& Name)
	{
		if (Name.IsEmpty() || Name.StartsWith(TEXT(".")) || Name.EndsWith(TEXT(".")))
		{
			return false;
		}

		bool bHasSeparator = false;
		bool bPreviousWasSeparator = false;
		for (const TCHAR Character : Name)
		{
			if (Character == TEXT('.'))
			{
				if (bPreviousWasSeparator)
				{
					return false;
				}
				bHasSeparator = true;
				bPreviousWasSeparator = true;
				continue;
			}
			if (!FChar::IsAlnum(Character) && Character != TEXT('_'))
			{
				return false;
			}
			bPreviousWasSeparator = false;
		}
		return bHasSeparator;
	}

	bool FitsUtf8Bound(const FString& Value, const int32 MaxBytes)
	{
		const FTCHARToUTF8 Converted(*Value);
		return Converted.Length() <= MaxBytes;
	}

	bool DeserializeObject(const FString& JsonText, TSharedPtr<FJsonObject>& OutObject)
	{
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
		return FJsonSerializer::Deserialize(Reader, OutObject) && OutObject.IsValid();
	}

	FString SerializePrimitive(const TSharedPtr<FJsonValue>& Value)
	{
		FString Serialized;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Serialized);
		FJsonSerializer::Serialize(Value, FString(), Writer);
		return Serialized;
	}
}

bool FHyperAIStudioCapabilityInventoryParser::ValidateResponseByteLengths(
	const uint64 DeclaredContentBytes,
	const int64 BufferedContentBytes,
	FString& OutError)
{
	OutError.Reset();
	if (BufferedContentBytes < 0)
	{
		OutError = TEXT("MCP HTTP response reported an invalid buffered byte count.");
		return false;
	}
	if (DeclaredContentBytes > static_cast<uint64>(MaxResponseBytes)
		|| BufferedContentBytes > MaxResponseBytes)
	{
		OutError = TEXT("MCP HTTP response exceeded the 2 MiB inventory bound.");
		return false;
	}
	return true;
}

bool FHyperAIStudioCapabilityInventoryParser::ParseToolsListResponse(
	const FString& ResponseBody,
	const int64 ExpectedRequestId,
	FHyperAIStudioCapabilitySnapshot& OutSnapshot,
	FString& OutError)
{
	OutSnapshot = {};
	OutError.Reset();
	TSharedPtr<FJsonObject> Envelope;
	if (!ParseJsonRpcEnvelope(ResponseBody, ExpectedRequestId, Envelope, OutError))
	{
		OutSnapshot.ProbeError = OutError;
		return false;
	}

	const TSharedPtr<FJsonObject>* Result = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Tools = nullptr;
	if (!Envelope->TryGetObjectField(TEXT("result"), Result)
		|| !Result
		|| !Result->IsValid()
		|| !(*Result)->TryGetArrayField(TEXT("tools"), Tools)
		|| !Tools)
	{
		OutError = TEXT("tools/list JSON-RPC result.tools array is missing.");
		OutSnapshot.ProbeError = OutError;
		return false;
	}

	if ((*Result)->HasField(TEXT("nextCursor")))
	{
		if (!(*Result)->TryGetStringField(TEXT("nextCursor"), OutSnapshot.NextCursor))
		{
			OutError = TEXT("tools/list result.nextCursor must be a string when present.");
			OutSnapshot.ProbeError = OutError;
			return false;
		}
		OutSnapshot.NextCursor.TrimStartAndEndInline();
		OutSnapshot.bTruncated = !OutSnapshot.NextCursor.IsEmpty();
	}

	TMap<FString, FString> SeenCaseFolded;
	TSet<FString> ExactNames;
	for (const TSharedPtr<FJsonValue>& ToolValue : *Tools)
	{
		const TSharedPtr<FJsonObject>* ToolObject = nullptr;
		FString Name;
		if (!ToolValue.IsValid()
			|| !ToolValue->TryGetObject(ToolObject)
			|| !ToolObject
			|| !ToolObject->IsValid()
			|| !(*ToolObject)->TryGetStringField(TEXT("name"), Name)
			|| Name.IsEmpty())
		{
			OutError = TEXT("tools/list contains a tool without a valid name.");
			OutSnapshot.ProbeError = OutError;
			return false;
		}

		const FString CaseFoldedName = Name.ToLower();
		if (const FString* ExistingName = SeenCaseFolded.Find(CaseFoldedName))
		{
			OutError = ExistingName->Equals(Name, ESearchCase::CaseSensitive)
				? FString::Printf(TEXT("tools/list contains duplicate tool name '%s'."), *Name)
				: FString::Printf(TEXT("tools/list contains case-insensitive tool-name collision '%s' / '%s'."), **ExistingName, *Name);
			OutSnapshot.ProbeError = OutError;
			return false;
		}
		SeenCaseFolded.Add(CaseFoldedName, Name);
		ExactNames.Add(Name);

		if (OutSnapshot.TopLevelToolNames.Num() < MaxTopLevelTools)
		{
			OutSnapshot.TopLevelToolNames.Add(Name);
		}
		else
		{
			// Continue scanning so dispatcher classification and duplicate detection stay correct.
			OutSnapshot.bTruncated = true;
		}
	}
	OutSnapshot.TopLevelToolCount = SeenCaseFolded.Num();

	int32 DispatcherCount = 0;
	for (const FString& Dispatcher : HyperAIStudio::CapabilityInventory::Private::RequiredDispatchers())
	{
		if (ExactNames.Contains(Dispatcher))
		{
			++DispatcherCount;
		}
		else
		{
			OutSnapshot.MissingDispatchers.Add(Dispatcher);
		}
	}

	if (DispatcherCount == HyperAIStudio::CapabilityInventory::Private::RequiredDispatchers().Num())
	{
		OutSnapshot.DiscoveryMode = EHyperAIStudioToolDiscoveryMode::ToolSearch;
	}
	else if (DispatcherCount == 0 && OutSnapshot.TopLevelToolCount > 0 && OutSnapshot.NextCursor.IsEmpty())
	{
		OutSnapshot.DiscoveryMode = EHyperAIStudioToolDiscoveryMode::Eager;
		OutSnapshot.MissingDispatchers.Reset();
	}
	else if (DispatcherCount == 0)
	{
		OutSnapshot.DiscoveryMode = EHyperAIStudioToolDiscoveryMode::Degraded;
		OutSnapshot.ProbeError = TEXT("tools/list returned no top-level tools; discovery mode is not provable.");
	}
	else
	{
		OutSnapshot.DiscoveryMode = EHyperAIStudioToolDiscoveryMode::Degraded;
		OutSnapshot.ProbeError = FString::Printf(
			TEXT("Tool-search contract is partial; missing: %s."),
			*FString::Join(OutSnapshot.MissingDispatchers, TEXT(", ")));
	}

	if (!OutSnapshot.NextCursor.IsEmpty())
	{
		const FString PaginationMessage = FString::Printf(
			TEXT("tools/list is incomplete; the server returned nextCursor '%s'."),
			*OutSnapshot.NextCursor);
		if (OutSnapshot.DiscoveryMode == EHyperAIStudioToolDiscoveryMode::ToolSearch)
		{
			OutSnapshot.ProbeError = PaginationMessage;
		}
		else
		{
			OutSnapshot.DiscoveryMode = EHyperAIStudioToolDiscoveryMode::Degraded;
			OutSnapshot.ProbeError = PaginationMessage;
		}
	}

	return true;
}

bool FHyperAIStudioCapabilityInventoryParser::ParseListToolsetsResponse(
	const FString& ResponseBody,
	const int64 ExpectedRequestId,
	TArray<FHyperAIStudioDiscoveredToolset>& OutToolsets,
	bool& bOutTruncated,
	int32& OutMalformedLineCount,
	FString& OutError)
{
	OutToolsets.Reset();
	bOutTruncated = false;
	OutMalformedLineCount = 0;
	OutError.Reset();

	FString Text;
	if (!ExtractToolText(ResponseBody, ExpectedRequestId, MaxResponseBytes, Text, OutError))
	{
		return false;
	}

	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines, false);
	TSet<FString> Seen;
	for (FString Line : Lines)
	{
		// Epic descriptions can contain indented prose and nested "- Action: detail" rows.
		// Only column-zero, registry-qualified Module.Toolset rows belong to this list.
		Line.TrimEndInline();
		if (!Line.StartsWith(TEXT("- ")))
		{
			continue;
		}

		Line.RightChopInline(2, EAllowShrinking::No);
		int32 ColonIndex = INDEX_NONE;
		FString Name;
		FString Description;
		if (Line.FindChar(TEXT(':'), ColonIndex))
		{
			Name = Line.Left(ColonIndex).TrimStartAndEnd();
			Description = Line.Mid(ColonIndex + 1).TrimStartAndEnd();
		}
		else
		{
			// Epic may emit "- ToolsetName" when no description is registered.
			Name = Line.TrimStartAndEnd();
		}
		if (Name.IsEmpty())
		{
			++OutMalformedLineCount;
			continue;
		}
		if (!HyperAIStudio::CapabilityInventory::Private::IsQualifiedToolsetName(Name))
		{
			continue;
		}
		if (Seen.Contains(Name))
		{
			continue;
		}
		if (OutToolsets.Num() >= MaxToolsets)
		{
			bOutTruncated = true;
			break;
		}

		Seen.Add(Name);
		OutToolsets.Add({ MoveTemp(Name), MoveTemp(Description) });
	}

	if (OutToolsets.IsEmpty())
	{
		OutError = OutMalformedLineCount > 0
			? TEXT("list_toolsets contained only malformed toolset rows.")
			: TEXT("list_toolsets returned no parseable toolset rows.");
		return false;
	}
	return true;
}

bool FHyperAIStudioCapabilityInventoryParser::ParseDescribeToolsetResponse(
	const FString& ResponseBody,
	const int64 ExpectedRequestId,
	FHyperAIStudioDescribedToolset& OutToolset,
	FString& OutError)
{
	OutToolset = {};
	OutError.Reset();
	FString SchemaText;
	if (!ExtractToolText(ResponseBody, ExpectedRequestId, MaxSchemaBytes, SchemaText, OutError))
	{
		return false;
	}

	TSharedPtr<FJsonObject> Schema;
	if (!HyperAIStudio::CapabilityInventory::Private::DeserializeObject(SchemaText, Schema))
	{
		OutError = TEXT("describe_toolset content is not a valid JSON object schema.");
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Tools = nullptr;
	if (!Schema->TryGetStringField(TEXT("name"), OutToolset.Name)
		|| OutToolset.Name.IsEmpty()
		|| !Schema->TryGetArrayField(TEXT("tools"), Tools)
		|| !Tools)
	{
		OutError = TEXT("describe_toolset schema requires a name and tools array.");
		return false;
	}
	Schema->TryGetStringField(TEXT("version"), OutToolset.Version);
	Schema->TryGetStringField(TEXT("description"), OutToolset.Description);
	if (Tools->Num() > MaxToolsPerToolset)
	{
		OutError = FString::Printf(
			TEXT("describe_toolset contains %d tools, exceeding the per-toolset bound of %d."),
			Tools->Num(),
			MaxToolsPerToolset);
		return false;
	}

	TMap<FString, FString> ToolNamesCaseFolded;
	OutToolset.Tools.Reserve(Tools->Num());
	for (const TSharedPtr<FJsonValue>& ToolValue : *Tools)
	{
		const TSharedPtr<FJsonObject>* ToolObject = nullptr;
		FString ToolName;
		if (!ToolValue.IsValid()
			|| !ToolValue->TryGetObject(ToolObject)
			|| !ToolObject
			|| !ToolObject->IsValid()
			|| !(*ToolObject)->TryGetStringField(TEXT("name"), ToolName)
			|| ToolName.IsEmpty())
		{
			OutError = TEXT("describe_toolset tools array contains an invalid tool schema.");
			return false;
		}
		if (!HyperAIStudio::CapabilityInventory::Private::FitsUtf8Bound(ToolName, MaxToolNameBytes))
		{
			OutError = FString::Printf(
				TEXT("describe_toolset tool name exceeds the %d-byte bound."),
				MaxToolNameBytes);
			return false;
		}
		FString ToolDescription;
		const TSharedPtr<FJsonValue> ToolDescriptionValue =
			(*ToolObject)->TryGetField(TEXT("description"));
		if (ToolDescriptionValue.IsValid()
			&& (ToolDescriptionValue->Type != EJson::String
				|| !ToolDescriptionValue->TryGetString(ToolDescription)))
		{
			OutError = FString::Printf(
				TEXT("describe_toolset tool '%s' has a non-string description."),
				*ToolName);
			return false;
		}
		if (!HyperAIStudio::CapabilityInventory::Private::FitsUtf8Bound(
			ToolDescription,
			MaxToolDescriptionBytes))
		{
			OutError = FString::Printf(
				TEXT("describe_toolset tool '%s' description exceeds the %d-byte bound."),
				*ToolName,
				MaxToolDescriptionBytes);
			return false;
		}
		const FString CaseFoldedName = ToolName.ToLower();
		if (const FString* ExistingName = ToolNamesCaseFolded.Find(CaseFoldedName))
		{
			OutError = ExistingName->Equals(ToolName, ESearchCase::CaseSensitive)
				? FString::Printf(TEXT("describe_toolset contains duplicate tool name '%s'."), *ToolName)
				: FString::Printf(TEXT("describe_toolset contains case-insensitive tool-name collision '%s' / '%s'."), **ExistingName, *ToolName);
			return false;
		}
		ToolNamesCaseFolded.Add(CaseFoldedName, ToolName);
		OutToolset.Tools.Add({ MoveTemp(ToolName), MoveTemp(ToolDescription) });
	}
	OutToolset.ToolCount = OutToolset.Tools.Num();

	FString Canonical;
	if (!CanonicalizeJson(SchemaText, Canonical, OutError))
	{
		return false;
	}
	OutToolset.SchemaHash = HashCanonicalJson(Canonical);
	return true;
}

bool FHyperAIStudioCapabilityInventoryParser::CanonicalizeJson(
	const FString& JsonText,
	FString& OutCanonicalJson,
	FString& OutError)
{
	OutCanonicalJson.Reset();
	OutError.Reset();
	TSharedPtr<FJsonValue> RootValue;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
	if (!FJsonSerializer::Deserialize(Reader, RootValue) || !RootValue.IsValid())
	{
		OutError = TEXT("JSON value could not be parsed for canonicalization.");
		return false;
	}
	return CanonicalizeValue(RootValue, 0, OutCanonicalJson, OutError);
}

bool FHyperAIStudioCapabilityInventoryParser::ParseJsonRpcResult(
	const FString& ResponseBody,
	const int64 ExpectedRequestId,
	TSharedPtr<FJsonObject>& OutResult,
	FString& OutError)
{
	OutResult.Reset();
	TSharedPtr<FJsonObject> Envelope;
	if (!ParseJsonRpcEnvelope(ResponseBody, ExpectedRequestId, Envelope, OutError))
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* Result = nullptr;
	if (!Envelope->TryGetObjectField(TEXT("result"), Result) || !Result || !Result->IsValid())
	{
		OutError = TEXT("MCP JSON-RPC response result is not an object.");
		return false;
	}
	OutResult = *Result;
	return true;
}

bool FHyperAIStudioCapabilityInventoryParser::ParseJsonRpcEnvelope(
	const FString& ResponseBody,
	const int64 ExpectedRequestId,
	TSharedPtr<FJsonObject>& OutEnvelope,
	FString& OutError)
{
	OutEnvelope.Reset();
	OutError.Reset();
	if (!HyperAIStudio::CapabilityInventory::Private::FitsUtf8Bound(ResponseBody, MaxResponseBytes))
	{
		OutError = TEXT("MCP response exceeded the 2 MiB inventory bound.");
		return false;
	}

	TArray<TSharedPtr<FJsonObject>> Events;
	const FString Trimmed = ResponseBody.TrimStartAndEnd();
	if (Trimmed.StartsWith(TEXT("{")))
	{
		TSharedPtr<FJsonObject> Event;
		if (!HyperAIStudio::CapabilityInventory::Private::DeserializeObject(Trimmed, Event))
		{
			OutError = TEXT("MCP response is not valid JSON.");
			return false;
		}
		Events.Add(MoveTemp(Event));
	}
	else
	{
		TArray<FString> Lines;
		ResponseBody.ParseIntoArrayLines(Lines, false);
		Lines.Add(FString());
		FString EventData;
		bool bHasEventData = false;
		for (FString Line : Lines)
		{
			if (Line.IsEmpty())
			{
				if (!bHasEventData)
				{
					continue;
				}
				TSharedPtr<FJsonObject> Event;
				if (!HyperAIStudio::CapabilityInventory::Private::DeserializeObject(EventData, Event))
				{
					OutError = TEXT("SSE response contains a malformed JSON data event.");
					return false;
				}
				Events.Add(MoveTemp(Event));
				EventData.Reset();
				bHasEventData = false;
				continue;
			}

			FString TrimmedLine = Line.TrimStart();
			if (TrimmedLine.StartsWith(TEXT("data:")))
			{
				if (bHasEventData)
				{
					EventData += TEXT("\n");
				}
				EventData += TrimmedLine.Mid(5).TrimStart();
				bHasEventData = true;
			}
		}
		if (Events.IsEmpty())
		{
			OutError = TEXT("SSE response contained no JSON data event.");
			return false;
		}
	}

	for (const TSharedPtr<FJsonObject>& Event : Events)
	{
		FString JsonRpcVersion;
		if (!Event.IsValid()
			|| !Event->TryGetStringField(TEXT("jsonrpc"), JsonRpcVersion)
			|| JsonRpcVersion != TEXT("2.0"))
		{
			OutError = TEXT("MCP response event does not declare jsonrpc '2.0'.");
			return false;
		}

		const bool bHasResult = Event->HasField(TEXT("result"));
		const bool bHasError = Event->HasField(TEXT("error"));
		if (!Event->HasField(TEXT("id")))
		{
			// Progress and other valid JSON-RPC notifications are not request responses.
			if (Event->HasField(TEXT("method")) && !bHasResult && !bHasError)
			{
				continue;
			}
			OutError = TEXT("MCP JSON-RPC response is missing its request id.");
			return false;
		}

		const TSharedPtr<FJsonValue> IdValue = Event->TryGetField(TEXT("id"));
		double NumericId = 0.0;
		if (!IdValue.IsValid()
			|| IdValue->Type != EJson::Number
			|| !IdValue->TryGetNumber(NumericId)
			|| NumericId != static_cast<double>(ExpectedRequestId))
		{
			OutError = FString::Printf(
				TEXT("MCP response contains a stray or non-numeric request id; expected %lld."),
				ExpectedRequestId);
			return false;
		}
		if (bHasResult == bHasError)
		{
			OutError = TEXT("MCP JSON-RPC response must contain exactly one of result or error.");
			return false;
		}
		if (OutEnvelope.IsValid())
		{
			OutError = FString::Printf(TEXT("MCP response contains duplicate results for request id %lld."), ExpectedRequestId);
			return false;
		}
		if (bHasError)
		{
			OutError = FString::Printf(TEXT("MCP JSON-RPC request %lld returned an error object."), ExpectedRequestId);
			return false;
		}
		OutEnvelope = Event;
	}

	if (!OutEnvelope.IsValid())
	{
		OutError = FString::Printf(TEXT("MCP response contained no result for request id %lld."), ExpectedRequestId);
		return false;
	}
	return true;
}

bool FHyperAIStudioCapabilityInventoryParser::ExtractToolText(
	const FString& ResponseBody,
	const int64 ExpectedRequestId,
	const int32 MaxBytes,
	FString& OutText,
	FString& OutError)
{
	OutText.Reset();
	TSharedPtr<FJsonObject> Envelope;
	if (!ParseJsonRpcEnvelope(ResponseBody, ExpectedRequestId, Envelope, OutError))
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* Result = nullptr;
	if (!Envelope->TryGetObjectField(TEXT("result"), Result) || !Result || !Result->IsValid())
	{
		OutError = TEXT("tools/call response has no result object.");
		return false;
	}
	bool bIsError = false;
	if ((*Result)->HasField(TEXT("isError")) && !(*Result)->TryGetBoolField(TEXT("isError"), bIsError))
	{
		OutError = TEXT("tools/call result.isError must be a boolean when present.");
		return false;
	}
	if (bIsError)
	{
		OutError = TEXT("tools/call returned isError=true.");
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Content = nullptr;
	if (!(*Result)->TryGetArrayField(TEXT("content"), Content) || !Content)
	{
		OutError = TEXT("tools/call result.content array is missing.");
		return false;
	}

	for (const TSharedPtr<FJsonValue>& ContentValue : *Content)
	{
		const TSharedPtr<FJsonObject>* ContentObject = nullptr;
		FString Type;
		FString Text;
		if (ContentValue.IsValid()
			&& ContentValue->TryGetObject(ContentObject)
			&& ContentObject
			&& ContentObject->IsValid()
			&& (*ContentObject)->TryGetStringField(TEXT("type"), Type)
			&& Type == TEXT("text")
			&& (*ContentObject)->TryGetStringField(TEXT("text"), Text))
		{
			if (!OutText.IsEmpty())
			{
				OutText += LINE_TERMINATOR;
			}
			OutText += Text;
			if (!HyperAIStudio::CapabilityInventory::Private::FitsUtf8Bound(OutText, MaxBytes))
			{
				OutText.Reset();
				OutError = TEXT("MCP text content exceeded its inventory bound.");
				return false;
			}
		}
	}

	if (OutText.IsEmpty())
	{
		OutError = TEXT("tools/call returned no readable text content.");
		return false;
	}
	return true;
}

bool FHyperAIStudioCapabilityInventoryParser::CanonicalizeValue(
	const TSharedPtr<FJsonValue>& Value,
	const int32 Depth,
	FString& OutCanonicalJson,
	FString& OutError)
{
	if (!Value.IsValid())
	{
		OutError = TEXT("Canonical JSON contains an invalid value.");
		return false;
	}
	if (Depth > MaxCanonicalJsonDepth)
	{
		OutError = FString::Printf(TEXT("Canonical JSON exceeds the maximum nesting depth of %d."), MaxCanonicalJsonDepth);
		return false;
	}

	switch (Value->Type)
	{
	case EJson::Object:
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (!Value->TryGetObject(Object) || !Object || !Object->IsValid())
		{
			OutError = TEXT("Canonical JSON object is invalid.");
			return false;
		}
		struct FCanonicalMember
		{
			FString Key;
			TSharedPtr<FJsonValue> MemberValue;
		};
		TArray<FCanonicalMember> Members;
		Members.Reserve((*Object)->Values.Num());
		for (const auto& Pair : (*Object)->Values)
		{
			Members.Add({ FString(Pair.Key.Len(), *Pair.Key), Pair.Value });
		}
		Members.Sort([](const FCanonicalMember& Left, const FCanonicalMember& Right)
		{
			return Left.Key.Compare(Right.Key, ESearchCase::CaseSensitive) < 0;
		});
		OutCanonicalJson += TEXT("{");
		for (int32 Index = 0; Index < Members.Num(); ++Index)
		{
			if (Index > 0)
			{
				OutCanonicalJson += TEXT(",");
			}
			OutCanonicalJson += HyperAIStudio::CapabilityInventory::Private::SerializePrimitive(MakeShared<FJsonValueString>(Members[Index].Key));
			OutCanonicalJson += TEXT(":");
			if (!CanonicalizeValue(Members[Index].MemberValue, Depth + 1, OutCanonicalJson, OutError))
			{
				return false;
			}
		}
		OutCanonicalJson += TEXT("}");
		return true;
	}
	case EJson::Array:
	{
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (!Value->TryGetArray(Array) || !Array)
		{
			OutError = TEXT("Canonical JSON array is invalid.");
			return false;
		}
		OutCanonicalJson += TEXT("[");
		for (int32 Index = 0; Index < Array->Num(); ++Index)
		{
			if (Index > 0)
			{
				OutCanonicalJson += TEXT(",");
			}
			if (!CanonicalizeValue((*Array)[Index], Depth + 1, OutCanonicalJson, OutError))
			{
				return false;
			}
		}
		OutCanonicalJson += TEXT("]");
		return true;
	}
	case EJson::String:
	case EJson::Number:
	case EJson::Boolean:
	case EJson::Null:
		OutCanonicalJson += HyperAIStudio::CapabilityInventory::Private::SerializePrimitive(Value);
		return true;
	default:
		OutError = TEXT("Canonical JSON contains an unsupported value type.");
		return false;
	}
}

FString FHyperAIStudioCapabilityInventoryParser::HashCanonicalJson(const FString& CanonicalJson)
{
	const FTCHARToUTF8 Utf8(*CanonicalJson);
	uint8 Digest[FSHA1::DigestSize];
	FSHA1::HashBuffer(Utf8.Get(), Utf8.Length(), Digest);
	FString Hex;
	Hex.Reserve(FSHA1::DigestSize * 2 + 5);
	Hex = TEXT("sha1:");
	for (const uint8 Byte : Digest)
	{
		Hex += FString::Printf(TEXT("%02x"), Byte);
	}
	return Hex;
}
