// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioLightingToolset.h"

class AActor;
class UWorld;

/** The only code that reads pixels or touches lighting actors; the toolset handles contracts and execution. */
namespace HyperAIStudio::Lighting::Gate
{
	/** Pixels are display-referred sRGB (a viewport capture or an 8-bit image). */
	FHyperAILightingImageMetrics MeasurePixels(TConstArrayView<FColor> Pixels, int32 Width, int32 Height);
	/** Earth mover's distance between two histograms of equal length, 0 (same) to 1. */
	float HistogramDistance(const TArray<float>& A, const TArray<float>& B);
	/** Fills the deltas, bMatched and ranked suggestions from two measurements. */
	void CompareMetrics(const FHyperAILightingImageMetrics& Reference, const FHyperAILightingImageMetrics& Current,
		bool bExposureLocked, float MatchThreshold, FHyperAILightingCompareReport& Report);

	/** PNG, JPG, BMP or TGA inside the project folder. */
	bool LoadProjectImage(const FString& Path, TArray<FColor>& OutPixels, int32& OutWidth, int32& OutHeight, FString& OutError);
	/** Redraws the active level viewport and reads it back. */
	bool CaptureActiveViewport(TArray<FColor>& OutPixels, int32& OutWidth, int32& OutHeight, FString& OutError);
	bool WritePng(const FString& Path, TConstArrayView<FColor> Pixels, int32 Width, int32 Height);

	UWorld* GetEditorWorld();
	/** Directional lights, sky lights, height fogs, sky atmospheres and unbound post-process volumes, in a stable order. */
	TArray<FHyperAILightingActorRecord> ReadActors(UWorld& World);
	/** Level path plus every value ReadActors reports; any lighting change moves it. */
	FString ComputeRevision(UWorld& World);
	/** True when the level viewport uses a fixed exposure or an unbound post-process volume locks it. */
	bool IsExposureLocked(UWorld& World);

	/** Checks every op against the level as it would be at that point in the plan; changes nothing. */
	bool ValidateOps(UWorld& World, const TArray<FHyperAILightingOp>& Ops, FString& OutError);
	/** Applies ops in order as one undo step, adding a missing actor where an op needs one. */
	bool ApplyOps(UWorld& World, const TArray<FHyperAILightingOp>& Ops, TArray<AActor*>& OutTouched, FString& OutError);
	/** The actors ops resolve to now, without adding any. */
	TArray<AActor*> ResolveTargets(UWorld& World, const TArray<FHyperAILightingOp>& Ops);
	/** Packages holding these actors: their own under One File Per Actor, otherwise the level's. */
	TArray<UPackage*> PackagesOf(const TArray<AActor*>& Actors);
}
