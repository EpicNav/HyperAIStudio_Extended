// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"

enum class EHyperAIStudioContextSnapshotStatus : uint8
{
	Complete,
	Partial,
	Unavailable
};

struct FHyperAIStudioContextSnapshotDiagnostic
{
	FString Code;
	FString Field;
	FString Message;
};

/**
 * Value-only input for the context projection seam. UObject and editor access must finish before
 * an instance crosses the game-thread boundary.
 */
struct FHyperAIStudioContextSnapshotInput
{
	FString CapturedAtUtc;
	uint64 CaptureFrameNumber = 0;

	bool bActorSelectionAvailable = false;
	int32 SelectedActorTotal = INDEX_NONE;
	TArray<FString> SelectedActorPaths;
	bool bActorSourceTruncated = false;

	bool bViewportAvailable = false;
	FTransform ViewportTransform = FTransform::Identity;

	bool bAssetSelectionAvailable = false;
	int32 SelectedAssetTotal = INDEX_NONE;
	TArray<FString> SelectedAssetPackagePaths;
	bool bAssetSourceTruncated = false;

	TArray<FHyperAIStudioContextSnapshotDiagnostic> Diagnostics;
};

struct FHyperAIStudioContextSnapshotLimits
{
	static constexpr int32 HardMaxSelectedActors = 256;
	static constexpr int32 HardMaxSelectedAssets = 256;
	static constexpr int32 HardMaxSourceItemsScanned = 512;
	static constexpr int32 HardMaxPathChars = 4096;
	static constexpr int32 HardMaxDiagnostics = 32;
	static constexpr int32 HardMaxOutputBytes = 256 * 1024;

	int32 MaxSelectedActors = 64;
	int32 MaxSelectedAssets = 64;
	int32 MaxPathChars = 1024;
	int32 MaxDiagnostics = 16;
	int32 MaxOutputBytes = 32 * 1024;

	bool IsValid(FString& OutError) const;
};

/** A bounded, immutable-by-convention value result suitable for worker-thread serialization. */
struct FHyperAIStudioContextSnapshotResult
{
	static constexpr const TCHAR* SchemaVersion = TEXT("hyperai.context-snapshot.v1");

	EHyperAIStudioContextSnapshotStatus Status = EHyperAIStudioContextSnapshotStatus::Unavailable;
	FString CapturedAtUtc;
	uint64 CaptureFrameNumber = 0;

	bool bActorSelectionAvailable = false;
	int32 SelectedActorTotal = 0;
	TArray<FString> SelectedActorPaths;
	bool bActorsTruncated = false;

	bool bViewportAvailable = false;
	FTransform ViewportTransform = FTransform::Identity;

	bool bAssetSelectionAvailable = false;
	int32 SelectedAssetTotal = 0;
	TArray<FString> SelectedAssetPackagePaths;
	bool bAssetsTruncated = false;

	TArray<FHyperAIStudioContextSnapshotDiagnostic> Diagnostics;
	bool bDiagnosticsTruncated = false;
	bool bOutputBudgetTruncated = false;
	int32 OutputBudgetBytes = 0;
};

/**
 * Read-only compound candidate implementation seam. It is deliberately not a ToolsetDefinition
 * and is not registered or advertised until packaged equivalence and benchmark gates pass.
 */
class FHyperAIStudioContextSnapshotCandidate
{
public:
	/** Captures editor state on the game thread, then applies the pure bounded projection. */
	static bool CaptureFromEditor(
		const FHyperAIStudioContextSnapshotLimits& Limits,
		FHyperAIStudioContextSnapshotResult& OutResult,
		FString& OutError);

	/** Pure/value-only projection. Safe to call from any thread. */
	static bool Project(
		const FHyperAIStudioContextSnapshotInput& Input,
		const FHyperAIStudioContextSnapshotLimits& Limits,
		FHyperAIStudioContextSnapshotResult& OutResult,
		FString& OutError);

	/** Deterministic compact JSON for a projected result. */
	static bool SerializeJson(
		const FHyperAIStudioContextSnapshotResult& Result,
		FString& OutJson,
		FString& OutError);

	static FString StatusToString(EHyperAIStudioContextSnapshotStatus Status);
};
