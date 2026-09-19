// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"

/**
 * PNG previews of edited assets under Saved/HyperAIStudio/Previews, so an agent can open the result with
 * its own image reader. Epic's MCP bridge returns images as base64 inside JSON text, which an agent cannot
 * see as an image, so a file path is what makes a result visible.
 */
class HYPERAISTUDIO_API FHyperAIStudioResultPreview
{
public:
	/** Deterministic, so an edit report can name the file before its async work writes it. */
	static FString GetAssetPreviewPath(const FString& ObjectPath);

	/** Game thread only. Renders the editor thumbnail at 512 px; callers wait for compiles to finish first. */
	static bool WriteAssetPreview(UObject& Asset, FString& OutPath, FString& OutError);
};
