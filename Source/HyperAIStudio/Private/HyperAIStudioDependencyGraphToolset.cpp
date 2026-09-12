// Games by Hyper 2026.

#include "HyperAIStudioDependencyGraphToolset.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Misc/PackageName.h"
#include "Misc/SecureHash.h"
#include "Modules/ModuleManager.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace HyperAIStudio::DependencyGraph::Private
{
	constexpr int32 MaxReasonChars = 256;
	constexpr int32 MaxDiagnosticFieldChars = 256;
	constexpr int32 MaxDiagnosticMessageChars = 384;

	const TArray<FString>& AllowedCategories()
	{
		static const TArray<FString> Values = {
			TEXT("manage"),
			TEXT("package"),
			TEXT("searchable_name")
		};
		return Values;
	}

	const TArray<FString>& AllowedProperties()
	{
		static const TArray<FString> Values = {
			TEXT("build"),
			TEXT("chunk_only"),
			TEXT("cook_rule"),
			TEXT("direct"),
			TEXT("editor_only"),
			TEXT("game"),
			TEXT("hard"),
			TEXT("indirect"),
			TEXT("none"),
			TEXT("not_build"),
			TEXT("soft")
		};
		return Values;
	}

	bool ContainsEmbeddedNull(const FString& Value)
	{
		for (int32 Index = 0; Index < Value.Len(); ++Index)
		{
			if (Value[Index] == TEXT('\0'))
			{
				return true;
			}
		}
		return false;
	}

	FString Sha1Utf8(const FString& Value)
	{
		const FTCHARToUTF8 Utf8(*Value);
		uint8 Digest[FSHA1::DigestSize];
		FSHA1::HashBuffer(Utf8.Get(), Utf8.Length(), Digest);
		FString Hex;
		Hex.Reserve(FSHA1::DigestSize * 2);
		for (const uint8 Byte : Digest)
		{
			Hex += FString::Printf(TEXT("%02x"), Byte);
		}
		return Hex;
	}

	void AppendFingerprintToken(FString& Buffer, const FString& Value)
	{
		Buffer += FString::FromInt(Value.Len());
		Buffer += TEXT(":");
		Buffer += Value;
		Buffer += TEXT("|");
	}

	void AddDiagnostic(
		TArray<FHyperAIStudioDependencyGraphDiagnostic>& Diagnostics,
		const FString& Code,
		const FString& Severity,
		const FString& Field,
		const FString& Message)
	{
		if (Diagnostics.Num() >= FHyperAIStudioDependencyGraphAnalyzer::HardMaxDiagnostics)
		{
			return;
		}
		FHyperAIStudioDependencyGraphDiagnostic& Diagnostic = Diagnostics.AddDefaulted_GetRef();
		Diagnostic.Code = Code.Left(64);
		Diagnostic.Severity = Severity.Left(16);
		Diagnostic.Field = Field.Left(MaxDiagnosticFieldChars);
		Diagnostic.Message = Message.Left(MaxDiagnosticMessageChars);
	}

	void AddDiagnosticOnce(
		TArray<FHyperAIStudioDependencyGraphDiagnostic>& Diagnostics,
		const FString& Code,
		const FString& Severity,
		const FString& Field,
		const FString& Message)
	{
		if (!Diagnostics.ContainsByPredicate([&Code](const FHyperAIStudioDependencyGraphDiagnostic& Existing)
		{
			return Existing.Code == Code;
		}))
		{
			AddDiagnostic(Diagnostics, Code, Severity, Field, Message);
		}
	}

	void RejectCursorWithoutStableGraph(
		FHyperAIStudioDependencyGraphResult& Result,
		const FString& Cursor,
		const FString& Message)
	{
		if (Cursor.IsEmpty())
		{
			return;
		}
		Result.CursorStatus = TEXT("rejected");
		Result.CursorDiagnosticCode = TEXT("cursor_graph_unavailable");
		AddDiagnosticOnce(
			Result.Diagnostics,
			TEXT("cursor_graph_unavailable"),
			TEXT("error"),
			TEXT("cursor"),
			Message);
	}

	bool NormalizeAllowlist(
		const TArray<FString>& Source,
		const TArray<FString>& Allowed,
		bool bAllowEmpty,
		const TCHAR* Field,
		TArray<FString>& OutValues,
		FString& OutErrorCode,
		FString& OutErrorMessage)
	{
		OutValues.Reset();
		if (!bAllowEmpty && Source.IsEmpty())
		{
			OutErrorCode = TEXT("empty_allowlist");
			OutErrorMessage = FString::Printf(TEXT("%s must contain at least one allowlisted value."), Field);
			return false;
		}
		if (Source.Num() > Allowed.Num())
		{
			OutErrorCode = TEXT("allowlist_too_large");
			OutErrorMessage = FString::Printf(TEXT("%s contains more entries than the supported allowlist."), Field);
			return false;
		}

		TSet<FString> Seen;
		for (const FString& Value : Source)
		{
			if (Value.IsEmpty() || ContainsEmbeddedNull(Value) || !Allowed.Contains(Value))
			{
				OutErrorCode = TEXT("unsupported_allowlist_value");
				OutErrorMessage = FString::Printf(TEXT("%s contains unsupported value '%s'."), Field, *Value.Left(64));
				return false;
			}
			Seen.Add(Value);
		}
		OutValues = Seen.Array();
		OutValues.Sort();
		return true;
	}

	FString MakeRequestFingerprint(const FHyperAIStudioNormalizedDependencyGraphRequest& Request)
	{
		FString Canonical;
		AppendFingerprintToken(Canonical, TEXT("hyperai.dependency-request.v1"));
		AppendFingerprintToken(Canonical, Request.InputAssetPath);
		AppendFingerprintToken(Canonical, Request.RootPackage);
		AppendFingerprintToken(Canonical, Request.Direction);
		AppendFingerprintToken(Canonical, FString::Join(Request.Categories, TEXT(",")));
		AppendFingerprintToken(Canonical, FString::Join(Request.Properties, TEXT(",")));
		AppendFingerprintToken(Canonical, FString::FromInt(Request.MaxDepth));
		AppendFingerprintToken(Canonical, FString::FromInt(Request.MaxNodes));
		AppendFingerprintToken(Canonical, FString::FromInt(Request.PageSize));
		return TEXT("sha1:") + Sha1Utf8(Canonical);
	}


	bool MatchesPropertyAllowlist(
		const TArray<FString>& EdgeProperties,
		const TArray<FString>& PropertyAllowlist)
	{
		if (PropertyAllowlist.IsEmpty())
		{
			return true;
		}
		return PropertyAllowlist.ContainsByPredicate([&EdgeProperties](const FString& Allowed)
		{
			return EdgeProperties.Contains(Allowed);
		});
	}

	FString EdgeKey(const FHyperAIStudioDependencyGraphSnapshotEdge& Edge)
	{
		FString Key;
		AppendFingerprintToken(Key, Edge.SourceIdentifier);
		AppendFingerprintToken(Key, Edge.TargetIdentifier);
		AppendFingerprintToken(Key, Edge.Category);
		AppendFingerprintToken(Key, FString::Join(Edge.Properties, TEXT(",")));
		return Key;
	}

	template <typename ItemType>
	void SiftMaxHeapUp(TArray<ItemType>& Heap, int32 Index)
	{
		while (Index > 0)
		{
			const int32 Parent = (Index - 1) / 2;
			if (Heap[Parent].SortKey >= Heap[Index].SortKey)
			{
				break;
			}
			Swap(Heap[Parent], Heap[Index]);
			Index = Parent;
		}
	}

	template <typename ItemType>
	void SiftMaxHeapDown(TArray<ItemType>& Heap, int32 Index)
	{
		for (;;)
		{
			const int32 Left = Index * 2 + 1;
			if (Left >= Heap.Num())
			{
				return;
			}
			const int32 Right = Left + 1;
			const int32 Largest = Right < Heap.Num() && Heap[Right].SortKey > Heap[Left].SortKey
				? Right
				: Left;
			if (Heap[Index].SortKey >= Heap[Largest].SortKey)
			{
				return;
			}
			Swap(Heap[Index], Heap[Largest]);
			Index = Largest;
		}
	}

	template <typename ItemType>
	bool OfferDeterministicBounded(
		TArray<ItemType>& Heap,
		TSet<FString>& RetainedKeys,
		ItemType&& Candidate,
		const int32 Limit,
		bool& bOutLimitReached)
	{
		if (RetainedKeys.Contains(Candidate.SortKey))
		{
			return true;
		}
		if (Limit <= 0)
		{
			bOutLimitReached = true;
			return false;
		}
		if (Heap.Num() < Limit)
		{
			RetainedKeys.Add(Candidate.SortKey);
			Heap.Add(MoveTemp(Candidate));
			SiftMaxHeapUp(Heap, Heap.Num() - 1);
			return true;
		}

		bOutLimitReached = true;
		if (Candidate.SortKey >= Heap[0].SortKey)
		{
			return false;
		}
		RetainedKeys.Remove(Heap[0].SortKey);
		RetainedKeys.Add(Candidate.SortKey);
		Heap[0] = MoveTemp(Candidate);
		SiftMaxHeapDown(Heap, 0);
		return true;
	}

	bool IsHexDigest(const FString& Value)
	{
		if (Value.Len() != FSHA1::DigestSize * 2)
		{
			return false;
		}
		for (const TCHAR Character : Value)
		{
			if (!FChar::IsHexDigit(Character))
			{
				return false;
			}
		}
		return true;
	}

	FString StripSha1Prefix(const FString& Fingerprint)
	{
		return Fingerprint.StartsWith(TEXT("sha1:")) ? Fingerprint.RightChop(5) : Fingerprint;
	}

	FString CursorChecksum(const FString& RequestHex, const FString& SnapshotHex, const FString& Offset)
	{
		FString Canonical;
		AppendFingerprintToken(Canonical, TEXT("hyperai.dependency-cursor.v1"));
		AppendFingerprintToken(Canonical, RequestHex);
		AppendFingerprintToken(Canonical, SnapshotHex);
		AppendFingerprintToken(Canonical, Offset);
		return Sha1Utf8(Canonical);
	}

	FString MakeCursor(const FString& RequestFingerprint, const FString& SnapshotFingerprint, int32 Offset)
	{
		const FString RequestHex = StripSha1Prefix(RequestFingerprint);
		const FString SnapshotHex = StripSha1Prefix(SnapshotFingerprint);
		const FString OffsetText = FString::FromInt(Offset);
		return FString::Printf(
			TEXT("v1.%s.%s.%s.%s"),
			*RequestHex,
			*SnapshotHex,
			*OffsetText,
			*CursorChecksum(RequestHex, SnapshotHex, OffsetText));
	}

	bool ParseCursor(
		const FString& Cursor,
		const FString& RequestFingerprint,
		const FString& SnapshotFingerprint,
		int32 TotalRecords,
		int32& OutOffset,
		FString& OutErrorCode,
		FString& OutErrorMessage)
	{
		OutOffset = 0;
		OutErrorCode.Reset();
		OutErrorMessage.Reset();
		if (Cursor.IsEmpty())
		{
			return true;
		}
		if (Cursor.Len() > FHyperAIStudioDependencyGraphAnalyzer::HardMaxCursorChars || ContainsEmbeddedNull(Cursor))
		{
			OutErrorCode = TEXT("cursor_invalid_format");
			OutErrorMessage = TEXT("Cursor length or encoding is invalid.");
			return false;
		}

		TArray<FString> Parts;
		Cursor.ParseIntoArray(Parts, TEXT("."), false);
		if (Parts.Num() != 5 || Parts[0] != TEXT("v1")
			|| !IsHexDigest(Parts[1]) || !IsHexDigest(Parts[2]) || !IsHexDigest(Parts[4]))
		{
			OutErrorCode = TEXT("cursor_invalid_format");
			OutErrorMessage = TEXT("Cursor is not a valid HyperAI dependency cursor.");
			return false;
		}
		if (CursorChecksum(Parts[1], Parts[2], Parts[3]) != Parts[4])
		{
			OutErrorCode = TEXT("cursor_checksum_mismatch");
			OutErrorMessage = TEXT("Cursor integrity check failed.");
			return false;
		}
		if (Parts[1] != StripSha1Prefix(RequestFingerprint))
		{
			OutErrorCode = TEXT("cursor_request_mismatch");
			OutErrorMessage = TEXT("Cursor belongs to a different normalized request.");
			return false;
		}
		if (Parts[2] != StripSha1Prefix(SnapshotFingerprint))
		{
			OutErrorCode = TEXT("cursor_snapshot_changed");
			OutErrorMessage = TEXT("Dependency snapshot changed; restart paging without a cursor.");
			return false;
		}
		if (!LexTryParseString(OutOffset, *Parts[3]) || OutOffset < 0 || OutOffset > TotalRecords)
		{
			OutErrorCode = TEXT("cursor_offset_invalid");
			OutErrorMessage = TEXT("Cursor offset is outside the current result.");
			return false;
		}
		return true;
	}

	FString RecordCanonical(const FHyperAIStudioDependencyGraphRecord& Record)
	{
		FString Canonical;
		AppendFingerprintToken(Canonical, Record.Kind);
		AppendFingerprintToken(Canonical, Record.RecordId);
		AppendFingerprintToken(Canonical, Record.Identifier);
		AppendFingerprintToken(Canonical, FString::FromInt(Record.Depth));
		AppendFingerprintToken(Canonical, Record.bRoot ? TEXT("1") : TEXT("0"));
		AppendFingerprintToken(Canonical, Record.SourceIdentifier);
		AppendFingerprintToken(Canonical, Record.TargetIdentifier);
		AppendFingerprintToken(Canonical, Record.Relation);
		AppendFingerprintToken(Canonical, Record.Category);
		AppendFingerprintToken(Canonical, FString::Join(Record.Properties, TEXT(",")));
		AppendFingerprintToken(Canonical, Record.Reason);
		AppendFingerprintToken(Canonical, Record.bReasonAvailable ? TEXT("1") : TEXT("0"));
		AppendFingerprintToken(Canonical, Record.ComponentId);
		AppendFingerprintToken(Canonical, FString::FromInt(Record.ComponentSize));
		AppendFingerprintToken(Canonical, Record.bCycle ? TEXT("1") : TEXT("0"));
		return Canonical;
	}

	FString MakeSnapshotFingerprint(
		const TArray<FHyperAIStudioDependencyGraphRecord>& Records,
		const FHyperAIStudioDependencyGraphResult& State)
	{
		FString Canonical;
		AppendFingerprintToken(Canonical, TEXT("hyperai.dependency-snapshot.v3"));
		AppendFingerprintToken(Canonical, State.RegistryStatus);
		AppendFingerprintToken(Canonical, State.ObservationScope);
		AppendFingerprintToken(Canonical, State.bIncludesUnsavedChanges ? TEXT("1") : TEXT("0"));
		AppendFingerprintToken(Canonical, State.bRegistryLoading ? TEXT("1") : TEXT("0"));
		AppendFingerprintToken(Canonical, State.bRegistryGathering ? TEXT("1") : TEXT("0"));
		AppendFingerprintToken(Canonical, State.bSearchAllAssetsObserved ? TEXT("1") : TEXT("0"));
		AppendFingerprintToken(Canonical, State.bUpstreamRelationAllocationBounded ? TEXT("1") : TEXT("0"));
		AppendFingerprintToken(Canonical, State.bIncomplete ? TEXT("1") : TEXT("0"));
		AppendFingerprintToken(Canonical, State.bDepthLimitReached ? TEXT("1") : TEXT("0"));
		AppendFingerprintToken(Canonical, State.bNodeLimitReached ? TEXT("1") : TEXT("0"));
		AppendFingerprintToken(Canonical, State.bEdgeLimitReached ? TEXT("1") : TEXT("0"));
		AppendFingerprintToken(Canonical, State.bCaptureWorkBudgetReached ? TEXT("1") : TEXT("0"));
		AppendFingerprintToken(Canonical, LexToString(State.RawRelationRowsObserved));
		AppendFingerprintToken(Canonical, LexToString(State.RawRelationRowsProcessed));
		AppendFingerprintToken(Canonical, State.bDirtyLoadedPackageListTruncated ? TEXT("1") : TEXT("0"));
		for (const FString& DirtyPackage : State.DirtyLoadedPackages)
		{
			AppendFingerprintToken(Canonical, DirtyPackage);
		}
		for (const FHyperAIStudioDependencyGraphRecord& Record : Records)
		{
			AppendFingerprintToken(Canonical, RecordCanonical(Record));
		}
		return TEXT("sha1:") + Sha1Utf8(Canonical);
	}

	struct FTraversalArc
	{
		FString Neighbor;
		int32 EdgeIndex = INDEX_NONE;
	};

	struct FStrongComponent
	{
		TArray<FString> Members;
		FString ComponentId;
		bool bCycle = false;
	};

	TArray<FStrongComponent> FindStrongComponents(
		const TArray<FString>& Nodes,
		const TArray<FHyperAIStudioDependencyGraphSnapshotEdge>& Edges)
	{
		TArray<FStrongComponent> Result;
		if (Nodes.IsEmpty())
		{
			return Result;
		}

		TMap<FString, int32> NodeIndexes;
		for (int32 Index = 0; Index < Nodes.Num(); ++Index)
		{
			NodeIndexes.Add(Nodes[Index], Index);
		}

		TArray<TArray<int32>> Adjacency;
		TArray<TArray<int32>> ReverseAdjacency;
		Adjacency.SetNum(Nodes.Num());
		ReverseAdjacency.SetNum(Nodes.Num());
		TSet<uint64> ArcKeys;
		for (const FHyperAIStudioDependencyGraphSnapshotEdge& Edge : Edges)
		{
			const int32* SourceIndex = NodeIndexes.Find(Edge.SourceIdentifier);
			const int32* TargetIndex = NodeIndexes.Find(Edge.TargetIdentifier);
			if (!SourceIndex || !TargetIndex)
			{
				continue;
			}
			const uint64 ArcKey = (static_cast<uint64>(static_cast<uint32>(*SourceIndex)) << 32)
				| static_cast<uint32>(*TargetIndex);
			if (ArcKeys.Contains(ArcKey))
			{
				continue;
			}
			ArcKeys.Add(ArcKey);
			Adjacency[*SourceIndex].Add(*TargetIndex);
			ReverseAdjacency[*TargetIndex].Add(*SourceIndex);
		}
		for (int32 Index = 0; Index < Nodes.Num(); ++Index)
		{
			Adjacency[Index].Sort();
			ReverseAdjacency[Index].Sort();
		}

		struct FDepthFirstFrame
		{
			int32 Node = INDEX_NONE;
			int32 NextNeighbor = 0;
		};

		TArray<uint8> Visited;
		Visited.Init(0, Nodes.Num());
		TArray<int32> FinishOrder;
		FinishOrder.Reserve(Nodes.Num());
		for (int32 Start = 0; Start < Nodes.Num(); ++Start)
		{
			if (Visited[Start])
			{
				continue;
			}
			Visited[Start] = 1;
			TArray<FDepthFirstFrame> Stack;
			Stack.Add({ Start, 0 });
			while (!Stack.IsEmpty())
			{
				FDepthFirstFrame& Frame = Stack.Last();
				if (Frame.NextNeighbor < Adjacency[Frame.Node].Num())
				{
					const int32 Neighbor = Adjacency[Frame.Node][Frame.NextNeighbor++];
					if (!Visited[Neighbor])
					{
						Visited[Neighbor] = 1;
						Stack.Add({ Neighbor, 0 });
					}
				}
				else
				{
					FinishOrder.Add(Frame.Node);
					Stack.Pop();
				}
			}
		}

		TArray<uint8> Assigned;
		Assigned.Init(0, Nodes.Num());
		for (int32 OrderIndex = FinishOrder.Num() - 1; OrderIndex >= 0; --OrderIndex)
		{
			const int32 Start = FinishOrder[OrderIndex];
			if (Assigned[Start])
			{
				continue;
			}
			FStrongComponent Component;
			TArray<int32> Stack = { Start };
			Assigned[Start] = 1;
			while (!Stack.IsEmpty())
			{
				const int32 Node = Stack.Pop();
				Component.Members.Add(Nodes[Node]);
				for (const int32 Neighbor : ReverseAdjacency[Node])
				{
					if (!Assigned[Neighbor])
					{
						Assigned[Neighbor] = 1;
						Stack.Add(Neighbor);
					}
				}
			}
			Component.Members.Sort();
			Component.bCycle = Component.Members.Num() > 1;
			if (!Component.bCycle && Component.Members.Num() == 1)
			{
				const int32 SingletonIndex = NodeIndexes[Component.Members[0]];
				Component.bCycle = Adjacency[SingletonIndex].Contains(SingletonIndex);
			}
			FString ComponentCanonical;
			for (const FString& Member : Component.Members)
			{
				AppendFingerprintToken(ComponentCanonical, Member);
			}
			Component.ComponentId = TEXT("scc:sha1:") + Sha1Utf8(ComponentCanonical);
			Result.Add(MoveTemp(Component));
		}

		Result.Sort([](const FStrongComponent& Left, const FStrongComponent& Right)
		{
			return Left.Members[0] < Right.Members[0];
		});
		return Result;
	}
}

bool FHyperAIStudioDependencyGraphAnalyzer::NormalizeRequest(
	const FHyperAIStudioDependencyGraphRequest& Request,
	FHyperAIStudioNormalizedDependencyGraphRequest& OutRequest,
	FString& OutErrorCode,
	FString& OutErrorMessage)
{
	using namespace HyperAIStudio::DependencyGraph::Private;
	OutRequest = FHyperAIStudioNormalizedDependencyGraphRequest();
	OutErrorCode.Reset();
	OutErrorMessage.Reset();

	if (Request.AssetPath.IsEmpty()
		|| Request.AssetPath.Len() > HardMaxPathChars
		|| ContainsEmbeddedNull(Request.AssetPath))
	{
		OutErrorCode = TEXT("asset_path_invalid");
		OutErrorMessage = TEXT("AssetPath must be a bounded canonical /Game package or object path.");
		return false;
	}
	FString TrimmedPath = Request.AssetPath;
	TrimmedPath.TrimStartAndEndInline();
	if (TrimmedPath != Request.AssetPath || Request.AssetPath.Contains(TEXT("\\"))
		|| Request.AssetPath.Contains(TEXT("//")) || Request.AssetPath.Contains(TEXT(":")))
	{
		OutErrorCode = TEXT("asset_path_not_canonical");
		OutErrorMessage = TEXT("AssetPath contains whitespace, a backslash, duplicate slash, or subobject suffix.");
		return false;
	}

	FText PathReason;
	FString RootPackage;
	if (FPackageName::IsValidLongPackageName(Request.AssetPath, false, &PathReason))
	{
		RootPackage = Request.AssetPath;
	}
	else if (FPackageName::IsValidObjectPath(Request.AssetPath, &PathReason))
	{
		RootPackage = FPackageName::ObjectPathToPackageName(Request.AssetPath);
	}
	else
	{
		OutErrorCode = TEXT("asset_path_not_canonical");
		OutErrorMessage = FString::Printf(TEXT("AssetPath is not canonical: %s"), *PathReason.ToString().Left(256));
		return false;
	}
	if (!RootPackage.StartsWith(TEXT("/Game/"), ESearchCase::CaseSensitive)
		|| !FPackageName::IsValidLongPackageName(RootPackage, false, &PathReason))
	{
		OutErrorCode = TEXT("asset_path_outside_game");
		OutErrorMessage = TEXT("AssetPath must resolve to one project package below /Game/.");
		return false;
	}

	if (Request.Direction != TEXT("dependencies")
		&& Request.Direction != TEXT("referencers")
		&& Request.Direction != TEXT("both"))
	{
		OutErrorCode = TEXT("direction_invalid");
		OutErrorMessage = TEXT("Direction must be dependencies, referencers, or both.");
		return false;
	}
	if (Request.MaxDepth < 0 || Request.MaxDepth > HardMaxDepth)
	{
		OutErrorCode = TEXT("max_depth_out_of_range");
		OutErrorMessage = TEXT("MaxDepth must be between 0 and 16.");
		return false;
	}
	if (Request.MaxNodes < 1 || Request.MaxNodes > HardMaxNodes)
	{
		OutErrorCode = TEXT("max_nodes_out_of_range");
		OutErrorMessage = TEXT("MaxNodes must be between 1 and 4096.");
		return false;
	}
	if (Request.PageSize < 1 || Request.PageSize > HardMaxPageSize)
	{
		OutErrorCode = TEXT("page_size_out_of_range");
		OutErrorMessage = TEXT("PageSize must be between 1 and 256.");
		return false;
	}
	if (Request.Cursor.Len() > HardMaxCursorChars || ContainsEmbeddedNull(Request.Cursor))
	{
		OutErrorCode = TEXT("cursor_invalid_format");
		OutErrorMessage = TEXT("Cursor exceeds its hard bound or contains invalid data.");
		return false;
	}

	OutRequest.InputAssetPath = Request.AssetPath;
	OutRequest.RootPackage = RootPackage;
	OutRequest.Direction = Request.Direction;
	OutRequest.MaxDepth = Request.MaxDepth;
	OutRequest.MaxNodes = Request.MaxNodes;
	OutRequest.PageSize = Request.PageSize;
	OutRequest.Cursor = Request.Cursor;
	if (!NormalizeAllowlist(
		Request.Categories,
		AllowedCategories(),
		false,
		TEXT("Categories"),
		OutRequest.Categories,
		OutErrorCode,
		OutErrorMessage)
		|| !NormalizeAllowlist(
			Request.Properties,
			AllowedProperties(),
			true,
			TEXT("Properties"),
			OutRequest.Properties,
			OutErrorCode,
			OutErrorMessage))
	{
		return false;
	}
	OutRequest.RequestFingerprint = MakeRequestFingerprint(OutRequest);
	return true;
}

TArray<FHyperAIStudioDependencyGraphSnapshotEdge>
FHyperAIStudioDependencyGraphSnapshotBuilder::SelectDeterministicBoundedEdges(
	const TArray<FHyperAIStudioDependencyGraphSnapshotEdge>& Source,
	const int32 MaxEdges,
	bool& bOutTruncated)
{
	using namespace HyperAIStudio::DependencyGraph::Private;
	struct FKeyedEdge
	{
		FString SortKey;
		FHyperAIStudioDependencyGraphSnapshotEdge Edge;
	};

	bOutTruncated = false;
	const int32 Limit = FMath::Clamp(
		MaxEdges,
		0,
		FHyperAIStudioDependencyGraphAnalyzer::HardMaxEdges);
	TArray<FKeyedEdge> Heap;
	Heap.Reserve(FMath::Min(Source.Num(), Limit));
	TSet<FString> RetainedKeys;
	for (const FHyperAIStudioDependencyGraphSnapshotEdge& Edge : Source)
	{
		FKeyedEdge Candidate;
		Candidate.SortKey = EdgeKey(Edge);
		Candidate.Edge = Edge;
		OfferDeterministicBounded(
			Heap,
			RetainedKeys,
			MoveTemp(Candidate),
			Limit,
			bOutTruncated);
	}
	Heap.Sort([](const FKeyedEdge& Left, const FKeyedEdge& Right)
	{
		return Left.SortKey < Right.SortKey;
	});
	TArray<FHyperAIStudioDependencyGraphSnapshotEdge> Result;
	Result.Reserve(Heap.Num());
	for (FKeyedEdge& Entry : Heap)
	{
		Result.Add(MoveTemp(Entry.Edge));
	}
	return Result;
}

FHyperAIStudioDependencyGraphSnapshot FHyperAIStudioDependencyGraphSnapshotBuilder::Capture(
	const FHyperAIStudioNormalizedDependencyGraphRequest& Request)
{
	using namespace HyperAIStudio::DependencyGraph::Private;
	FHyperAIStudioDependencyGraphSnapshot Snapshot;
	Snapshot.RootPackage = Request.RootPackage;
	AddDiagnosticOnce(
		Snapshot.Diagnostics,
		TEXT("async_dependency_index_backend_required"),
		TEXT("warning"),
		TEXT("registry"),
		TEXT("UE 5.8 Asset Registry relation calls allocate their full raw TArray before caller bounds; live relation capture is disabled until a generation-cached bounded backend supplies an immutable snapshot."));

	if (!IsInGameThread())
	{
		Snapshot.bRegistryAvailable = false;
		Snapshot.bQueryIncomplete = true;
		AddDiagnostic(
			Snapshot.Diagnostics,
			TEXT("game_thread_required"),
			TEXT("error"),
			TEXT("registry"),
			TEXT("Fresh Asset Registry capture must run on the Unreal game thread."));
		return Snapshot;
	}

	FAssetRegistryModule* AssetRegistryModule =
		FModuleManager::GetModulePtr<FAssetRegistryModule>(TEXT("AssetRegistry"));
	if (!AssetRegistryModule)
	{
		Snapshot.bRegistryAvailable = false;
		Snapshot.bQueryIncomplete = true;
		AddDiagnostic(
			Snapshot.Diagnostics,
			TEXT("asset_registry_unavailable"),
			TEXT("error"),
			TEXT("registry"),
			TEXT("The Asset Registry module is not loaded."));
		return Snapshot;
	}

	IAssetRegistry& Registry = AssetRegistryModule->Get();
	Snapshot.bRegistryGatheringAtStart = Registry.IsGathering();
	auto CaptureRegistryEndState = [&Registry, &Snapshot]()
	{
		Snapshot.bRegistryGatheringAtEnd = Registry.IsGathering();
	};
	TSet<FString> DirtyPackages;
	auto ObserveDirtyPackage = [&Snapshot, &DirtyPackages](const FName PackageName)
	{
		if (PackageName.IsNone())
		{
			return;
		}
		const FString PackagePath = PackageName.ToString();
		UPackage* LoadedPackage = FindPackage(nullptr, *PackagePath);
		if (!LoadedPackage || !LoadedPackage->IsDirty() || DirtyPackages.Contains(PackagePath))
		{
			return;
		}
		if (DirtyPackages.Num() < FHyperAIStudioDependencyGraphAnalyzer::HardMaxDirtyLoadedPackages)
		{
			DirtyPackages.Add(PackagePath);
			return;
		}
		Snapshot.bDirtyLoadedPackageListTruncated = true;
		FString LargestRetained;
		for (const FString& Retained : DirtyPackages)
		{
			if (LargestRetained.IsEmpty() || Retained > LargestRetained)
			{
				LargestRetained = Retained;
			}
		}
		if (!LargestRetained.IsEmpty() && PackagePath < LargestRetained)
		{
			DirtyPackages.Remove(LargestRetained);
			DirtyPackages.Add(PackagePath);
		}
	};
	ObserveDirtyPackage(FName(*Request.RootPackage));
	FAssetPackageData RootPackageData;
	const UE::AssetRegistry::EExists RootPackageState = Registry.TryGetAssetPackageData(
		FName(*Request.RootPackage), RootPackageData, /*bFailIfLockHeld=*/true);
	// DoesNotExist is returned only when UE has full registry knowledge. Exists
	// alone does not prove that the global search finished, so it is not promoted
	// to SearchAllAssets evidence.
	Snapshot.bSearchAllAssetsAtStart =
		RootPackageState == UE::AssetRegistry::EExists::DoesNotExist;
	Snapshot.bSearchAllAssetsAtEnd = Snapshot.bSearchAllAssetsAtStart;
	Snapshot.bRootFound = RootPackageState == UE::AssetRegistry::EExists::Exists;
	if (RootPackageState == UE::AssetRegistry::EExists::Unknown)
	{
		Snapshot.bQueryIncomplete = true;
		AddDiagnosticOnce(
			Snapshot.Diagnostics,
			TEXT("root_package_state_unavailable"),
			TEXT("warning"),
			TEXT("asset_path"),
			TEXT("The nonblocking package query could not acquire a registry read lock or the registry has not completed its full search; root absence is not inferred."));
	}
	if (Request.InputAssetPath != Request.RootPackage)
	{
		Snapshot.bQueryIncomplete = true;
		AddDiagnosticOnce(
			Snapshot.Diagnostics,
			TEXT("object_identity_not_independently_verified"),
			TEXT("warning"),
			TEXT("asset_path"),
			TEXT("The bounded dependency graph is package-based. Exact object/class identity requires the future typed asset index and is not inferred from package existence."));
	}

	if (!Snapshot.bRootFound)
	{
		Snapshot.DirtyLoadedPackages = DirtyPackages.Array();
		Snapshot.DirtyLoadedPackages.Sort();
		CaptureRegistryEndState();
		return Snapshot;
	}

	Snapshot.bQueryIncomplete = true;
	Snapshot.DirtyLoadedPackages = DirtyPackages.Array();
	Snapshot.DirtyLoadedPackages.Sort();
	CaptureRegistryEndState();
	return Snapshot;
}

FHyperAIStudioDependencyGraphResult FHyperAIStudioDependencyGraphAnalyzer::Analyze(
	const FHyperAIStudioDependencyGraphSnapshot& Snapshot,
	const FHyperAIStudioDependencyGraphRequest& Request,
	const FHyperAIStudioNormalizedDependencyGraphRequest* PreparedRequest)
{
	using namespace HyperAIStudio::DependencyGraph::Private;
	FHyperAIStudioDependencyGraphResult Result;
	Result.PageSize = Request.PageSize;

	FHyperAIStudioNormalizedDependencyGraphRequest NormalizedStorage;
	FString ErrorCode;
	FString ErrorMessage;
	if (!PreparedRequest
		&& !NormalizeRequest(Request, NormalizedStorage, ErrorCode, ErrorMessage))
	{
		Result.Status = TEXT("invalid_request");
		Result.bIncomplete = false;
		Result.CursorStatus = Request.Cursor.IsEmpty() ? TEXT("none") : TEXT("rejected");
		Result.CursorDiagnosticCode = Request.Cursor.IsEmpty() ? FString() : ErrorCode;
		AddDiagnostic(Result.Diagnostics, ErrorCode, TEXT("error"), TEXT("request"), ErrorMessage);
		return Result;
	}
	const FHyperAIStudioNormalizedDependencyGraphRequest& Normalized =
		PreparedRequest ? *PreparedRequest : NormalizedStorage;

	Result.InputAssetPath = Normalized.InputAssetPath;
	Result.RootPackage = Normalized.RootPackage;
	Result.Direction = Normalized.Direction;
	Result.RequestFingerprint = Normalized.RequestFingerprint;
	Result.PageSize = Normalized.PageSize;
	Result.bCaptureWorkBudgetReached = Snapshot.bCaptureWorkBudgetReached;
	Result.RawRelationRowsObserved = Snapshot.RawRelationRowsObserved;
	Result.RawRelationRowsProcessed = Snapshot.RawRelationRowsProcessed;
	Result.DirtyLoadedPackages = Snapshot.DirtyLoadedPackages;
	Result.DirtyLoadedPackages.Sort();
	if (Result.DirtyLoadedPackages.Num() > HardMaxDirtyLoadedPackages)
	{
		Result.DirtyLoadedPackages.SetNum(HardMaxDirtyLoadedPackages);
		Result.bDirtyLoadedPackageListTruncated = true;
	}
	Result.bDirtyLoadedPackageListTruncated |= Snapshot.bDirtyLoadedPackageListTruncated;
	Result.DirtyLoadedPackageCount = Result.DirtyLoadedPackages.Num();
	for (const FHyperAIStudioDependencyGraphDiagnostic& Diagnostic : Snapshot.Diagnostics)
	{
		AddDiagnostic(
			Result.Diagnostics,
			Diagnostic.Code,
			Diagnostic.Severity,
			Diagnostic.Field,
			Diagnostic.Message);
	}
	AddDiagnosticOnce(
		Result.Diagnostics,
		TEXT("no_delete_inference"),
		TEXT("info"),
		TEXT("deletion_assessment"),
		Result.SafetyNotice);
	AddDiagnosticOnce(
		Result.Diagnostics,
		TEXT("observation_scope_asset_registry_on_disk"),
		TEXT("info"),
		TEXT("observation_scope"),
		TEXT("Dependency and referencer relations are the Asset Registry's indexed on-disk observation; unsaved in-memory graph changes are not included."));
	AddDiagnosticOnce(
		Result.Diagnostics,
		TEXT("upstream_relation_allocation_unbounded"),
		TEXT("warning"),
		TEXT("registry"),
		TEXT("UE 5.8 relation APIs allocate full raw arrays before HyperAI can enforce cooperative post-call checks; allocation/call latency is not hard-bounded, so packaged/live admission remains blocked."));
	if (!Result.DirtyLoadedPackages.IsEmpty())
	{
		AddDiagnosticOnce(
			Result.Diagnostics,
			TEXT("dirty_loaded_packages_excluded"),
			TEXT("warning"),
			TEXT("observation_scope"),
			TEXT("One or more loaded graph packages are dirty; their unsaved relation changes are outside this on-disk observation."));
	}
	if (Result.bDirtyLoadedPackageListTruncated)
	{
		AddDiagnosticOnce(
			Result.Diagnostics,
			TEXT("dirty_loaded_package_list_truncated"),
			TEXT("warning"),
			TEXT("dirty_loaded_packages"),
			TEXT("The dirty loaded-package evidence list reached its fixed output bound."));
	}

	if (!Snapshot.RootPackage.IsEmpty() && Snapshot.RootPackage != Normalized.RootPackage)
	{
		Result.Status = TEXT("invalid_snapshot");
		Result.RegistryStatus = TEXT("incomplete");
		Result.bIncomplete = true;
		AddDiagnostic(
			Result.Diagnostics,
			TEXT("snapshot_request_mismatch"),
			TEXT("error"),
			TEXT("root_package"),
			TEXT("The value snapshot belongs to a different root package."));
		RejectCursorWithoutStableGraph(Result, Normalized.Cursor, TEXT("Cursor cannot be used with a mismatched value snapshot."));
		return Result;
	}
	if (Snapshot.Edges.Num() > HardMaxEdges)
	{
		Result.Status = TEXT("invalid_snapshot");
		Result.RegistryStatus = TEXT("incomplete");
		Result.bIncomplete = true;
		Result.bEdgeLimitReached = true;
		Result.bGraphTruncated = true;
		Result.bTruncated = true;
		AddDiagnostic(
			Result.Diagnostics,
			TEXT("snapshot_edge_bound_exceeded"),
			TEXT("error"),
			TEXT("edges"),
			TEXT("The immutable snapshot exceeded the analyzer hard edge bound."));
		RejectCursorWithoutStableGraph(Result, Normalized.Cursor, TEXT("Cursor cannot be used with an invalid value snapshot."));
		return Result;
	}
	if (!Snapshot.bRegistryAvailable)
	{
		Result.Status = TEXT("unavailable");
		Result.RegistryStatus = TEXT("unavailable");
		Result.bIncomplete = true;
		AddDiagnosticOnce(
			Result.Diagnostics,
			TEXT("asset_registry_unavailable"),
			TEXT("error"),
			TEXT("registry"),
			TEXT("Asset Registry state is unavailable."));
		RejectCursorWithoutStableGraph(Result, Normalized.Cursor, TEXT("Cursor cannot be validated while Asset Registry state is unavailable."));
		return Result;
	}

	Result.bRegistryGathering = Snapshot.bRegistryGatheringAtStart || Snapshot.bRegistryGatheringAtEnd;
	Result.bRegistryLoading = Result.bRegistryGathering;
	Result.bSearchAllAssetsObserved = Snapshot.bSearchAllAssetsAtStart && Snapshot.bSearchAllAssetsAtEnd;
	Result.bUpstreamRelationAllocationBounded = Snapshot.bUpstreamRelationAllocationBounded;
	Result.bIncomplete = Result.bRegistryGathering
		|| !Result.bSearchAllAssetsObserved
		|| (Snapshot.bRootFound && !Result.bUpstreamRelationAllocationBounded)
		|| Snapshot.bQueryIncomplete
		|| Snapshot.bCaptureWorkBudgetReached;
	Result.RegistryStatus = Result.bRegistryGathering
		? TEXT("indexing")
		: (!Result.bSearchAllAssetsObserved
			|| (Snapshot.bRootFound && !Result.bUpstreamRelationAllocationBounded)
			|| Snapshot.bQueryIncomplete
			|| Snapshot.bCaptureWorkBudgetReached ? TEXT("incomplete") : TEXT("ready"));
	if (Result.bRegistryGathering)
	{
		AddDiagnosticOnce(
			Result.Diagnostics,
			TEXT("registry_indexing"),
			TEXT("warning"),
			TEXT("registry"),
			TEXT("Asset Registry is still indexing; this graph is explicitly incomplete."));
	}
	if (!Result.bSearchAllAssetsObserved)
	{
		AddDiagnosticOnce(
			Result.Diagnostics,
			TEXT("registry_full_search_not_observed"),
			TEXT("warning"),
			TEXT("registry"),
			TEXT("SearchAllAssets was not already observed at both capture boundaries; dependency and referencer absence is incomplete."));
	}
	if (Snapshot.bRootFound && !Result.bUpstreamRelationAllocationBounded)
	{
		AddDiagnosticOnce(
			Result.Diagnostics,
			TEXT("async_dependency_index_backend_required"),
			TEXT("warning"),
			TEXT("registry"),
			TEXT("No bounded immutable relation cache is attached; UE's raw dependency arrays are not queried and the graph remains partial."));
	}
	if (Snapshot.bQueryIncomplete)
	{
		AddDiagnosticOnce(
			Result.Diagnostics,
			TEXT("registry_query_incomplete"),
			TEXT("warning"),
			TEXT("registry"),
			TEXT("At least one direct-edge query was unavailable or omitted invalid evidence."));
	}
	if (Snapshot.bCaptureWorkBudgetReached)
	{
		AddDiagnosticOnce(
			Result.Diagnostics,
			TEXT("capture_work_budget_reached"),
			TEXT("warning"),
			TEXT("registry"),
			TEXT("Dependency capture reached its fixed raw-row or elapsed-time budget; the on-disk graph is partial."));
	}

	if (!Snapshot.bRootFound)
	{
		if (Result.bIncomplete)
		{
			Result.Status = TEXT("partial");
			AddDiagnostic(
				Result.Diagnostics,
				TEXT("root_unconfirmed_while_incomplete"),
				TEXT("warning"),
				TEXT("asset_path"),
				TEXT("The requested asset is not present in the current incomplete registry snapshot."));
		}
		else
		{
			Result.Status = TEXT("not_found");
			AddDiagnostic(
				Result.Diagnostics,
				TEXT("root_not_found"),
				TEXT("error"),
				TEXT("asset_path"),
				TEXT("The requested asset is not present in the current Asset Registry state."));
		}
		RejectCursorWithoutStableGraph(Result, Normalized.Cursor, TEXT("Cursor cannot be validated without a confirmed root asset."));
		return Result;
	}

	struct FKeyedCanonicalEdge
	{
		FString Key;
		FHyperAIStudioDependencyGraphSnapshotEdge Edge;
	};
	TArray<FKeyedCanonicalEdge> CandidateEdges;
	CandidateEdges.Reserve(Snapshot.Edges.Num());
	bool bInvalidSnapshotEdgeOmitted = false;
	for (const FHyperAIStudioDependencyGraphSnapshotEdge& SourceEdge : Snapshot.Edges)
	{
		if (SourceEdge.SourceIdentifier.IsEmpty() || SourceEdge.TargetIdentifier.IsEmpty()
			|| SourceEdge.SourceIdentifier.Len() > HardMaxPathChars
			|| SourceEdge.TargetIdentifier.Len() > HardMaxPathChars
			|| ContainsEmbeddedNull(SourceEdge.SourceIdentifier)
			|| ContainsEmbeddedNull(SourceEdge.TargetIdentifier)
			|| !AllowedCategories().Contains(SourceEdge.Category)
			|| !Normalized.Categories.Contains(SourceEdge.Category))
		{
			bInvalidSnapshotEdgeOmitted = true;
			continue;
		}
		FHyperAIStudioDependencyGraphSnapshotEdge Edge = SourceEdge;
		TSet<FString> PropertySet;
		bool bPropertiesValid = true;
		for (const FString& Property : Edge.Properties)
		{
			if (!AllowedProperties().Contains(Property))
			{
				bPropertiesValid = false;
				break;
			}
			PropertySet.Add(Property);
		}
		if (!bPropertiesValid)
		{
			bInvalidSnapshotEdgeOmitted = true;
			continue;
		}
		Edge.Properties = PropertySet.Array();
		Edge.Properties.Sort();
		if (!MatchesPropertyAllowlist(Edge.Properties, Normalized.Properties))
		{
			continue;
		}
		Edge.Reason = Edge.Reason.Left(MaxReasonChars);
		Edge.bReasonAvailable = Edge.bReasonAvailable && !Edge.Reason.IsEmpty();
		if (!Edge.bReasonAvailable)
		{
			Edge.Reason.Reset();
		}
		FKeyedCanonicalEdge& Candidate = CandidateEdges.AddDefaulted_GetRef();
		Candidate.Key = EdgeKey(Edge);
		Candidate.Edge = MoveTemp(Edge);
	}
	CandidateEdges.Sort([](
		const FKeyedCanonicalEdge& Left,
		const FKeyedCanonicalEdge& Right)
	{
		if (Left.Key != Right.Key)
		{
			return Left.Key < Right.Key;
		}
		if (Left.Edge.bReasonAvailable != Right.Edge.bReasonAvailable)
		{
			return Left.Edge.bReasonAvailable;
		}
		return Left.Edge.Reason < Right.Edge.Reason;
	});
	TArray<FHyperAIStudioDependencyGraphSnapshotEdge> Edges;
	FString PreviousEdgeKey;
	for (FKeyedCanonicalEdge& Candidate : CandidateEdges)
	{
		if (Edges.IsEmpty() || Candidate.Key != PreviousEdgeKey)
		{
			PreviousEdgeKey = MoveTemp(Candidate.Key);
			Edges.Add(MoveTemp(Candidate.Edge));
		}
	}
	if (bInvalidSnapshotEdgeOmitted)
	{
		Result.bIncomplete = true;
		Result.RegistryStatus = Result.bRegistryLoading ? TEXT("indexing") : TEXT("incomplete");
		AddDiagnosticOnce(
			Result.Diagnostics,
			TEXT("snapshot_edge_omitted"),
			TEXT("warning"),
			TEXT("edges"),
			TEXT("An invalid or out-of-contract snapshot edge was omitted."));
	}

	TMap<FString, TArray<FTraversalArc>> Traversal;
	for (int32 EdgeIndex = 0; EdgeIndex < Edges.Num(); ++EdgeIndex)
	{
		const FHyperAIStudioDependencyGraphSnapshotEdge& Edge = Edges[EdgeIndex];
		if (Normalized.Direction == TEXT("dependencies") || Normalized.Direction == TEXT("both"))
		{
			Traversal.FindOrAdd(Edge.SourceIdentifier).Add({ Edge.TargetIdentifier, EdgeIndex });
		}
		if (Normalized.Direction == TEXT("referencers") || Normalized.Direction == TEXT("both"))
		{
			Traversal.FindOrAdd(Edge.TargetIdentifier).Add({ Edge.SourceIdentifier, EdgeIndex });
		}
	}
	for (TPair<FString, TArray<FTraversalArc>>& Pair : Traversal)
	{
		Pair.Value.Sort([](const FTraversalArc& Left, const FTraversalArc& Right)
		{
			if (Left.Neighbor != Right.Neighbor)
			{
				return Left.Neighbor < Right.Neighbor;
			}
			return Left.EdgeIndex < Right.EdgeIndex;
		});
	}

	TMap<FString, int32> DepthByIdentifier;
	DepthByIdentifier.Add(Normalized.RootPackage, 0);
	TArray<FString> Queue = { Normalized.RootPackage };
	bool bAnalysisDepthLimitReached = false;
	bool bAnalysisNodeLimitReached = false;
	for (int32 QueueIndex = 0; QueueIndex < Queue.Num(); ++QueueIndex)
	{
		const FString Current = Queue[QueueIndex];
		const int32 CurrentDepth = DepthByIdentifier[Current];
		const TArray<FTraversalArc>* Arcs = Traversal.Find(Current);
		if (!Arcs)
		{
			continue;
		}
		for (const FTraversalArc& Arc : *Arcs)
		{
			if (DepthByIdentifier.Contains(Arc.Neighbor))
			{
				continue;
			}
			if (CurrentDepth >= Normalized.MaxDepth)
			{
				bAnalysisDepthLimitReached = true;
				continue;
			}
			if (DepthByIdentifier.Num() >= Normalized.MaxNodes)
			{
				bAnalysisNodeLimitReached = true;
				continue;
			}
			DepthByIdentifier.Add(Arc.Neighbor, CurrentDepth + 1);
			Queue.Add(Arc.Neighbor);
		}
	}

	TArray<FString> Nodes;
	DepthByIdentifier.GenerateKeyArray(Nodes);
	Nodes.Sort();
	TArray<FHyperAIStudioDependencyGraphSnapshotEdge> GraphEdges;
	for (const FHyperAIStudioDependencyGraphSnapshotEdge& Edge : Edges)
	{
		if (DepthByIdentifier.Contains(Edge.SourceIdentifier)
			&& DepthByIdentifier.Contains(Edge.TargetIdentifier))
		{
			GraphEdges.Add(Edge);
		}
	}

	Result.bDepthLimitReached = Snapshot.bCaptureDepthLimitReached || bAnalysisDepthLimitReached;
	Result.bNodeLimitReached = Snapshot.bCaptureNodeLimitReached || bAnalysisNodeLimitReached;
	Result.bEdgeLimitReached = Snapshot.bCaptureEdgeLimitReached;
	Result.bCaptureWorkBudgetReached = Snapshot.bCaptureWorkBudgetReached;
	Result.bGraphTruncated = Result.bDepthLimitReached
		|| Result.bNodeLimitReached
		|| Result.bEdgeLimitReached
		|| Result.bCaptureWorkBudgetReached;
	Result.NodeCount = Nodes.Num();
	Result.EdgeCount = GraphEdges.Num();
	if (Result.bDepthLimitReached)
	{
		AddDiagnosticOnce(
			Result.Diagnostics,
			TEXT("max_depth_reached"),
			TEXT("warning"),
			TEXT("max_depth"),
			TEXT("Matching neighbors exist beyond MaxDepth and were omitted."));
	}
	if (Result.bNodeLimitReached)
	{
		AddDiagnosticOnce(
			Result.Diagnostics,
			TEXT("max_nodes_reached"),
			TEXT("warning"),
			TEXT("max_nodes"),
			TEXT("Matching graph identifiers exceeded MaxNodes and were omitted."));
	}
	if (Result.bEdgeLimitReached)
	{
		AddDiagnosticOnce(
			Result.Diagnostics,
			TEXT("hard_edge_limit_reached"),
			TEXT("warning"),
			TEXT("edges"),
			TEXT("The fixed internal edge safety limit was reached."));
	}

	const TArray<FStrongComponent> Components = FindStrongComponents(Nodes, GraphEdges);
	Result.StronglyConnectedComponentCount = Components.Num();
	for (const FStrongComponent& Component : Components)
	{
		Result.CycleComponentCount += Component.bCycle ? 1 : 0;
	}

	TArray<FHyperAIStudioDependencyGraphRecord> AllRecords;
	AllRecords.Reserve(Nodes.Num() + GraphEdges.Num() + Nodes.Num());
	for (const FString& Node : Nodes)
	{
		FHyperAIStudioDependencyGraphRecord& Record = AllRecords.AddDefaulted_GetRef();
		Record.Kind = TEXT("node");
		Record.RecordId = TEXT("node:sha1:") + Sha1Utf8(Node);
		Record.Identifier = Node;
		Record.Depth = DepthByIdentifier[Node];
		Record.bRoot = Node == Normalized.RootPackage;
	}
	for (const FHyperAIStudioDependencyGraphSnapshotEdge& Edge : GraphEdges)
	{
		FHyperAIStudioDependencyGraphRecord& Record = AllRecords.AddDefaulted_GetRef();
		Record.Kind = TEXT("edge");
		Record.RecordId = TEXT("edge:sha1:") + Sha1Utf8(EdgeKey(Edge));
		Record.SourceIdentifier = Edge.SourceIdentifier;
		Record.TargetIdentifier = Edge.TargetIdentifier;
		Record.Relation = TEXT("source_depends_on_target");
		Record.Category = Edge.Category;
		Record.Properties = Edge.Properties;
		Record.Reason = Edge.Reason;
		Record.bReasonAvailable = Edge.bReasonAvailable;
	}
	for (const FStrongComponent& Component : Components)
	{
		if (!Component.bCycle)
		{
			continue;
		}
		for (const FString& Member : Component.Members)
		{
			FHyperAIStudioDependencyGraphRecord& Record = AllRecords.AddDefaulted_GetRef();
			Record.Kind = TEXT("scc_member");
			Record.RecordId = TEXT("scc-member:sha1:") + Sha1Utf8(Component.ComponentId + TEXT("|") + Member);
			Record.Identifier = Member;
			Record.ComponentId = Component.ComponentId;
			Record.ComponentSize = Component.Members.Num();
			Record.bCycle = true;
		}
	}

	Result.TotalRecords = AllRecords.Num();
	Result.SnapshotFingerprint = MakeSnapshotFingerprint(
		AllRecords,
		Result);
	Result.Status = Result.bIncomplete || Result.bGraphTruncated
		? TEXT("partial")
		: TEXT("complete_on_disk");
	if (Result.bCaptureWorkBudgetReached && !Normalized.Cursor.IsEmpty())
	{
		Result.Status = TEXT("invalid_request");
		Result.CursorStatus = TEXT("rejected");
		Result.CursorDiagnosticCode = TEXT("cursor_unstable_work_budget");
		Result.bTruncated = true;
		AddDiagnosticOnce(
			Result.Diagnostics,
			TEXT("cursor_unstable_work_budget"),
			TEXT("error"),
			TEXT("cursor"),
			TEXT("Continuation is disabled for elapsed-time/raw-work partial captures because a fresh capture can stop at a different boundary."));
		return Result;
	}

	int32 Offset = 0;
	if (!ParseCursor(
		Normalized.Cursor,
		Normalized.RequestFingerprint,
		Result.SnapshotFingerprint,
		AllRecords.Num(),
		Offset,
		ErrorCode,
		ErrorMessage))
	{
		Result.Status = TEXT("invalid_request");
		Result.CursorStatus = TEXT("rejected");
		Result.CursorDiagnosticCode = ErrorCode;
		Result.PageOffset = 0;
		Result.ReturnedRecords = 0;
		Result.bHasMore = false;
		Result.bTruncated = Result.bGraphTruncated;
		AddDiagnostic(Result.Diagnostics, ErrorCode, TEXT("error"), TEXT("cursor"), ErrorMessage);
		return Result;
	}

	Result.CursorStatus = Normalized.Cursor.IsEmpty() ? TEXT("none") : TEXT("accepted");
	if (!Normalized.Cursor.IsEmpty())
	{
		Result.CursorDiagnosticCode = TEXT("cursor_accepted");
		AddDiagnosticOnce(
			Result.Diagnostics,
			TEXT("cursor_accepted"),
			TEXT("info"),
			TEXT("cursor"),
			TEXT("Cursor matches this normalized request and unchanged dependency snapshot."));
	}
	Result.PageOffset = Offset;
	const int32 PageEnd = FMath::Min(Offset + Normalized.PageSize, AllRecords.Num());
	for (int32 Index = Offset; Index < PageEnd; ++Index)
	{
		Result.Records.Add(AllRecords[Index]);
	}
	Result.ReturnedRecords = Result.Records.Num();
	Result.bHasMore = PageEnd < AllRecords.Num();
	if (Result.bHasMore && !Result.bCaptureWorkBudgetReached)
	{
		Result.NextCursor = MakeCursor(
			Normalized.RequestFingerprint,
			Result.SnapshotFingerprint,
			PageEnd);
	}
	else if (Result.bHasMore)
	{
		Result.CursorDiagnosticCode = TEXT("continuation_suppressed_unstable_capture");
		AddDiagnosticOnce(
			Result.Diagnostics,
			TEXT("continuation_suppressed_unstable_capture"),
			TEXT("warning"),
			TEXT("cursor"),
			TEXT("More bounded records exist, but no cursor is emitted because this work-budget partial capture is not a stable continuation source."));
	}
	Result.bTruncated = Result.bGraphTruncated || Result.ReturnedRecords < Result.TotalRecords;
	if (Result.ReturnedRecords < Result.TotalRecords && !Result.bCaptureWorkBudgetReached)
	{
		AddDiagnosticOnce(
			Result.Diagnostics,
			TEXT("result_paginated"),
			TEXT("info"),
			TEXT("page_size"),
			TEXT("This response contains a deterministic page of the bounded graph records."));
	}
	return Result;
}

FHyperAIStudioDependencyGraphResult UHyperAIStudioDependencyGraphToolset::hyper_asset_dependency_graph(
	const FHyperAIStudioDependencyGraphRequest& Request)
{
	FHyperAIStudioNormalizedDependencyGraphRequest Normalized;
	FString ErrorCode;
	FString ErrorMessage;
	if (!FHyperAIStudioDependencyGraphAnalyzer::NormalizeRequest(
		Request,
		Normalized,
		ErrorCode,
		ErrorMessage))
	{
		return FHyperAIStudioDependencyGraphAnalyzer::Analyze(
			FHyperAIStudioDependencyGraphSnapshot(),
			Request);
	}
	const FHyperAIStudioDependencyGraphSnapshot Snapshot =
		FHyperAIStudioDependencyGraphSnapshotBuilder::Capture(Normalized);
	return FHyperAIStudioDependencyGraphAnalyzer::Analyze(Snapshot, Request, &Normalized);
}
