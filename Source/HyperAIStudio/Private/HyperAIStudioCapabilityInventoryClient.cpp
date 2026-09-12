// Games by Hyper 2026.

#include "HyperAIStudioCapabilityInventoryClient.h"
#include "HyperAIStudioMcpSerialQueue.h"

#include "Dom/JsonObject.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/SecureHash.h"
#include "Misc/ScopeLock.h"
#include "ModelContextProtocol.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace HyperAIStudio::CapabilityInventoryClient::Private
{
	constexpr double CacheLifetimeSeconds = 30.0;
	constexpr double FailureCacheLifetimeSeconds = 10.0;
	constexpr double DescribedSchemaLifetimeSeconds = 5.0 * 60.0;
	constexpr double TotalSessionDeadlineSeconds = 30.0;
	constexpr double DetailedSessionDeadlineSeconds = 60.0;
	constexpr float RequestTimeoutSeconds = 5.0f;
	constexpr int32 MaxCacheEntries = 8;
	const FString CoreSentinelToolset = TEXT("editor_toolset.toolsets.scene.SceneTools");

	struct FCachedResult
	{
		FHyperAIStudioCapabilityInventoryResult Result;
		double CompletedSeconds = 0.0;
	};

	struct FCachedSchema
	{
		FHyperAIStudioDescribedToolset Toolset;
		double CompletedSeconds = 0.0;
	};

	struct FEndpointState
	{
		struct FDetailedState
		{
			uint64 CurrentGeneration = 0;
			bool bCurrentGenerationForced = false;
			TSet<uint64> OutstandingGenerations;
			TMap<uint64, TArray<FHyperAIStudioCapabilityInventoryClient::FCompletion>> WaitersByGeneration;
			TOptional<FCachedResult> LatestResult;
			TOptional<FCachedResult> LastSuccess;
		};

		uint64 CurrentGeneration = 0;
		bool bCurrentGenerationForced = false;
		TSet<uint64> OutstandingGenerations;
		TMap<uint64, TArray<FHyperAIStudioCapabilityInventoryClient::FCompletion>> WaitersByGeneration;
		TOptional<FCachedResult> LatestResult;
		TOptional<FCachedResult> LastSuccess;
		TMap<uint8, FDetailedState> DetailedStatesByScope;
		TMap<FString, FCachedSchema> Schemas;
		double LastAccessSeconds = 0.0;
	};

	FCriticalSection CacheMutex;
	TMap<FString, FEndpointState> StatesByEndpoint;

	bool IsAllowedEpicEndpoint(const FString& Endpoint, FString& OutEndpointKey)
	{
		OutEndpointKey = FHyperAIStudioMcpSerialQueue::CanonicalizeEndpointKey(Endpoint);
		return OutEndpointKey.StartsWith(TEXT("http://127.0.0.1:"), ESearchCase::CaseSensitive)
			&& !OutEndpointKey.Contains(TEXT("@"));
	}

	bool ResponseFitsBound(const FHttpResponsePtr& Response, FString& OutError)
	{
		OutError.Reset();
		if (!Response.IsValid())
		{
			OutError = TEXT("Capability inventory received no HTTP response.");
			return false;
		}
		return FHyperAIStudioCapabilityInventoryParser::ValidateResponseByteLengths(
			Response->GetContentLength(),
			Response->GetContent().Num(),
			OutError);
	}

	bool ReadBoundedBody(const FHttpResponsePtr& Response, FString& OutBody, FString& OutError)
	{
		OutBody.Reset();
		if (!ResponseFitsBound(Response, OutError))
		{
			return false;
		}
		// The byte bounds above must be checked before this UTF-8 conversion/allocation.
		OutBody = Response->GetContentAsString();
		return true;
	}

	bool IsProvablyNotSent(const FHttpRequestPtr& Request)
	{
		// FailureReason::ConnectionError is intentionally not enough: UE's HTTP backends can map
		// send/activity timeouts to that value. Only NotStarted proves the request never entered
		// transport; ProcessRequest() == false is handled by the corresponding immediate path.
		return Request.IsValid()
			&& Request->GetStatus() == EHttpRequestStatus::NotStarted;
	}

	FString SerializeObject(const TSharedRef<FJsonObject>& Object)
	{
		FString Json;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
		FJsonSerializer::Serialize(Object, Writer);
		return Json;
	}

	FString Sha1Utf8(const FString& Value)
	{
		const FTCHARToUTF8 Utf8(*Value);
		uint8 Digest[FSHA1::DigestSize];
		FSHA1::HashBuffer(Utf8.Get(), Utf8.Length(), Digest);
		FString Hex(TEXT("sha1:"));
		Hex.Reserve(5 + FSHA1::DigestSize * 2);
		for (const uint8 Byte : Digest)
		{
			Hex += FString::Printf(TEXT("%02x"), Byte);
		}
		return Hex;
	}

	FString MakeInventoryFingerprint(const FHyperAIStudioCapabilitySnapshot& Snapshot)
	{
		TArray<FString> Parts;
		Parts.Add(FString::Printf(
			TEXT("mode:%d:top-count:%d:toolset-count:%d:truncated:%d"),
			static_cast<int32>(Snapshot.DiscoveryMode),
			Snapshot.TopLevelToolCount,
			Snapshot.DiscoverableToolsetCount,
			Snapshot.bTruncated ? 1 : 0));
		if (Snapshot.DetailedTargetToolsetCount > 0 || Snapshot.bDetailedInventoryComplete)
		{
			Parts.Add(FString::Printf(
				TEXT("detailed-targets:%d:detailed-complete:%d"),
				Snapshot.DetailedTargetToolsetCount,
				Snapshot.bDetailedInventoryComplete ? 1 : 0));
		}
		if (!Snapshot.NextCursor.IsEmpty())
		{
			Parts.Add(TEXT("next-cursor:") + Snapshot.NextCursor);
		}
		for (const FString& ToolName : Snapshot.TopLevelToolNames)
		{
			Parts.Add(TEXT("top:") + ToolName);
		}
		for (const FHyperAIStudioDiscoveredToolset& Toolset : Snapshot.Toolsets)
		{
			Parts.Add(FString::Printf(TEXT("discovered:%s:%s"), *Toolset.Name, *Toolset.Description));
		}
		for (const FHyperAIStudioDescribedToolset& Toolset : Snapshot.DescribedToolsets)
		{
			Parts.Add(FString::Printf(TEXT("described:%s:%s:%d"), *Toolset.Name, *Toolset.SchemaHash, Toolset.ToolCount));
		}
		Parts.Sort([](const FString& Left, const FString& Right)
		{
			return Left.Compare(Right, ESearchCase::CaseSensitive) < 0;
		});
		return Sha1Utf8(FString::Join(Parts, TEXT("\n")));
	}

	class FInventorySession : public TSharedFromThis<FInventorySession>
	{
	public:
		using FFinished = TFunction<void(FHyperAIStudioCapabilityInventoryResult&&, bool)>;

		FInventorySession(
			FString InRequestEndpoint,
			FString InEndpointKey,
			const uint64 InGeneration,
			TOptional<FCachedSchema> InCachedCoreSchema,
			TOptional<EHyperAIStudioDetailedInventoryScope> InDetailedScope,
			FFinished InOnFinished)
			: RequestEndpoint(MoveTemp(InRequestEndpoint))
			, EndpointKey(MoveTemp(InEndpointKey))
			, Generation(InGeneration)
			, CachedCoreSchema(MoveTemp(InCachedCoreSchema))
			, DetailedScope(InDetailedScope)
			, OnFinished(MoveTemp(InOnFinished))
			, StartedSeconds(FPlatformTime::Seconds())
		{
			Result.Endpoint = EndpointKey;
			Result.RefreshGeneration = Generation;
			Result.Snapshot.RefreshGeneration = Generation;
		}

		void Start()
		{
			TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
			Params->SetStringField(TEXT("protocolVersion"), UE::ModelContextProtocol::ProtocolVersion);
			Params->SetObjectField(TEXT("capabilities"), MakeShared<FJsonObject>());
			TSharedRef<FJsonObject> ClientInfo = MakeShared<FJsonObject>();
			ClientInfo->SetStringField(TEXT("name"), TEXT("HyperAIStudio Capability Inventory"));
			ClientInfo->SetStringField(TEXT("version"), TEXT("1.0.0"));
			Params->SetObjectField(TEXT("clientInfo"), ClientInfo);

			TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetStringField(TEXT("jsonrpc"), UE::ModelContextProtocol::JsonRpcVersion);
			Root->SetNumberField(TEXT("id"), 1);
			Root->SetStringField(TEXT("method"), TEXT("initialize"));
			Root->SetObjectField(TEXT("params"), Params);
			Post(SerializeObject(Root), false, [Self = AsShared()](FHttpResponsePtr Response)
			{
				Self->OnInitialize(Response);
			});
		}

	private:
		void Post(const FString& Payload, const bool bInitialized, TFunction<void(FHttpResponsePtr)> OnResponse)
		{
			if (bFinished)
			{
				return;
			}
			const double SessionDeadlineSeconds = DetailedScope.IsSet()
				? DetailedSessionDeadlineSeconds
				: TotalSessionDeadlineSeconds;
			if (FPlatformTime::Seconds() - StartedSeconds > SessionDeadlineSeconds)
			{
				Finish(false, FString::Printf(
					TEXT("Capability inventory exceeded its %.0f second total deadline."),
					SessionDeadlineSeconds), false);
				return;
			}

			TSharedRef<IHttpRequest> Request = FHttpModule::Get().CreateRequest();
			Request->SetURL(RequestEndpoint);
			Request->SetVerb(TEXT("POST"));
			Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
			Request->SetHeader(TEXT("Accept"), TEXT("application/json, text/event-stream"));
			if (bInitialized)
			{
				Request->SetHeader(TEXT("Mcp-Protocol-Version"), ProtocolVersion);
				if (!SessionId.IsEmpty())
				{
					Request->SetHeader(TEXT("Mcp-Session-Id"), SessionId);
				}
			}
			Request->SetContentAsString(Payload);
			Request->SetTimeout(RequestTimeoutSeconds);
			Request->OnProcessRequestComplete().BindLambda(
				[Self = AsShared(), OnResponse = MoveTemp(OnResponse)](FHttpRequestPtr CompletedRequest, FHttpResponsePtr Response, const bool bConnected) mutable
				{
					if (Self->bFinished)
					{
						return;
					}
					if (!bConnected || !Response.IsValid())
					{
						const bool bAmbiguous = !IsProvablyNotSent(CompletedRequest);
						Self->Finish(
							false,
							bAmbiguous
								? TEXT("Capability inventory transport failed after the request may have been sent; Unreal MCP is locked until a confirmed server restart.")
								: TEXT("Capability inventory could not connect; the request was not sent."),
							false,
							bAmbiguous);
						return;
					}
					if (Self->SessionId.IsEmpty())
					{
						Self->SessionId = Response->GetHeader(TEXT("Mcp-Session-Id")).TrimStartAndEnd();
					}
					if (Self->ProtocolVersion.IsEmpty())
					{
						Self->ProtocolVersion = UE::ModelContextProtocol::ProtocolVersion;
					}
					const int32 Code = Response->GetResponseCode();
					if (Code < 200 || Code >= 300)
					{
						Self->Finish(false, FString::Printf(TEXT("Capability inventory received HTTP %d."), Code), true);
						return;
					}
					FString SizeError;
					if (!ResponseFitsBound(Response, SizeError))
					{
						Self->Finish(false, SizeError, true);
						return;
					}
					OnResponse(Response);
				});
			if (!Request->ProcessRequest())
			{
				Finish(false, TEXT("Capability inventory request could not be queued."), false);
			}
		}

		void OnInitialize(const FHttpResponsePtr& Response)
		{
			// Capture optional session ownership before parsing so malformed initialize bodies can still be cleaned up.
			SessionId = Response->GetHeader(TEXT("Mcp-Session-Id")).TrimStartAndEnd();
			ProtocolVersion = UE::ModelContextProtocol::ProtocolVersion;
			FString Body;
			FString ParseError;
			if (!ReadBoundedBody(Response, Body, ParseError))
			{
				Finish(false, ParseError, true);
				return;
			}
			TSharedPtr<FJsonObject> InitializeResult;
			if (!FHyperAIStudioCapabilityInventoryParser::ParseJsonRpcResult(Body, 1, InitializeResult, ParseError))
			{
				Finish(false, ParseError, true);
				return;
			}

			if (InitializeResult->HasField(TEXT("protocolVersion")))
			{
				FString Negotiated;
				if (!InitializeResult->TryGetStringField(TEXT("protocolVersion"), Negotiated) || Negotiated.IsEmpty())
				{
					Finish(false, TEXT("initialize result.protocolVersion is not a non-empty string."), true);
					return;
				}
				ProtocolVersion = MoveTemp(Negotiated);
			}

			Post(TEXT("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\",\"params\":{}}"), true,
				[Self = AsShared()](FHttpResponsePtr InitializedResponse)
				{
					FString Body;
					FString Error;
					if (!ReadBoundedBody(InitializedResponse, Body, Error))
					{
						Self->Finish(false, Error, true);
						return;
					}
					if (!Body.TrimStartAndEnd().IsEmpty())
					{
						Self->Finish(false, TEXT("notifications/initialized returned an unexpected response body."), true);
						return;
					}
					Self->RequestToolsList();
				});
		}

		void RequestToolsList()
		{
			Post(TEXT("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}"), true,
				[Self = AsShared()](FHttpResponsePtr Response)
				{
					FString Body;
					FString ParseError;
					if (!ReadBoundedBody(Response, Body, ParseError)
						|| !FHyperAIStudioCapabilityInventoryParser::ParseToolsListResponse(
							Body, 2, Self->Result.Snapshot, ParseError))
					{
						Self->Finish(false, ParseError, true);
						return;
					}
					Self->Result.Snapshot.RefreshGeneration = Self->Generation;
					if (Self->Result.Snapshot.DiscoveryMode == EHyperAIStudioToolDiscoveryMode::Eager)
					{
						Self->Result.Snapshot.InventoryFingerprint = MakeInventoryFingerprint(Self->Result.Snapshot);
						Self->Finish(true, FString::Printf(
							TEXT("Eager inventory contains %d top-level tools."),
							Self->Result.Snapshot.TopLevelToolCount), true);
						return;
					}
					if (Self->Result.Snapshot.DiscoveryMode != EHyperAIStudioToolDiscoveryMode::ToolSearch)
					{
						Self->Finish(false, Self->Result.Snapshot.ProbeError, true);
						return;
					}

					Self->CallMetaTool(TEXT("list_toolsets"), MakeShared<FJsonObject>(),
						[Self](FHttpResponsePtr ListResponse, const int64 RequestId)
						{
							Self->OnListToolsets(ListResponse, RequestId);
						});
				});
		}

		void CallMetaTool(
			const FString& Name,
			const TSharedRef<FJsonObject>& Arguments,
			TFunction<void(FHttpResponsePtr, int64)> OnResponse)
		{
			const int64 RequestId = NextRequestId++;
			TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
			Params->SetStringField(TEXT("name"), Name);
			Params->SetObjectField(TEXT("arguments"), Arguments);
			TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
			Root->SetNumberField(TEXT("id"), RequestId);
			Root->SetStringField(TEXT("method"), TEXT("tools/call"));
			Root->SetObjectField(TEXT("params"), Params);
			Post(SerializeObject(Root), true,
				[OnResponse = MoveTemp(OnResponse), RequestId](FHttpResponsePtr Response) mutable
				{
					OnResponse(Response, RequestId);
				});
		}

		void OnListToolsets(const FHttpResponsePtr& Response, const int64 RequestId)
		{
			FString Body;
			FString ParseError;
			bool bTruncated = false;
			int32 MalformedLines = 0;
			if (!ReadBoundedBody(Response, Body, ParseError)
				|| !FHyperAIStudioCapabilityInventoryParser::ParseListToolsetsResponse(
					Body,
					RequestId,
					Result.Snapshot.Toolsets,
					bTruncated,
					MalformedLines,
					ParseError))
			{
				Finish(false, ParseError, true);
				return;
			}
			Result.Snapshot.DiscoverableToolsetCount = Result.Snapshot.Toolsets.Num();
			Result.Snapshot.bTruncated |= bTruncated;
			if (MalformedLines > 0)
			{
				Finish(false, FString::Printf(TEXT("list_toolsets contained %d malformed rows."), MalformedLines), true);
				return;
			}

			const FHyperAIStudioDiscoveredToolset* Sentinel = Result.Snapshot.Toolsets.FindByPredicate(
				[](const FHyperAIStudioDiscoveredToolset& Toolset)
				{
					return Toolset.Name.Equals(CoreSentinelToolset, ESearchCase::CaseSensitive);
				});
			if (!Sentinel)
			{
				Finish(false, FString::Printf(
					TEXT("Core sentinel %s was not discovered by list_toolsets."),
					*CoreSentinelToolset), true);
				return;
			}
			if (DetailedScope.IsSet())
			{
				if (Result.Snapshot.bTruncated)
				{
					Finish(false, TEXT("Detailed capability inventory cannot be exact because toolset discovery was truncated."), true);
					return;
				}

				DetailedToolsetNames.Reset();
				for (const FHyperAIStudioDiscoveredToolset& Toolset : Result.Snapshot.Toolsets)
				{
					const bool bInclude = DetailedScope.GetValue() == EHyperAIStudioDetailedInventoryScope::AllToolsets
						|| !Toolset.Name.StartsWith(TEXT("HyperAIStudio"), ESearchCase::CaseSensitive);
					if (bInclude)
					{
						DetailedToolsetNames.Add(Toolset.Name);
					}
				}
				Result.Snapshot.DetailedTargetToolsetCount = DetailedToolsetNames.Num();
				DetailedToolsetIndex = 0;
				RequestNextDetailedToolset();
				return;
			}

			if (CachedCoreSchema.IsSet()
				&& FPlatformTime::Seconds() - CachedCoreSchema->CompletedSeconds <= DescribedSchemaLifetimeSeconds)
			{
				Result.bSchemaFromCache = true;
				Result.Snapshot.DescribedToolsets.Add(CachedCoreSchema->Toolset);
				Result.Snapshot.DescribedToolCount = CachedCoreSchema->Toolset.ToolCount;
				FinishToolSearchSuccess();
				return;
			}

			TSharedRef<FJsonObject> Arguments = MakeShared<FJsonObject>();
			Arguments->SetStringField(TEXT("toolset_name"), CoreSentinelToolset);
			CallMetaTool(TEXT("describe_toolset"), Arguments,
				[Self = AsShared()](FHttpResponsePtr DescribeResponse, const int64 DescribeRequestId)
				{
					Self->OnDescribeCoreSentinel(DescribeResponse, DescribeRequestId);
				});
		}

		void OnDescribeCoreSentinel(const FHttpResponsePtr& Response, const int64 RequestId)
		{
			FString Body;
			FString ParseError;
			FHyperAIStudioDescribedToolset Described;
			if (!ReadBoundedBody(Response, Body, ParseError)
				|| !FHyperAIStudioCapabilityInventoryParser::ParseDescribeToolsetResponse(
					Body, RequestId, Described, ParseError))
			{
				Finish(false, FString::Printf(TEXT("describe_toolset failed for %s: %s"), *CoreSentinelToolset, *ParseError), true);
				return;
			}
			if (!Described.Name.Equals(CoreSentinelToolset, ESearchCase::CaseSensitive))
			{
				Finish(false, FString::Printf(
					TEXT("describe_toolset returned %s while %s was requested."),
					*Described.Name,
					*CoreSentinelToolset), true);
				return;
			}
			Result.Snapshot.DescribedToolCount = Described.ToolCount;
			Result.Snapshot.DescribedToolsets.Add(MoveTemp(Described));
			FinishToolSearchSuccess();
		}

		void RequestNextDetailedToolset()
		{
			if (DetailedToolsetIndex >= DetailedToolsetNames.Num())
			{
				FinishDetailedToolSearchSuccess();
				return;
			}

			const FString ToolsetName = DetailedToolsetNames[DetailedToolsetIndex];
			TSharedRef<FJsonObject> Arguments = MakeShared<FJsonObject>();
			Arguments->SetStringField(TEXT("toolset_name"), ToolsetName);
			CallMetaTool(TEXT("describe_toolset"), Arguments,
				[Self = AsShared(), ToolsetName](FHttpResponsePtr DescribeResponse, const int64 DescribeRequestId)
				{
					Self->OnDescribeDetailedToolset(DescribeResponse, DescribeRequestId, ToolsetName);
				});
		}

		void OnDescribeDetailedToolset(
			const FHttpResponsePtr& Response,
			const int64 RequestId,
			const FString& RequestedToolsetName)
		{
			FString Body;
			FString ParseError;
			FHyperAIStudioDescribedToolset Described;
			if (!ReadBoundedBody(Response, Body, ParseError)
				|| !FHyperAIStudioCapabilityInventoryParser::ParseDescribeToolsetResponse(
					Body, RequestId, Described, ParseError))
			{
				Finish(false, FString::Printf(
					TEXT("describe_toolset failed for %s: %s"),
					*RequestedToolsetName,
					*ParseError), true);
				return;
			}
			if (!Described.Name.Equals(RequestedToolsetName, ESearchCase::CaseSensitive))
			{
				Finish(false, FString::Printf(
					TEXT("describe_toolset returned %s while %s was requested."),
					*Described.Name,
					*RequestedToolsetName), true);
				return;
			}
			if (Described.ToolCount > FHyperAIStudioCapabilityInventoryParser::MaxDetailedTools
				- Result.Snapshot.DescribedToolCount)
			{
				Finish(false, FString::Printf(
					TEXT("Detailed capability inventory exceeds the aggregate bound of %d tools."),
					FHyperAIStudioCapabilityInventoryParser::MaxDetailedTools), true);
				return;
			}

			Result.Snapshot.DescribedToolCount += Described.ToolCount;
			Result.Snapshot.DescribedToolsets.Add(MoveTemp(Described));
			++DetailedToolsetIndex;
			RequestNextDetailedToolset();
		}

		void FinishDetailedToolSearchSuccess()
		{
			Result.Snapshot.bDetailedInventoryComplete = true;
			Result.Snapshot.InventoryFingerprint = MakeInventoryFingerprint(Result.Snapshot);
			const TCHAR* ScopeLabel = DetailedScope.GetValue() == EHyperAIStudioDetailedInventoryScope::EpicToolsets
				? TEXT("Epic")
				: TEXT("all");
			Finish(true, FString::Printf(
				TEXT("Detailed %s inventory described %d toolsets with %d tools."),
				ScopeLabel,
				Result.Snapshot.DescribedToolsets.Num(),
				Result.Snapshot.DescribedToolCount), true);
		}

		void FinishToolSearchSuccess()
		{
			Result.Snapshot.InventoryFingerprint = MakeInventoryFingerprint(Result.Snapshot);
			const FString TruncatedText = Result.Snapshot.bTruncated ? TEXT(" (incomplete/bounded)") : FString();
			Finish(true, FString::Printf(
				TEXT("Tool-search inventory discovered %d toolsets and described only the Core sentinel (%d tools)%s."),
				Result.Snapshot.DiscoverableToolsetCount,
				Result.Snapshot.DescribedToolCount,
				*TruncatedText), true);
		}

		void Finish(
			const bool bSuccess,
			const FString& Message,
			const bool bCleanupSafe,
			const bool bAmbiguousTransport = false)
		{
			if (bFinished)
			{
				return;
			}
			bFinished = true;
			Result.bSuccess = bSuccess;
			Result.Message = Message.IsEmpty() ? TEXT("Capability inventory failed without detail.") : Message;
			Result.CompletedUtc = FDateTime::UtcNow();
			bQuarantineRelease = bAmbiguousTransport;
			Result.RefreshGeneration = Generation;
			Result.Snapshot.RefreshGeneration = Generation;
			Result.Snapshot.CapturedAtUtc = Result.CompletedUtc;
			if (!bSuccess)
			{
				Result.Snapshot.ProbeError = Result.Message;
			}

			if (bCleanupSafe && !SessionId.IsEmpty())
			{
				TSharedRef<IHttpRequest> DeleteRequest = FHttpModule::Get().CreateRequest();
				DeleteRequest->SetURL(RequestEndpoint);
				DeleteRequest->SetVerb(TEXT("DELETE"));
				DeleteRequest->SetHeader(TEXT("Mcp-Session-Id"), SessionId);
				DeleteRequest->SetHeader(TEXT("Mcp-Protocol-Version"), ProtocolVersion);
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

		void Deliver()
		{
			if (bDelivered)
			{
				return;
			}
			bDelivered = true;
			OnFinished(MoveTemp(Result), bQuarantineRelease);
		}

		FString RequestEndpoint;
		FString EndpointKey;
		uint64 Generation = 0;
		TOptional<FCachedSchema> CachedCoreSchema;
		TOptional<EHyperAIStudioDetailedInventoryScope> DetailedScope;
		TArray<FString> DetailedToolsetNames;
		int32 DetailedToolsetIndex = 0;
		FString SessionId;
		FString ProtocolVersion;
		FFinished OnFinished;
		FHyperAIStudioCapabilityInventoryResult Result;
		double StartedSeconds = 0.0;
		int64 NextRequestId = 3;
		bool bFinished = false;
		bool bDelivered = false;
		bool bQuarantineRelease = false;
	};

	void TrimEndpointStatesLocked()
	{
		while (StatesByEndpoint.Num() > MaxCacheEntries)
		{
			FString OldestKey;
			double OldestAccess = TNumericLimits<double>::Max();
			for (const auto& Pair : StatesByEndpoint)
			{
				bool bDetailedRefreshOutstanding = false;
				for (const auto& DetailedPair : Pair.Value.DetailedStatesByScope)
				{
					bDetailedRefreshOutstanding |= !DetailedPair.Value.OutstandingGenerations.IsEmpty();
				}
				if (Pair.Value.OutstandingGenerations.IsEmpty()
					&& !bDetailedRefreshOutstanding
					&& Pair.Value.LastAccessSeconds < OldestAccess)
				{
					OldestKey = Pair.Key;
					OldestAccess = Pair.Value.LastAccessSeconds;
				}
			}
			if (OldestKey.IsEmpty())
			{
				return;
			}
			StatesByEndpoint.Remove(OldestKey);
		}
	}

	void CompleteEndpoint(
		const FString& EndpointKey,
		const uint64 Generation,
		FHyperAIStudioCapabilityInventoryResult&& InResult)
	{
		TArray<FHyperAIStudioCapabilityInventoryClient::FCompletion> Waiters;
		FHyperAIStudioCapabilityInventoryResult DeliveredResult = MoveTemp(InResult);
		{
			FScopeLock Lock(&CacheMutex);
			FEndpointState* State = StatesByEndpoint.Find(EndpointKey);
			if (!State)
			{
				DeliveredResult.bSuccess = false;
				DeliveredResult.bSuperseded = true;
				DeliveredResult.Message = TEXT("Capability inventory result was discarded after endpoint invalidation.");
			}
			else
			{
				State->OutstandingGenerations.Remove(Generation);
				State->WaitersByGeneration.RemoveAndCopyValue(Generation, Waiters);
				State->LastAccessSeconds = FPlatformTime::Seconds();
				if (Generation != State->CurrentGeneration)
				{
					DeliveredResult.bSuccess = false;
					DeliveredResult.bSuperseded = true;
					DeliveredResult.bStale = true;
					DeliveredResult.bRefreshing = State->OutstandingGenerations.Contains(State->CurrentGeneration);
					DeliveredResult.Message = TEXT("Capability inventory result was superseded by a newer refresh generation.");
					DeliveredResult.Snapshot.bStale = true;
					DeliveredResult.Snapshot.bRefreshing = DeliveredResult.bRefreshing;
				}
				else
				{
					const double Now = FPlatformTime::Seconds();
					DeliveredResult.bFromCache = false;
					DeliveredResult.bRefreshing = false;
					DeliveredResult.RefreshGeneration = Generation;
					DeliveredResult.Snapshot.RefreshGeneration = Generation;
					DeliveredResult.Snapshot.bRefreshing = false;
					if (DeliveredResult.bSuccess)
					{
						DeliveredResult.bStale = false;
						DeliveredResult.Snapshot.bStale = false;
						FCachedResult Success;
						Success.Result = DeliveredResult;
						Success.CompletedSeconds = Now;
						State->LastSuccess = Success;
						State->LatestResult = Success;
						if (DeliveredResult.Snapshot.DiscoveryMode != EHyperAIStudioToolDiscoveryMode::ToolSearch)
						{
							State->Schemas.Reset();
						}
						else if (!DeliveredResult.bSchemaFromCache && !DeliveredResult.Snapshot.DescribedToolsets.IsEmpty())
						{
							const FHyperAIStudioDescribedToolset& Described = DeliveredResult.Snapshot.DescribedToolsets[0];
							if (Described.Name.Equals(CoreSentinelToolset, ESearchCase::CaseSensitive))
							{
								FCachedSchema Schema;
								Schema.Toolset = Described;
								Schema.CompletedSeconds = Now;
								State->Schemas.Add(CoreSentinelToolset, MoveTemp(Schema));
							}
						}
					}
					else
					{
						const FString FailureMessage = DeliveredResult.Message;
						if (State->LastSuccess.IsSet())
						{
							DeliveredResult.Snapshot = State->LastSuccess->Result.Snapshot;
							DeliveredResult.bStale = true;
							DeliveredResult.Snapshot.bStale = true;
							DeliveredResult.Snapshot.bRefreshing = false;
							DeliveredResult.Snapshot.RefreshGeneration = Generation;
							DeliveredResult.Snapshot.ProbeError = FailureMessage;
							DeliveredResult.Message = FString::Printf(
								TEXT("Stale inventory retained after refresh failure: %s"),
								*FailureMessage);
						}
						FCachedResult Failure;
						Failure.Result = DeliveredResult;
						Failure.CompletedSeconds = Now;
						State->LatestResult = MoveTemp(Failure);
					}
				}
			}
			TrimEndpointStatesLocked();
		}

		for (FHyperAIStudioCapabilityInventoryClient::FCompletion& Waiter : Waiters)
		{
			if (Waiter)
			{
				Waiter(DeliveredResult);
			}
		}
	}

	void CompleteDetailedEndpoint(
		const FString& EndpointKey,
		const EHyperAIStudioDetailedInventoryScope Scope,
		const uint64 Generation,
		FHyperAIStudioCapabilityInventoryResult&& InResult)
	{
		TArray<FHyperAIStudioCapabilityInventoryClient::FCompletion> Waiters;
		FHyperAIStudioCapabilityInventoryResult DeliveredResult = MoveTemp(InResult);
		{
			FScopeLock Lock(&CacheMutex);
			FEndpointState* State = StatesByEndpoint.Find(EndpointKey);
			FEndpointState::FDetailedState* DetailedState = State
				? State->DetailedStatesByScope.Find(static_cast<uint8>(Scope))
				: nullptr;
			if (!State || !DetailedState)
			{
				DeliveredResult.bSuccess = false;
				DeliveredResult.bSuperseded = true;
				DeliveredResult.Message = TEXT("Detailed capability inventory result was discarded after endpoint invalidation.");
			}
			else
			{
				DetailedState->OutstandingGenerations.Remove(Generation);
				DetailedState->WaitersByGeneration.RemoveAndCopyValue(Generation, Waiters);
				State->LastAccessSeconds = FPlatformTime::Seconds();
				if (Generation != DetailedState->CurrentGeneration)
				{
					DeliveredResult.bSuccess = false;
					DeliveredResult.bSuperseded = true;
					DeliveredResult.bStale = true;
					DeliveredResult.bRefreshing = DetailedState->OutstandingGenerations.Contains(
						DetailedState->CurrentGeneration);
					DeliveredResult.Message = TEXT("Detailed capability inventory result was superseded by a newer refresh generation.");
					DeliveredResult.Snapshot.bStale = true;
					DeliveredResult.Snapshot.bRefreshing = DeliveredResult.bRefreshing;
				}
				else
				{
					const double Now = FPlatformTime::Seconds();
					DeliveredResult.bFromCache = false;
					DeliveredResult.bRefreshing = false;
					DeliveredResult.RefreshGeneration = Generation;
					DeliveredResult.Snapshot.RefreshGeneration = Generation;
					DeliveredResult.Snapshot.bRefreshing = false;
					if (DeliveredResult.bSuccess)
					{
						DeliveredResult.bStale = false;
						DeliveredResult.Snapshot.bStale = false;
						FCachedResult Success;
						Success.Result = DeliveredResult;
						Success.CompletedSeconds = Now;
						DetailedState->LastSuccess = Success;
						DetailedState->LatestResult = Success;
						for (const FHyperAIStudioDescribedToolset& Described : DeliveredResult.Snapshot.DescribedToolsets)
						{
							FCachedSchema Schema;
							Schema.Toolset = Described;
							Schema.CompletedSeconds = Now;
							State->Schemas.Add(Described.Name, MoveTemp(Schema));
						}
					}
					else
					{
						const FString FailureMessage = DeliveredResult.Message;
						if (DetailedState->LastSuccess.IsSet())
						{
							DeliveredResult.Snapshot = DetailedState->LastSuccess->Result.Snapshot;
							DeliveredResult.bStale = true;
							DeliveredResult.Snapshot.bStale = true;
							DeliveredResult.Snapshot.bRefreshing = false;
							DeliveredResult.Snapshot.RefreshGeneration = Generation;
							DeliveredResult.Snapshot.ProbeError = FailureMessage;
							DeliveredResult.Message = FString::Printf(
								TEXT("Stale detailed inventory retained after refresh failure: %s"),
								*FailureMessage);
						}
						FCachedResult Failure;
						Failure.Result = DeliveredResult;
						Failure.CompletedSeconds = Now;
						DetailedState->LatestResult = MoveTemp(Failure);
					}
				}
			}
			TrimEndpointStatesLocked();
		}
		for (FHyperAIStudioCapabilityInventoryClient::FCompletion& Waiter : Waiters)
		{
			if (Waiter)
			{
				Waiter(DeliveredResult);
			}
		}
	}
}

TOptional<FHyperAIStudioCapabilityInventoryResult> FHyperAIStudioCapabilityInventoryClient::GetCached(
	const FString& Endpoint,
	const FTimespan MaxAge)
{
	using namespace HyperAIStudio::CapabilityInventoryClient::Private;
	FString EndpointKey;
	if (!IsAllowedEpicEndpoint(Endpoint, EndpointKey))
	{
		return {};
	}

	FScopeLock Lock(&CacheMutex);
	FEndpointState* State = StatesByEndpoint.Find(EndpointKey);
	if (!State)
	{
		return {};
	}
	const double Now = FPlatformTime::Seconds();
	State->LastAccessSeconds = Now;
	if (State->OutstandingGenerations.Contains(State->CurrentGeneration) && State->LastSuccess.IsSet())
	{
		FHyperAIStudioCapabilityInventoryResult Result = State->LastSuccess->Result;
		Result.bFromCache = true;
		Result.bRefreshing = true;
		Result.bStale = Now - State->LastSuccess->CompletedSeconds > MaxAge.GetTotalSeconds();
		Result.Snapshot.bRefreshing = true;
		Result.Snapshot.bStale = Result.bStale;
		Result.Message = FString::Printf(
			TEXT("Refreshing capability inventory; showing the last successful snapshot from %s."),
			*Result.CompletedUtc.ToIso8601());
		return Result;
	}
	if (!State->LatestResult.IsSet()
		|| Now - State->LatestResult->CompletedSeconds > MaxAge.GetTotalSeconds())
	{
		return {};
	}
	FHyperAIStudioCapabilityInventoryResult Result = State->LatestResult->Result;
	Result.bFromCache = true;
	return Result;
}

TOptional<FHyperAIStudioCapabilityInventoryResult> FHyperAIStudioCapabilityInventoryClient::GetDetailedCached(
	const FString& Endpoint,
	const EHyperAIStudioDetailedInventoryScope Scope,
	const FTimespan MaxAge)
{
	using namespace HyperAIStudio::CapabilityInventoryClient::Private;
	FString EndpointKey;
	if (!IsAllowedEpicEndpoint(Endpoint, EndpointKey))
	{
		return {};
	}

	FScopeLock Lock(&CacheMutex);
	FEndpointState* State = StatesByEndpoint.Find(EndpointKey);
	FEndpointState::FDetailedState* DetailedState = State
		? State->DetailedStatesByScope.Find(static_cast<uint8>(Scope))
		: nullptr;
	if (!State || !DetailedState)
	{
		return {};
	}
	const double Now = FPlatformTime::Seconds();
	State->LastAccessSeconds = Now;
	if (DetailedState->OutstandingGenerations.Contains(DetailedState->CurrentGeneration)
		&& DetailedState->LastSuccess.IsSet())
	{
		FHyperAIStudioCapabilityInventoryResult Result = DetailedState->LastSuccess->Result;
		Result.bFromCache = true;
		Result.bRefreshing = true;
		Result.bStale = Now - DetailedState->LastSuccess->CompletedSeconds > MaxAge.GetTotalSeconds();
		Result.Snapshot.bRefreshing = true;
		Result.Snapshot.bStale = Result.bStale;
		Result.Message = FString::Printf(
			TEXT("Refreshing detailed capability inventory; showing the last successful snapshot from %s."),
			*Result.CompletedUtc.ToIso8601());
		return Result;
	}
	if (!DetailedState->LatestResult.IsSet()
		|| Now - DetailedState->LatestResult->CompletedSeconds > MaxAge.GetTotalSeconds())
	{
		return {};
	}
	FHyperAIStudioCapabilityInventoryResult Result = DetailedState->LatestResult->Result;
	Result.bFromCache = true;
	return Result;
}

TOptional<FHyperAIStudioCapabilityInventoryResult> FHyperAIStudioCapabilityInventoryClient::GetLastDetailedSuccess(
	const FString& Endpoint,
	const EHyperAIStudioDetailedInventoryScope Scope)
{
	using namespace HyperAIStudio::CapabilityInventoryClient::Private;
	FString EndpointKey;
	if (!IsAllowedEpicEndpoint(Endpoint, EndpointKey))
	{
		return {};
	}

	FScopeLock Lock(&CacheMutex);
	FEndpointState* State = StatesByEndpoint.Find(EndpointKey);
	FEndpointState::FDetailedState* DetailedState = State
		? State->DetailedStatesByScope.Find(static_cast<uint8>(Scope))
		: nullptr;
	if (!State || !DetailedState || !DetailedState->LastSuccess.IsSet())
	{
		return {};
	}

	State->LastAccessSeconds = FPlatformTime::Seconds();
	FHyperAIStudioCapabilityInventoryResult Result = DetailedState->LastSuccess->Result;
	Result.bFromCache = true;
	Result.bRefreshing = DetailedState->OutstandingGenerations.Contains(DetailedState->CurrentGeneration);
	Result.bStale = true;
	Result.Snapshot.bRefreshing = Result.bRefreshing;
	Result.Snapshot.bStale = true;
	Result.Message = TEXT("Showing the last exact detailed inventory while refreshing it.");
	return Result;
}

void FHyperAIStudioCapabilityInventoryClient::RefreshAsync(
	const FString& Endpoint,
	const bool bForce,
	FCompletion OnComplete)
{
	using namespace HyperAIStudio::CapabilityInventoryClient::Private;
	FString EndpointKey;
	if (!IsAllowedEpicEndpoint(Endpoint, EndpointKey))
	{
		FHyperAIStudioCapabilityInventoryResult Rejected;
		Rejected.Endpoint = Endpoint;
		Rejected.Message = TEXT("Capability inventory accepts only an HTTP loopback Epic endpoint (localhost, 127.0.0.1 or ::1).");
		Rejected.CompletedUtc = FDateTime::UtcNow();
		if (OnComplete)
		{
			OnComplete(Rejected);
		}
		return;
	}

	TOptional<FHyperAIStudioCapabilityInventoryResult> CachedResult;
	TOptional<FCachedSchema> CachedCoreSchema;
	uint64 Generation = 0;
	bool bStartSession = false;
	{
		FScopeLock Lock(&CacheMutex);
		FEndpointState& State = StatesByEndpoint.FindOrAdd(EndpointKey);
		const double Now = FPlatformTime::Seconds();
		State.LastAccessSeconds = Now;

		const bool bCurrentOutstanding = State.OutstandingGenerations.Contains(State.CurrentGeneration);
		if (bCurrentOutstanding && (!bForce || State.bCurrentGenerationForced))
		{
			if (OnComplete)
			{
				State.WaitersByGeneration.FindOrAdd(State.CurrentGeneration).Add(MoveTemp(OnComplete));
			}
			return;
		}

		if (!bForce && !bCurrentOutstanding && State.LatestResult.IsSet())
		{
			const double AllowedAge = State.LatestResult->Result.bSuccess
				? CacheLifetimeSeconds
				: FailureCacheLifetimeSeconds;
			if (Now - State.LatestResult->CompletedSeconds <= AllowedAge)
			{
				CachedResult = State.LatestResult->Result;
				CachedResult->bFromCache = true;
			}
		}

		if (!CachedResult.IsSet())
		{
			++State.CurrentGeneration;
			if (State.CurrentGeneration == 0)
			{
				++State.CurrentGeneration;
			}
			Generation = State.CurrentGeneration;
			State.bCurrentGenerationForced = bForce;
			State.OutstandingGenerations.Add(Generation);
			if (OnComplete)
			{
				State.WaitersByGeneration.FindOrAdd(Generation).Add(MoveTemp(OnComplete));
			}
			if (!bForce)
			{
				const bool bLastSuccessfulModeWasToolSearch = State.LastSuccess.IsSet()
					&& State.LastSuccess->Result.Snapshot.DiscoveryMode == EHyperAIStudioToolDiscoveryMode::ToolSearch;
				if (bLastSuccessfulModeWasToolSearch)
				{
					if (const FCachedSchema* Schema = State.Schemas.Find(CoreSentinelToolset))
					{
						if (Now - Schema->CompletedSeconds <= DescribedSchemaLifetimeSeconds)
						{
							CachedCoreSchema = *Schema;
						}
					}
				}
			}
			bStartSession = true;
		}
	}

	if (CachedResult.IsSet())
	{
		if (OnComplete)
		{
			OnComplete(CachedResult.GetValue());
		}
		return;
	}

	if (bStartSession)
	{
		const FString RequestEndpoint = Endpoint.TrimStartAndEnd();
		FHyperAIStudioMcpSerialQueue::Enqueue(RequestEndpoint,
			[RequestEndpoint, EndpointKey, Generation, CachedCoreSchema](FHyperAIStudioMcpSerialQueue::FComplete Complete)
			{
				TSharedRef<FInventorySession> Session = MakeShared<FInventorySession>(
					RequestEndpoint,
					EndpointKey,
						Generation,
						CachedCoreSchema,
						TOptional<EHyperAIStudioDetailedInventoryScope>(),
					[EndpointKey, Generation, Complete = MoveTemp(Complete)](
						FHyperAIStudioCapabilityInventoryResult&& Result,
						const bool bAmbiguousTransport) mutable
					{
						Complete(bAmbiguousTransport
							? FHyperAIStudioMcpSerialQueue::ECompletionDisposition::AmbiguousTransport
							: FHyperAIStudioMcpSerialQueue::ECompletionDisposition::SafeToRelease);
						CompleteEndpoint(EndpointKey, Generation, MoveTemp(Result));
					});
				Session->Start();
			},
			[EndpointKey, Generation](const FString& Reason)
			{
				FHyperAIStudioCapabilityInventoryResult Rejected;
				Rejected.Endpoint = EndpointKey;
				Rejected.RefreshGeneration = Generation;
				Rejected.Message = Reason;
				Rejected.CompletedUtc = FDateTime::UtcNow();
				CompleteEndpoint(EndpointKey, Generation, MoveTemp(Rejected));
			});
	}
}

void FHyperAIStudioCapabilityInventoryClient::RefreshDetailedAsync(
	const FString& Endpoint,
	const bool bForce,
	const EHyperAIStudioDetailedInventoryScope Scope,
	FCompletion OnComplete)
{
	using namespace HyperAIStudio::CapabilityInventoryClient::Private;
	if (Scope != EHyperAIStudioDetailedInventoryScope::EpicToolsets
		&& Scope != EHyperAIStudioDetailedInventoryScope::AllToolsets)
	{
		FHyperAIStudioCapabilityInventoryResult Rejected;
		Rejected.Endpoint = Endpoint;
		Rejected.Message = TEXT("Detailed capability inventory received an invalid scope.");
		Rejected.CompletedUtc = FDateTime::UtcNow();
		if (OnComplete)
		{
			OnComplete(Rejected);
		}
		return;
	}

	FString EndpointKey;
	if (!IsAllowedEpicEndpoint(Endpoint, EndpointKey))
	{
		FHyperAIStudioCapabilityInventoryResult Rejected;
		Rejected.Endpoint = Endpoint;
		Rejected.Message = TEXT("Detailed capability inventory accepts only an HTTP loopback Epic endpoint.");
		Rejected.CompletedUtc = FDateTime::UtcNow();
		if (OnComplete)
		{
			OnComplete(Rejected);
		}
		return;
	}

	TOptional<FHyperAIStudioCapabilityInventoryResult> CachedResult;
	uint64 Generation = 0;
	bool bStartSession = false;
	{
		FScopeLock Lock(&CacheMutex);
		FEndpointState& State = StatesByEndpoint.FindOrAdd(EndpointKey);
		FEndpointState::FDetailedState& DetailedState =
			State.DetailedStatesByScope.FindOrAdd(static_cast<uint8>(Scope));
		const double Now = FPlatformTime::Seconds();
		State.LastAccessSeconds = Now;

		const bool bCurrentOutstanding = DetailedState.OutstandingGenerations.Contains(
			DetailedState.CurrentGeneration);
		if (bCurrentOutstanding && (!bForce || DetailedState.bCurrentGenerationForced))
		{
			if (OnComplete)
			{
				DetailedState.WaitersByGeneration.FindOrAdd(DetailedState.CurrentGeneration).Add(
					MoveTemp(OnComplete));
			}
			return;
		}

		if (!bForce && !bCurrentOutstanding && DetailedState.LatestResult.IsSet())
		{
			const double AllowedAge = DetailedState.LatestResult->Result.bSuccess
				? CacheLifetimeSeconds
				: FailureCacheLifetimeSeconds;
			if (Now - DetailedState.LatestResult->CompletedSeconds <= AllowedAge)
			{
				CachedResult = DetailedState.LatestResult->Result;
				CachedResult->bFromCache = true;
			}
		}

		if (!CachedResult.IsSet())
		{
			++DetailedState.CurrentGeneration;
			if (DetailedState.CurrentGeneration == 0)
			{
				++DetailedState.CurrentGeneration;
			}
			Generation = DetailedState.CurrentGeneration;
			DetailedState.bCurrentGenerationForced = bForce;
			DetailedState.OutstandingGenerations.Add(Generation);
			if (OnComplete)
			{
				DetailedState.WaitersByGeneration.FindOrAdd(Generation).Add(MoveTemp(OnComplete));
			}
			bStartSession = true;
		}
	}

	if (CachedResult.IsSet())
	{
		if (OnComplete)
		{
			OnComplete(CachedResult.GetValue());
		}
		return;
	}

	if (bStartSession)
	{
		const FString RequestEndpoint = Endpoint.TrimStartAndEnd();
		FHyperAIStudioMcpSerialQueue::Enqueue(RequestEndpoint,
			[RequestEndpoint, EndpointKey, Generation, Scope](FHyperAIStudioMcpSerialQueue::FComplete Complete)
			{
				TSharedRef<FInventorySession> Session = MakeShared<FInventorySession>(
					RequestEndpoint,
					EndpointKey,
					Generation,
					TOptional<FCachedSchema>(),
					Scope,
					[EndpointKey, Generation, Scope, Complete = MoveTemp(Complete)](
						FHyperAIStudioCapabilityInventoryResult&& Result,
						const bool bAmbiguousTransport) mutable
					{
						Complete(bAmbiguousTransport
							? FHyperAIStudioMcpSerialQueue::ECompletionDisposition::AmbiguousTransport
							: FHyperAIStudioMcpSerialQueue::ECompletionDisposition::SafeToRelease);
						CompleteDetailedEndpoint(EndpointKey, Scope, Generation, MoveTemp(Result));
					});
				Session->Start();
			},
			[EndpointKey, Generation, Scope](const FString& Reason)
			{
				FHyperAIStudioCapabilityInventoryResult Rejected;
				Rejected.Endpoint = EndpointKey;
				Rejected.RefreshGeneration = Generation;
				Rejected.Message = Reason;
				Rejected.CompletedUtc = FDateTime::UtcNow();
				CompleteDetailedEndpoint(EndpointKey, Scope, Generation, MoveTemp(Rejected));
			});
	}
}

void FHyperAIStudioCapabilityInventoryClient::Invalidate(const FString& Endpoint)
{
	using namespace HyperAIStudio::CapabilityInventoryClient::Private;
	const FString EndpointKey = FHyperAIStudioMcpSerialQueue::CanonicalizeEndpointKey(Endpoint);
	if (EndpointKey.IsEmpty())
	{
		return;
	}
	FScopeLock Lock(&CacheMutex);
	if (FEndpointState* State = StatesByEndpoint.Find(EndpointKey))
	{
		++State->CurrentGeneration;
		if (State->CurrentGeneration == 0)
		{
			++State->CurrentGeneration;
		}
		State->bCurrentGenerationForced = false;
		State->LatestResult.Reset();
		State->LastSuccess.Reset();
		for (auto& DetailedPair : State->DetailedStatesByScope)
		{
			FEndpointState::FDetailedState& DetailedState = DetailedPair.Value;
			++DetailedState.CurrentGeneration;
			if (DetailedState.CurrentGeneration == 0)
			{
				++DetailedState.CurrentGeneration;
			}
			DetailedState.bCurrentGenerationForced = false;
			DetailedState.LatestResult.Reset();
			DetailedState.LastSuccess.Reset();
		}
		State->Schemas.Reset();
		State->LastAccessSeconds = FPlatformTime::Seconds();
	}
}

void FHyperAIStudioCapabilityInventoryClient::InvalidateDetailed(const FString& Endpoint)
{
	using namespace HyperAIStudio::CapabilityInventoryClient::Private;
	const FString EndpointKey = FHyperAIStudioMcpSerialQueue::CanonicalizeEndpointKey(Endpoint);
	if (EndpointKey.IsEmpty())
	{
		return;
	}

	FScopeLock Lock(&CacheMutex);
	if (FEndpointState* State = StatesByEndpoint.Find(EndpointKey))
	{
		for (auto& DetailedPair : State->DetailedStatesByScope)
		{
			FEndpointState::FDetailedState& DetailedState = DetailedPair.Value;
			++DetailedState.CurrentGeneration;
			if (DetailedState.CurrentGeneration == 0)
			{
				++DetailedState.CurrentGeneration;
			}
			DetailedState.bCurrentGenerationForced = false;
			DetailedState.LatestResult.Reset();
			DetailedState.LastSuccess.Reset();
		}
		State->LastAccessSeconds = FPlatformTime::Seconds();
	}
}

void FHyperAIStudioCapabilityInventoryClient::NotifyConfirmedServerStopped(const FString& Endpoint)
{
	Invalidate(Endpoint);
	FHyperAIStudioMcpSerialQueue::ResetAfterConfirmedServerStop(Endpoint);
}
