// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioLevelDesignToolset.h"

class AActor;
class UWorld;

/** The only code that measures pawns, touches blockout actors, traces the level or renders its preview. */
namespace HyperAIStudio::LevelDesign::Gate
{
	UWorld* GetEditorWorld();

	/** False with OutError only when PawnClassPath names something that is not a pawn class. */
	bool MeasurePlayer(UWorld& World, const FString& PawnClassPath, FHyperAIPlayerMetrics& OutMetrics, FString& OutError);
	/** The sizes that follow from a player's height, width, crouch and jump. */
	void DeriveRecommendations(FHyperAIPlayerMetrics& Metrics);

	/** Pieces this toolset placed, by name. */
	TArray<FHyperAILevelDesignPiece> ReadPieces(UWorld& World);
	/** Level path plus every piece's parameters and actor transforms. */
	FString ComputeRevision(UWorld& World);

	/** Checks every op against the pieces as they would be at that point; changes nothing. */
	bool ValidateOps(UWorld& World, const TArray<FHyperAILevelDesignOp>& Ops, float MaxStepHeight, float MaxSlopeDegrees,
		TArray<FString>& OutNotes, FString& OutError);
	/** Applies ops in order as one undo step. */
	bool ApplyOps(UWorld& World, const TArray<FHyperAILevelDesignOp>& Ops, float MaxStepHeight, float MaxSlopeDegrees,
		int32& OutActorsChanged, FString& OutError);

	/** Floor, clearance, routes, widths, sightlines and cover against the metrics. */
	void CheckLayout(UWorld& World, const FHyperAIPlayerMetrics& Metrics, float MaxSightline, FHyperAILevelDesignValidateReport& Report);
	/** Top-down: height in grey from a depth capture, with markers, routes and issues drawn on. */
	bool WritePreview(UWorld& World, const FHyperAILevelDesignValidateReport& Report, FString& OutPath, FString& OutError);

	/** Where a world position lands in a square top-down image of Bounds; exposed for tests. */
	FIntPoint ToPixel(const FVector& World, const FBox& Bounds, int32 ImageSize);
}
