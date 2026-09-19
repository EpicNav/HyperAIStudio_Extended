// Games by Hyper 2026.

#include "HyperAIStudioResultPreview.h"

#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/ObjectThumbnail.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "ObjectTools.h"
#include "RenderingThread.h"

namespace
{
	constexpr uint32 PreviewSize = 512;
}

FString FHyperAIStudioResultPreview::GetAssetPreviewPath(const FString& ObjectPath)
{
	// "/Game/FX/M_Fire.M_Fire" -> "Game_FX_M_Fire.M_Fire_<hash>.png": readable, flat, and the hash keeps
	// two paths that sanitise to the same name apart.
	FString Name = ObjectPath;
	Name.RemoveFromStart(TEXT("/"));
	for (TCHAR& Char : Name)
	{
		if (!FChar::IsAlnum(Char) && Char != TEXT('_') && Char != TEXT('.') && Char != TEXT('-'))
		{
			Char = TEXT('_');
		}
	}
	const FString Hash = FMD5::HashAnsiString(*ObjectPath).Left(8);
	return FPaths::ConvertRelativePathToFull(
		FPaths::ProjectSavedDir() / TEXT("HyperAIStudio/Previews") / Name.Left(160) + TEXT("_") + Hash + TEXT(".png"));
}

bool FHyperAIStudioResultPreview::WriteAssetPreview(UObject& Asset, FString& OutPath, FString& OutError)
{
	check(IsInGameThread());
	OutPath = GetAssetPreviewPath(Asset.GetPathName());

	FlushRenderingCommands();
	FObjectThumbnail Thumbnail;
	ThumbnailTools::RenderThumbnail(&Asset, PreviewSize, PreviewSize,
		ThumbnailTools::EThumbnailTextureFlushMode::NeverFlush, nullptr, &Thumbnail);
	const int32 Width = Thumbnail.GetImageWidth();
	const int32 Height = Thumbnail.GetImageHeight();
	const TArray<uint8>& Bytes = Thumbnail.GetUncompressedImageData();
	if (Width <= 0 || Height <= 0 || Bytes.Num() != Width * Height * 4)
	{
		OutError = TEXT("preview_render_failed");
		return false;
	}

	// Thumbnail pixels are BGRA8, which is FColor's layout; some renderers leave alpha at zero.
	TArray<FColor> Pixels;
	Pixels.SetNumUninitialized(Width * Height);
	FMemory::Memcpy(Pixels.GetData(), Bytes.GetData(), Bytes.Num());
	for (FColor& Pixel : Pixels)
	{
		Pixel.A = 255;
	}
	TArray64<uint8> Png;
	FImageUtils::PNGCompressImageArray(Width, Height, TArrayView64<const FColor>(Pixels.GetData(), Pixels.Num()), Png);
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(OutPath), true);
	if (Png.IsEmpty() || !FFileHelper::SaveArrayToFile(Png, *OutPath))
	{
		OutError = TEXT("preview_write_failed");
		return false;
	}
	return true;
}
