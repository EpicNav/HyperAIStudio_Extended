// Games by Hyper 2026.

#include "HyperAIStudioContextSnapshot.h"

#include "AssetRegistry/AssetData.h"
#include "ContentBrowserModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "GameFramework/Actor.h"
#include "IContentBrowserSingleton.h"
#include "LevelEditorViewport.h"
#include "Modules/ModuleManager.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Selection.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace HyperAIStudio::ContextSnapshot::Private
{
	constexpr int32 MinimumOutputBytes = 2048;
	constexpr int32 MaxDiagnosticCodeChars = 64;
	constexpr int32 MaxDiagnosticFieldChars = 64;
	constexpr int32 MaxDiagnosticMessageChars = 256;

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

	bool IsCanonicalUtcTimestamp(const FString& Value)
	{
		if (Value.Len() != 24
			|| Value[4] != TEXT('-')
			|| Value[7] != TEXT('-')
			|| Value[10] != TEXT('T')
			|| Value[13] != TEXT(':')
			|| Value[16] != TEXT(':')
			|| Value[19] != TEXT('.')
			|| Value[23] != TEXT('Z'))
		{
			return false;
		}
		for (int32 Index = 0; Index < Value.Len(); ++Index)
		{
			if (Index == 4 || Index == 7 || Index == 10 || Index == 13
				|| Index == 16 || Index == 19 || Index == 23)
			{
				continue;
			}
			if (!FChar::IsDigit(Value[Index]))
			{
				return false;
			}
		}
		FDateTime Parsed;
		return FDateTime::ParseIso8601(*Value, Parsed) && Parsed.ToIso8601() == Value;
	}

	bool IsFiniteVector(const FVector& Value)
	{
		return FMath::IsFinite(Value.X) && FMath::IsFinite(Value.Y) && FMath::IsFinite(Value.Z);
	}

	bool IsFiniteRotator(const FRotator& Value)
	{
		return FMath::IsFinite(Value.Pitch) && FMath::IsFinite(Value.Yaw) && FMath::IsFinite(Value.Roll);
	}

	bool IsFiniteTransform(const FTransform& Value)
	{
		return IsFiniteVector(Value.GetLocation())
			&& IsFiniteRotator(Value.Rotator())
			&& IsFiniteVector(Value.GetScale3D());
	}

	void AddDiagnostic(
		FHyperAIStudioContextSnapshotInput& Input,
		const TCHAR* Code,
		const TCHAR* Field,
		const TCHAR* Message)
	{
		FHyperAIStudioContextSnapshotDiagnostic& Diagnostic = Input.Diagnostics.AddDefaulted_GetRef();
		Diagnostic.Code = Code;
		Diagnostic.Field = Field;
		Diagnostic.Message = Message;
	}

	TSharedRef<FJsonObject> MakeVectorObject(const FVector& Value)
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetNumberField(TEXT("x"), Value.X);
		Object->SetNumberField(TEXT("y"), Value.Y);
		Object->SetNumberField(TEXT("z"), Value.Z);
		return Object;
	}

	TSharedRef<FJsonObject> MakeRotatorObject(const FRotator& Value)
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetNumberField(TEXT("pitch"), Value.Pitch);
		Object->SetNumberField(TEXT("yaw"), Value.Yaw);
		Object->SetNumberField(TEXT("roll"), Value.Roll);
		return Object;
	}

	TSharedRef<FJsonObject> MakeCollectionObject(
		bool bAvailable,
		int32 Total,
		bool bTruncated,
		const TArray<FString>& Values,
		const TCHAR* ItemField)
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetBoolField(TEXT("available"), bAvailable);
		Object->SetNumberField(TEXT("total"), Total);
		Object->SetNumberField(TEXT("returned"), Values.Num());
		Object->SetBoolField(TEXT("truncated"), bTruncated);

		TArray<TSharedPtr<FJsonValue>> Items;
		Items.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(ItemField, Value);
			Items.Add(MakeShared<FJsonValueObject>(Item));
		}
		Object->SetArrayField(TEXT("items"), Items);
		return Object;
	}

	bool SerializeUnbounded(
		const FHyperAIStudioContextSnapshotResult& Result,
		FString& OutJson)
	{
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("schema_version"), FHyperAIStudioContextSnapshotResult::SchemaVersion);
		Root->SetStringField(TEXT("status"), FHyperAIStudioContextSnapshotCandidate::StatusToString(Result.Status));
		Root->SetStringField(TEXT("captured_at_utc"), Result.CapturedAtUtc);
		Root->SetStringField(TEXT("capture_frame"), LexToString(Result.CaptureFrameNumber));
		Root->SetObjectField(TEXT("selected_actors"), MakeCollectionObject(
			Result.bActorSelectionAvailable,
			Result.SelectedActorTotal,
			Result.bActorsTruncated,
			Result.SelectedActorPaths,
			TEXT("object_path")));

		TSharedRef<FJsonObject> Viewport = MakeShared<FJsonObject>();
		Viewport->SetBoolField(TEXT("available"), Result.bViewportAvailable);
		if (Result.bViewportAvailable)
		{
			TSharedRef<FJsonObject> Transform = MakeShared<FJsonObject>();
			Transform->SetObjectField(TEXT("location"), MakeVectorObject(Result.ViewportTransform.GetLocation()));
			Transform->SetObjectField(TEXT("rotation"), MakeRotatorObject(Result.ViewportTransform.Rotator()));
			Transform->SetObjectField(TEXT("scale"), MakeVectorObject(Result.ViewportTransform.GetScale3D()));
			Viewport->SetObjectField(TEXT("transform"), Transform);
		}
		Root->SetObjectField(TEXT("viewport"), Viewport);

		Root->SetObjectField(TEXT("selected_assets"), MakeCollectionObject(
			Result.bAssetSelectionAvailable,
			Result.SelectedAssetTotal,
			Result.bAssetsTruncated,
			Result.SelectedAssetPackagePaths,
			TEXT("package_path")));

		TArray<TSharedPtr<FJsonValue>> Diagnostics;
		Diagnostics.Reserve(Result.Diagnostics.Num());
		for (const FHyperAIStudioContextSnapshotDiagnostic& Diagnostic : Result.Diagnostics)
		{
			TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("code"), Diagnostic.Code);
			Object->SetStringField(TEXT("field"), Diagnostic.Field);
			Object->SetStringField(TEXT("message"), Diagnostic.Message);
			Diagnostics.Add(MakeShared<FJsonValueObject>(Object));
		}
		Root->SetArrayField(TEXT("diagnostics"), Diagnostics);

		TSharedRef<FJsonObject> Bounds = MakeShared<FJsonObject>();
		Bounds->SetNumberField(TEXT("output_budget_bytes"), Result.OutputBudgetBytes);
		Bounds->SetBoolField(TEXT("output_budget_truncated"), Result.bOutputBudgetTruncated);
		Bounds->SetBoolField(TEXT("diagnostics_truncated"), Result.bDiagnosticsTruncated);
		Root->SetObjectField(TEXT("bounds"), Bounds);

		OutJson.Reset();
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutJson);
		return FJsonSerializer::Serialize(Root, Writer);
	}

	int32 Utf8Bytes(const FString& Value)
	{
		return FTCHARToUTF8(*Value).Length();
	}

	bool IsValidIdentity(const FString& Value, int32 MaxChars)
	{
		if (Value.IsEmpty() || Value.Len() > MaxChars || ContainsEmbeddedNull(Value))
		{
			return false;
		}
		FString Trimmed = Value;
		Trimmed.TrimStartAndEndInline();
		return Trimmed == Value;
	}

	void CopyDiagnosticBounded(
		const FHyperAIStudioContextSnapshotDiagnostic& Source,
		FHyperAIStudioContextSnapshotDiagnostic& Target,
		bool& bOutTruncated)
	{
		Target.Code = Source.Code.Left(MaxDiagnosticCodeChars);
		Target.Field = Source.Field.Left(MaxDiagnosticFieldChars);
		Target.Message = Source.Message.Left(MaxDiagnosticMessageChars);
		bOutTruncated |= Target.Code.Len() != Source.Code.Len()
			|| Target.Field.Len() != Source.Field.Len()
			|| Target.Message.Len() != Source.Message.Len();
	}

	void RecomputeStatus(FHyperAIStudioContextSnapshotResult& Result)
	{
		const int32 AvailableFields = static_cast<int32>(Result.bActorSelectionAvailable)
			+ static_cast<int32>(Result.bViewportAvailable)
			+ static_cast<int32>(Result.bAssetSelectionAvailable);
		if (AvailableFields == 0)
		{
			Result.Status = EHyperAIStudioContextSnapshotStatus::Unavailable;
		}
		else if (AvailableFields < 3
			|| Result.bActorsTruncated
			|| Result.bAssetsTruncated
			|| Result.Diagnostics.Num() > 0
			|| Result.bDiagnosticsTruncated
			|| Result.bOutputBudgetTruncated)
		{
			Result.Status = EHyperAIStudioContextSnapshotStatus::Partial;
		}
		else
		{
			Result.Status = EHyperAIStudioContextSnapshotStatus::Complete;
		}
	}

	bool ValidateProjectedResult(
		const FHyperAIStudioContextSnapshotResult& Result,
		FString& OutError)
	{
		if (!IsCanonicalUtcTimestamp(Result.CapturedAtUtc) || Result.CaptureFrameNumber == 0)
		{
			OutError = TEXT("Projected snapshot freshness evidence is invalid.");
			return false;
		}
		if (Result.OutputBudgetBytes < MinimumOutputBytes
			|| Result.OutputBudgetBytes > FHyperAIStudioContextSnapshotLimits::HardMaxOutputBytes)
		{
			OutError = TEXT("Projected snapshot output budget is invalid.");
			return false;
		}
			auto ValidateCollection = [&](bool bAvailable, int32 Total, int32 Returned, bool bTruncated, const TCHAR* Field)
			{
				if (Total < 0 || Returned < 0 || (bAvailable && Total < Returned))
			{
				OutError = FString::Printf(TEXT("%s collection counts are inconsistent."), Field);
					return false;
				}
				if (bAvailable && Total > Returned && !bTruncated)
				{
					OutError = FString::Printf(TEXT("%s omits source items without a truncation flag."), Field);
					return false;
				}
			if (!bAvailable && (Total != 0 || Returned != 0 || bTruncated))
			{
				OutError = FString::Printf(TEXT("Unavailable %s cannot claim items, totals, or truncation."), Field);
				return false;
			}
			return true;
		};
		if (!ValidateCollection(
			Result.bActorSelectionAvailable,
			Result.SelectedActorTotal,
			Result.SelectedActorPaths.Num(),
			Result.bActorsTruncated,
			TEXT("selected_actors"))
			|| !ValidateCollection(
				Result.bAssetSelectionAvailable,
				Result.SelectedAssetTotal,
				Result.SelectedAssetPackagePaths.Num(),
				Result.bAssetsTruncated,
				TEXT("selected_assets")))
		{
			return false;
		}
		if (Result.bViewportAvailable && !IsFiniteTransform(Result.ViewportTransform))
		{
			OutError = TEXT("Available viewport transform must be finite.");
			return false;
		}
		if (!Result.bViewportAvailable && !Result.ViewportTransform.Equals(FTransform::Identity))
		{
			OutError = TEXT("Unavailable viewport cannot claim a transform.");
			return false;
		}
		FHyperAIStudioContextSnapshotResult Expected = Result;
		RecomputeStatus(Expected);
		if (Expected.Status != Result.Status)
		{
			OutError = TEXT("Projected snapshot status contradicts its availability or truncation fields.");
			return false;
		}
		return true;
	}
}

bool FHyperAIStudioContextSnapshotLimits::IsValid(FString& OutError) const
{
	OutError.Reset();
	if (MaxSelectedActors < 0 || MaxSelectedActors > HardMaxSelectedActors)
	{
		OutError = TEXT("MaxSelectedActors is outside the supported range.");
		return false;
	}
	if (MaxSelectedAssets < 0 || MaxSelectedAssets > HardMaxSelectedAssets)
	{
		OutError = TEXT("MaxSelectedAssets is outside the supported range.");
		return false;
	}
	if (MaxPathChars < 1 || MaxPathChars > HardMaxPathChars)
	{
		OutError = TEXT("MaxPathChars is outside the supported range.");
		return false;
	}
	if (MaxDiagnostics < 3 || MaxDiagnostics > HardMaxDiagnostics)
	{
		OutError = TEXT("MaxDiagnostics is outside the supported range.");
		return false;
	}
	if (MaxOutputBytes < HyperAIStudio::ContextSnapshot::Private::MinimumOutputBytes
		|| MaxOutputBytes > HardMaxOutputBytes)
	{
		OutError = TEXT("MaxOutputBytes is outside the supported range.");
		return false;
	}
	return true;
}

bool FHyperAIStudioContextSnapshotCandidate::CaptureFromEditor(
	const FHyperAIStudioContextSnapshotLimits& Limits,
	FHyperAIStudioContextSnapshotResult& OutResult,
	FString& OutError)
{
	using namespace HyperAIStudio::ContextSnapshot::Private;
	OutResult = FHyperAIStudioContextSnapshotResult();
	OutError.Reset();
	if (!IsInGameThread())
	{
		OutError = TEXT("Editor context capture is only permitted on the Unreal game thread.");
		return false;
	}
	if (!Limits.IsValid(OutError))
	{
		return false;
	}

	FHyperAIStudioContextSnapshotInput Input;
	Input.CapturedAtUtc = FDateTime::UtcNow().ToIso8601();
	Input.CaptureFrameNumber = GFrameCounter;

	if (!GEditor)
	{
		AddDiagnostic(Input, TEXT("editor_unavailable"), TEXT("selected_actors"), TEXT("The Unreal editor is unavailable."));
	}
	else if (USelection* Selection = GEditor->GetSelectedActors())
	{
		Input.bActorSelectionAvailable = true;
		Input.SelectedActorTotal = Selection->Num();
		int32 Scanned = 0;
		for (FSelectionIterator Iterator(*Selection);
			Iterator && Scanned < FHyperAIStudioContextSnapshotLimits::HardMaxSourceItemsScanned;
			++Iterator, ++Scanned)
		{
			if (const AActor* Actor = Cast<AActor>(*Iterator))
			{
				if (Input.SelectedActorPaths.Num() < Limits.MaxSelectedActors)
				{
					Input.SelectedActorPaths.Add(Actor->GetPathName());
				}
			}
			else
			{
				AddDiagnostic(Input, TEXT("invalid_actor_reference"), TEXT("selected_actors"), TEXT("A selected object was not an actor."));
			}
		}
		Input.bActorSourceTruncated = Input.SelectedActorTotal > Scanned
			|| Input.SelectedActorTotal > Input.SelectedActorPaths.Num();
	}
	else
	{
		AddDiagnostic(Input, TEXT("actor_selection_unavailable"), TEXT("selected_actors"), TEXT("Actor selection is unavailable."));
	}

	if (GCurrentLevelEditingViewportClient)
	{
		Input.bViewportAvailable = true;
		Input.ViewportTransform = FTransform(
			GCurrentLevelEditingViewportClient->GetViewRotation(),
			GCurrentLevelEditingViewportClient->GetViewLocation());
	}
	else
	{
		AddDiagnostic(Input, TEXT("viewport_unavailable"), TEXT("viewport"), TEXT("No current level viewport camera is available."));
	}

	// A read hot path must never synchronously load an editor module. If Content Browser
	// is not already resident, return explicit unavailability and let a later read retry.
	if (FContentBrowserModule* ContentBrowser =
		FModuleManager::GetModulePtr<FContentBrowserModule>(TEXT("ContentBrowser")))
	{
		Input.bAssetSelectionAvailable = true;
		TArray<FAssetData> SelectedAssets;
		ContentBrowser->Get().GetAllSelectedAssets(SelectedAssets);
		Input.SelectedAssetTotal = SelectedAssets.Num();
		const int32 ScanCount = FMath::Min(
			SelectedAssets.Num(),
			FHyperAIStudioContextSnapshotLimits::HardMaxSourceItemsScanned);
		for (int32 Index = 0; Index < ScanCount && Input.SelectedAssetPackagePaths.Num() < Limits.MaxSelectedAssets; ++Index)
		{
			if (SelectedAssets[Index].IsValid())
			{
				Input.SelectedAssetPackagePaths.Add(SelectedAssets[Index].PackageName.ToString());
			}
			else
			{
				AddDiagnostic(Input, TEXT("invalid_asset_reference"), TEXT("selected_assets"), TEXT("A selected Content Browser item was not a valid asset."));
			}
		}
		Input.bAssetSourceTruncated = Input.SelectedAssetTotal > ScanCount
			|| Input.SelectedAssetTotal > Input.SelectedAssetPackagePaths.Num();
	}
	else
	{
		AddDiagnostic(Input, TEXT("asset_selection_unavailable"), TEXT("selected_assets"), TEXT("The Content Browser module is unavailable."));
	}

	return Project(Input, Limits, OutResult, OutError);
}

bool FHyperAIStudioContextSnapshotCandidate::Project(
	const FHyperAIStudioContextSnapshotInput& Input,
	const FHyperAIStudioContextSnapshotLimits& Limits,
	FHyperAIStudioContextSnapshotResult& OutResult,
	FString& OutError)
{
	using namespace HyperAIStudio::ContextSnapshot::Private;
	OutResult = FHyperAIStudioContextSnapshotResult();
	OutError.Reset();
	if (!Limits.IsValid(OutError))
	{
		return false;
	}
	if (ContainsEmbeddedNull(Input.CapturedAtUtc) || !IsCanonicalUtcTimestamp(Input.CapturedAtUtc))
	{
		OutError = TEXT("CapturedAtUtc must be a canonical millisecond ISO-8601 UTC timestamp.");
		return false;
	}
	if (Input.CaptureFrameNumber == 0)
	{
		OutError = TEXT("CaptureFrameNumber must provide non-zero Unreal frame evidence.");
		return false;
	}
	if (Input.SelectedActorTotal < INDEX_NONE || Input.SelectedAssetTotal < INDEX_NONE)
	{
		OutError = TEXT("Selection totals cannot be less than INDEX_NONE.");
		return false;
	}
	if ((Input.SelectedActorTotal != INDEX_NONE && Input.SelectedActorTotal < Input.SelectedActorPaths.Num())
		|| (Input.SelectedAssetTotal != INDEX_NONE && Input.SelectedAssetTotal < Input.SelectedAssetPackagePaths.Num()))
	{
		OutError = TEXT("A selection total cannot be smaller than its captured value count.");
		return false;
	}
	if (Input.bActorSelectionAvailable && Input.SelectedActorTotal == INDEX_NONE)
	{
		OutError = TEXT("Available actor selection requires an explicit source total.");
		return false;
	}
	if (!Input.bActorSelectionAvailable
		&& (Input.SelectedActorTotal > 0
			|| Input.SelectedActorPaths.Num() > 0
			|| Input.bActorSourceTruncated))
	{
		OutError = TEXT("Unavailable actor selection cannot claim items, totals, or truncation.");
		return false;
	}
	if (Input.bAssetSelectionAvailable && Input.SelectedAssetTotal == INDEX_NONE)
	{
		OutError = TEXT("Available asset selection requires an explicit source total.");
		return false;
	}
	if (!Input.bAssetSelectionAvailable
		&& (Input.SelectedAssetTotal > 0
			|| Input.SelectedAssetPackagePaths.Num() > 0
			|| Input.bAssetSourceTruncated))
	{
		OutError = TEXT("Unavailable asset selection cannot claim items, totals, or truncation.");
		return false;
	}
	if (!Input.bViewportAvailable && !Input.ViewportTransform.Equals(FTransform::Identity))
	{
		OutError = TEXT("Unavailable viewport cannot claim a non-identity transform.");
		return false;
	}

	OutResult.CapturedAtUtc = Input.CapturedAtUtc;
	OutResult.CaptureFrameNumber = Input.CaptureFrameNumber;
	OutResult.bActorSelectionAvailable = Input.bActorSelectionAvailable;
	OutResult.bViewportAvailable = Input.bViewportAvailable && IsFiniteTransform(Input.ViewportTransform);
	OutResult.bAssetSelectionAvailable = Input.bAssetSelectionAvailable;
	OutResult.ViewportTransform = OutResult.bViewportAvailable ? Input.ViewportTransform : FTransform::Identity;
	OutResult.OutputBudgetBytes = Limits.MaxOutputBytes;

	const int32 ActorScanCount = FMath::Min(
		Input.SelectedActorPaths.Num(),
		FHyperAIStudioContextSnapshotLimits::HardMaxSourceItemsScanned);
	for (int32 Index = 0; Index < ActorScanCount && OutResult.SelectedActorPaths.Num() < Limits.MaxSelectedActors; ++Index)
	{
		const FString& Path = Input.SelectedActorPaths[Index];
		if (IsValidIdentity(Path, Limits.MaxPathChars))
		{
			OutResult.SelectedActorPaths.Add(Path);
		}
		else
		{
			OutResult.bActorsTruncated = true;
		}
	}
	OutResult.SelectedActorTotal = Input.bActorSelectionAvailable ? Input.SelectedActorTotal : 0;
	OutResult.bActorsTruncated |= Input.bActorSourceTruncated
		|| Input.SelectedActorPaths.Num() > ActorScanCount
		|| OutResult.SelectedActorTotal > OutResult.SelectedActorPaths.Num();

	const int32 AssetScanCount = FMath::Min(
		Input.SelectedAssetPackagePaths.Num(),
		FHyperAIStudioContextSnapshotLimits::HardMaxSourceItemsScanned);
	TArray<FString> ValidAssetPaths;
	ValidAssetPaths.Reserve(FMath::Min(AssetScanCount, Limits.MaxSelectedAssets));
	for (int32 Index = 0; Index < AssetScanCount; ++Index)
	{
		const FString& Path = Input.SelectedAssetPackagePaths[Index];
		if (IsValidIdentity(Path, Limits.MaxPathChars))
		{
			ValidAssetPaths.Add(Path);
		}
		else
		{
			OutResult.bAssetsTruncated = true;
		}
	}
	ValidAssetPaths.Sort();
	if (ValidAssetPaths.Num() > Limits.MaxSelectedAssets)
	{
		ValidAssetPaths.SetNum(Limits.MaxSelectedAssets);
		OutResult.bAssetsTruncated = true;
	}
	OutResult.SelectedAssetPackagePaths = MoveTemp(ValidAssetPaths);
	OutResult.SelectedAssetTotal = Input.bAssetSelectionAvailable ? Input.SelectedAssetTotal : 0;
	OutResult.bAssetsTruncated |= Input.bAssetSourceTruncated
		|| Input.SelectedAssetPackagePaths.Num() > AssetScanCount
		|| OutResult.SelectedAssetTotal > OutResult.SelectedAssetPackagePaths.Num();

	if (Input.bViewportAvailable && !OutResult.bViewportAvailable)
	{
		FHyperAIStudioContextSnapshotDiagnostic InvalidViewport;
		InvalidViewport.Code = TEXT("invalid_viewport_transform");
		InvalidViewport.Field = TEXT("viewport");
		InvalidViewport.Message = TEXT("The viewport transform contained a non-finite value.");
		if (OutResult.Diagnostics.Num() < Limits.MaxDiagnostics)
		{
			OutResult.Diagnostics.Add(MoveTemp(InvalidViewport));
		}
		else
		{
			OutResult.bDiagnosticsTruncated = true;
		}
	}
	TSet<int32> PrioritizedDiagnosticIndexes;
	const int32 SourceDiagnosticScanCount = FMath::Min(
		Input.Diagnostics.Num(),
		FHyperAIStudioContextSnapshotLimits::HardMaxDiagnostics);
	auto AddUnavailableDiagnostic = [&](const TCHAR* Field, const TCHAR* GenericCode, const TCHAR* GenericMessage)
	{
		for (int32 Index = 0; Index < SourceDiagnosticScanCount; ++Index)
		{
			if (Input.Diagnostics[Index].Field == Field && !Input.Diagnostics[Index].Code.IsEmpty())
			{
				FHyperAIStudioContextSnapshotDiagnostic Bounded;
				CopyDiagnosticBounded(Input.Diagnostics[Index], Bounded, OutResult.bDiagnosticsTruncated);
				OutResult.Diagnostics.Add(MoveTemp(Bounded));
				PrioritizedDiagnosticIndexes.Add(Index);
				return;
			}
		}
		OutResult.Diagnostics.Add({ GenericCode, Field, GenericMessage });
	};
	if (!Input.bActorSelectionAvailable)
	{
		AddUnavailableDiagnostic(
			TEXT("selected_actors"),
			TEXT("selected_actors_unavailable"),
			TEXT("Selected actor state is unavailable."));
	}
	if (!Input.bViewportAvailable)
	{
		AddUnavailableDiagnostic(
			TEXT("viewport"),
			TEXT("viewport_unavailable"),
			TEXT("Viewport camera state is unavailable."));
	}
	if (!Input.bAssetSelectionAvailable)
	{
		AddUnavailableDiagnostic(
			TEXT("selected_assets"),
			TEXT("selected_assets_unavailable"),
			TEXT("Selected asset state is unavailable."));
	}

	for (int32 Index = 0; Index < SourceDiagnosticScanCount; ++Index)
	{
		if (PrioritizedDiagnosticIndexes.Contains(Index))
		{
			continue;
		}
		if (OutResult.Diagnostics.Num() >= Limits.MaxDiagnostics)
		{
			OutResult.bDiagnosticsTruncated = true;
			break;
		}
		FHyperAIStudioContextSnapshotDiagnostic Bounded;
		CopyDiagnosticBounded(Input.Diagnostics[Index], Bounded, OutResult.bDiagnosticsTruncated);
		OutResult.Diagnostics.Add(MoveTemp(Bounded));
	}
	OutResult.bDiagnosticsTruncated |= Input.Diagnostics.Num() > SourceDiagnosticScanCount;

	RecomputeStatus(OutResult);
	FString Json;
	if (!SerializeUnbounded(OutResult, Json))
	{
		OutError = TEXT("Could not serialize the context snapshot.");
		return false;
	}

	while (Utf8Bytes(Json) > Limits.MaxOutputBytes)
	{
		OutResult.bOutputBudgetTruncated = true;
		if (OutResult.SelectedAssetPackagePaths.Num() > 0 || OutResult.SelectedActorPaths.Num() > 0)
		{
			const int32 LastAssetLength = OutResult.SelectedAssetPackagePaths.Num() > 0
				? OutResult.SelectedAssetPackagePaths.Last().Len()
				: -1;
			const int32 LastActorLength = OutResult.SelectedActorPaths.Num() > 0
				? OutResult.SelectedActorPaths.Last().Len()
				: -1;
			if (LastAssetLength >= LastActorLength)
			{
				OutResult.SelectedAssetPackagePaths.Pop();
				OutResult.bAssetsTruncated = true;
			}
			else
			{
				OutResult.SelectedActorPaths.Pop();
				OutResult.bActorsTruncated = true;
			}
		}
		else if (OutResult.Diagnostics.Num() > 0)
		{
			OutResult.Diagnostics.Pop();
			OutResult.bDiagnosticsTruncated = true;
		}
		else
		{
			OutError = TEXT("The minimum context snapshot exceeds MaxOutputBytes.");
			return false;
		}
		RecomputeStatus(OutResult);
		if (!SerializeUnbounded(OutResult, Json))
		{
			OutError = TEXT("Could not serialize the bounded context snapshot.");
			return false;
		}
	}

	RecomputeStatus(OutResult);
	return ValidateProjectedResult(OutResult, OutError);
}

bool FHyperAIStudioContextSnapshotCandidate::SerializeJson(
	const FHyperAIStudioContextSnapshotResult& Result,
	FString& OutJson,
	FString& OutError)
{
	OutError.Reset();
	if (!HyperAIStudio::ContextSnapshot::Private::ValidateProjectedResult(Result, OutError))
	{
		OutJson.Reset();
		return false;
	}
	if (!HyperAIStudio::ContextSnapshot::Private::SerializeUnbounded(Result, OutJson))
	{
		OutError = TEXT("Could not serialize the context snapshot.");
		return false;
	}
	if (HyperAIStudio::ContextSnapshot::Private::Utf8Bytes(OutJson) > Result.OutputBudgetBytes)
	{
		OutError = TEXT("The serialized snapshot exceeds its declared output budget.");
		OutJson.Reset();
		return false;
	}
	return true;
}

FString FHyperAIStudioContextSnapshotCandidate::StatusToString(EHyperAIStudioContextSnapshotStatus Status)
{
	switch (Status)
	{
	case EHyperAIStudioContextSnapshotStatus::Complete:
		return TEXT("complete");
	case EHyperAIStudioContextSnapshotStatus::Partial:
		return TEXT("partial");
	case EHyperAIStudioContextSnapshotStatus::Unavailable:
	default:
		return TEXT("unavailable");
	}
}
