// Games by Hyper 2026.

#include "HyperAIStudioViewportCaptureToolset.h"

#include "ImageUtils.h"
#include "LevelEditorViewport.h"
#include "Misc/DateTime.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Misc/SecureHash.h"
#include "UnrealClient.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

#include UE_INLINE_GENERATED_CPP_BY_NAME(HyperAIStudioViewportCaptureToolset)

namespace HyperAIStudio::ViewportCapture::Private
{
	FCriticalSection AuthorizationMutex;
	TSharedPtr<IHyperAIStudioViewportCaptureAuthorizationGate, ESPMode::ThreadSafe> AuthorizationGate;

	FHyperAIStudioDiagnosticsViewportCaptureResult ErrorResult(
		const FString& Status,
		const FString& Code,
		const FString& Field,
		const FString& Message)
	{
		FHyperAIStudioDiagnosticsViewportCaptureResult Result;
		Result.Status = Status;
		FHyperAIStudioDiagnosticsCommon::AddDiagnostic(
			Result.Diagnostics, Code, TEXT("error"), Field, Message);
		return Result;
	}

	FString Sha1Bytes(const TArray<uint8>& Bytes)
	{
		uint8 Digest[FSHA1::DigestSize];
		FSHA1::HashBuffer(Bytes.GetData(), Bytes.Num(), Digest);
		FString Hex;
		Hex.Reserve(FSHA1::DigestSize * 2);
		for (const uint8 Byte : Digest)
		{
			Hex += FString::Printf(TEXT("%02x"), Byte);
		}
		return TEXT("sha1:") + Hex;
	}

#if PLATFORM_WINDOWS
	FString NormalizeFinalPath(FString Path)
	{
		Path.ReplaceInline(TEXT("/"), TEXT("\\"));
		if (Path.StartsWith(TEXT("\\\\?\\UNC\\")))
		{
			Path = TEXT("\\\\") + Path.RightChop(8);
		}
		else if (Path.StartsWith(TEXT("\\\\?\\")))
		{
			Path.RightChopInline(4);
		}
		while (Path.Len() > 3 && Path.EndsWith(TEXT("\\")))
		{
			Path.LeftChopInline(1);
		}
		return Path.ToLower();
	}

	bool FinalPathFromHandle(HANDLE Handle, FString& OutPath)
	{
		OutPath.Reset();
		const DWORD Required = GetFinalPathNameByHandleW(
			Handle, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
		if (Required == 0 || Required > 32768)
		{
			return false;
		}
		TArray<WCHAR> Buffer;
		Buffer.SetNumZeroed(static_cast<int32>(Required) + 1);
		const DWORD Written = GetFinalPathNameByHandleW(
			Handle, Buffer.GetData(), Required + 1, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
		if (Written == 0 || Written > Required)
		{
			return false;
		}
		OutPath = NormalizeFinalPath(FString(static_cast<int32>(Written), Buffer.GetData()));
		return !OutPath.IsEmpty();
	}

	bool IsDirectChildPath(const FString& Child, const FString& Parent)
	{
		if (!Child.StartsWith(Parent + TEXT("\\")))
		{
			return false;
		}
		const FString Relative = Child.RightChop(Parent.Len() + 1);
		return !Relative.IsEmpty() && !Relative.Contains(TEXT("\\"));
	}

	bool MarkHandleForDelete(HANDLE Handle)
	{
		// Aggregate initialization avoids Unreal's DeleteFile -> DeleteFileW wrapper macro
		// rewriting the Windows SDK field name while retaining same-handle delete-on-close.
		FILE_DISPOSITION_INFO Disposition { 1 };
		return SetFileInformationByHandle(
			Handle, FileDispositionInfo, &Disposition, sizeof(Disposition)) != 0;
	}

	HANDLE OpenPinnedDirectory(const FString& Path, FString& OutFinalPath)
	{
		HANDLE Handle = CreateFileW(
			*Path,
			FILE_READ_ATTRIBUTES,
			FILE_SHARE_READ | FILE_SHARE_WRITE,
			nullptr,
			OPEN_EXISTING,
			FILE_FLAG_BACKUP_SEMANTICS,
			nullptr);
		if (Handle == INVALID_HANDLE_VALUE)
		{
			return INVALID_HANDLE_VALUE;
		}
		BY_HANDLE_FILE_INFORMATION Info = {};
		if (!GetFileInformationByHandle(Handle, &Info)
			|| (Info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0
			|| !FinalPathFromHandle(Handle, OutFinalPath))
		{
			CloseHandle(Handle);
			return INVALID_HANDLE_VALUE;
		}
		return Handle;
	}

	bool CreateAndPinDirectChildDirectory(
		const FString& ParentPath,
		const FString& ExpectedFinalParent,
		const FString& ChildName,
		FString& OutChildPath,
		FString& OutFinalChild,
		HANDLE& OutChildHandle,
		FString& OutErrorCode)
	{
		OutChildHandle = INVALID_HANDLE_VALUE;
		OutChildPath = FPaths::Combine(ParentPath, ChildName);
		if (!CreateDirectoryW(*OutChildPath, nullptr))
		{
			const DWORD Error = GetLastError();
			if (Error != ERROR_ALREADY_EXISTS)
			{
				OutErrorCode = TEXT("capture_directory_create_failed");
				return false;
			}
		}
		OutChildHandle = OpenPinnedDirectory(OutChildPath, OutFinalChild);
		if (OutChildHandle == INVALID_HANDLE_VALUE
			|| !IsDirectChildPath(OutFinalChild, ExpectedFinalParent))
		{
			if (OutChildHandle != INVALID_HANDLE_VALUE)
			{
				CloseHandle(OutChildHandle);
				OutChildHandle = INVALID_HANDLE_VALUE;
			}
			OutErrorCode = TEXT("capture_directory_containment_failed");
			return false;
		}
		return true;
	}

	bool EnsureCaptureDirectory(
		const FString& SavedRoot,
		FString& OutCaptureDirectory,
		FString& OutErrorCode)
	{
		FString FinalSaved;
		HANDLE SavedHandle = OpenPinnedDirectory(SavedRoot, FinalSaved);
		if (SavedHandle == INVALID_HANDLE_VALUE)
		{
			OutErrorCode = TEXT("saved_root_handle_failed");
			return false;
		}
		FString StudioDirectory;
		FString FinalStudio;
		HANDLE StudioHandle = INVALID_HANDLE_VALUE;
		if (!CreateAndPinDirectChildDirectory(
			SavedRoot,
			FinalSaved,
			TEXT("HyperAIStudio"),
			StudioDirectory,
			FinalStudio,
			StudioHandle,
			OutErrorCode))
		{
			CloseHandle(SavedHandle);
			return false;
		}
		FString FinalCapture;
		HANDLE CaptureHandle = INVALID_HANDLE_VALUE;
		const bool bCaptureReady = CreateAndPinDirectChildDirectory(
			StudioDirectory,
			FinalStudio,
			TEXT("Captures"),
			OutCaptureDirectory,
			FinalCapture,
			CaptureHandle,
			OutErrorCode);
		if (CaptureHandle != INVALID_HANDLE_VALUE)
		{
			CloseHandle(CaptureHandle);
		}
		CloseHandle(StudioHandle);
		CloseHandle(SavedHandle);
		return bCaptureReady;
	}

	bool WriteCaptureSameHandle(
		const FString& SavedRoot,
		const FString& CaptureDirectory,
		const FString& FileName,
		const TArray<uint8>& Bytes,
		FString& OutAbsolutePath,
		FString& OutErrorCode)
	{
		OutAbsolutePath.Reset();
		OutErrorCode.Reset();
		FString FinalSaved;
		HANDLE SavedHandle = OpenPinnedDirectory(SavedRoot, FinalSaved);
		if (SavedHandle == INVALID_HANDLE_VALUE)
		{
			OutErrorCode = TEXT("saved_root_handle_failed");
			return false;
		}
		FString FinalDirectory;
		HANDLE DirectoryHandle = OpenPinnedDirectory(CaptureDirectory, FinalDirectory);
		if (DirectoryHandle == INVALID_HANDLE_VALUE)
		{
			CloseHandle(SavedHandle);
			OutErrorCode = TEXT("capture_directory_handle_failed");
			return false;
		}
		const bool bRootsValid = FinalDirectory == FinalSaved + TEXT("\\hyperaistudio\\captures");
		if (!bRootsValid)
		{
			CloseHandle(DirectoryHandle);
			CloseHandle(SavedHandle);
			OutErrorCode = TEXT("capture_directory_outside_saved_root");
			return false;
		}

		const FString Candidate = FPaths::Combine(CaptureDirectory, FileName);
		HANDLE FileHandle = CreateFileW(
			*Candidate,
			GENERIC_WRITE | FILE_READ_ATTRIBUTES | DELETE,
			0,
			nullptr,
			CREATE_NEW,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
			nullptr);
		if (FileHandle == INVALID_HANDLE_VALUE)
		{
			const DWORD CreateError = GetLastError();
			CloseHandle(DirectoryHandle);
			CloseHandle(SavedHandle);
			OutErrorCode = CreateError == ERROR_FILE_EXISTS
				? TEXT("capture_name_collision") : TEXT("capture_file_create_failed");
			return false;
		}
		FString FinalFile;
		if (!FinalPathFromHandle(FileHandle, FinalFile) || !IsDirectChildPath(FinalFile, FinalDirectory))
		{
			const bool bCleanupScheduled = MarkHandleForDelete(FileHandle);
			CloseHandle(FileHandle);
			CloseHandle(DirectoryHandle);
			CloseHandle(SavedHandle);
			OutErrorCode = bCleanupScheduled
				? TEXT("capture_file_containment_failed")
				: TEXT("capture_partial_file_cleanup_unconfirmed");
			return false;
		}

		int64 Offset = 0;
		while (Offset < Bytes.Num())
		{
			const DWORD Chunk = static_cast<DWORD>(FMath::Min<int64>(Bytes.Num() - Offset, 1024 * 1024));
			DWORD Written = 0;
			if (!WriteFile(FileHandle, Bytes.GetData() + Offset, Chunk, &Written, nullptr) || Written != Chunk)
			{
				const bool bCleanupScheduled = MarkHandleForDelete(FileHandle);
				CloseHandle(FileHandle);
				CloseHandle(DirectoryHandle);
				CloseHandle(SavedHandle);
				OutErrorCode = bCleanupScheduled
					? TEXT("capture_file_write_failed")
					: TEXT("capture_partial_file_cleanup_unconfirmed");
				return false;
			}
			Offset += Written;
		}
		if (!FlushFileBuffers(FileHandle))
		{
			const bool bCleanupScheduled = MarkHandleForDelete(FileHandle);
			CloseHandle(FileHandle);
			CloseHandle(DirectoryHandle);
			CloseHandle(SavedHandle);
			OutErrorCode = bCleanupScheduled
				? TEXT("capture_file_flush_failed")
				: TEXT("capture_partial_file_cleanup_unconfirmed");
			return false;
		}
		LARGE_INTEGER FileSize = {};
		if (!GetFileSizeEx(FileHandle, &FileSize) || FileSize.QuadPart != Bytes.Num())
		{
			const bool bCleanupScheduled = MarkHandleForDelete(FileHandle);
			CloseHandle(FileHandle);
			CloseHandle(DirectoryHandle);
			CloseHandle(SavedHandle);
			OutErrorCode = bCleanupScheduled
				? TEXT("capture_file_size_mismatch")
				: TEXT("capture_partial_file_cleanup_unconfirmed");
			return false;
		}
		CloseHandle(FileHandle);
		CloseHandle(DirectoryHandle);
		CloseHandle(SavedHandle);
		OutAbsolutePath = FPaths::ConvertRelativePathToFull(Candidate);
		return true;
	}
#endif
}

FHyperAIStudioViewportCaptureGrantIssuer::FHyperAIStudioViewportCaptureGrantIssuer()
	: FHyperAIStudioViewportCaptureGrantIssuer([] { return FDateTime::UtcNow(); })
{
}

FHyperAIStudioViewportCaptureGrantIssuer::FHyperAIStudioViewportCaptureGrantIssuer(
	TFunction<FDateTime()> InUtcNow)
	: UtcNow(MoveTemp(InUtcNow))
{
	if (!UtcNow)
	{
		UtcNow = [] { return FDateTime::UtcNow(); };
	}
}

FHyperAIStudioViewportCaptureGrantIssuer::~FHyperAIStudioViewportCaptureGrantIssuer()
{
	Shutdown();
}

bool FHyperAIStudioViewportCaptureGrantIssuer::IssueForRequest(
	const FHyperAIStudioDiagnosticsNormalizedViewportCaptureRequest& Request,
	const FTimespan Lifetime,
	FHyperAIStudioViewportCaptureGrant& OutGrant,
	FString& OutErrorCode,
	FString& OutError)
{
	OutGrant = FHyperAIStudioViewportCaptureGrant();
	OutErrorCode.Reset();
	OutError.Reset();

	// Re-normalize a server-owned projection so this non-reflected seam cannot mint a grant for an
	// input the AICallable capture surface would reject.
	FHyperAIStudioDiagnosticsViewportCaptureRequest ValidationRequest;
	ValidationRequest.bConfirmFileWrite = true;
	ValidationRequest.OperationId = Request.OperationId;
	ValidationRequest.AuthorizationToken = TEXT("server-owned-validation-placeholder");
	ValidationRequest.CaptureMode = TEXT("color");
	ValidationRequest.MaxWidth = Request.MaxWidth;
	ValidationRequest.MaxHeight = Request.MaxHeight;
	ValidationRequest.MaxPixels = Request.MaxPixels;
	ValidationRequest.MaxCompressedBytes = Request.MaxCompressedBytes;
	FHyperAIStudioDiagnosticsNormalizedViewportCaptureRequest Validated;
	if (!FHyperAIStudioViewportCaptureContracts::NormalizeRequest(
		ValidationRequest, Validated, OutErrorCode, OutError))
	{
		return false;
	}
	if (Lifetime.GetTicks() <= 0
		|| Lifetime > FTimespan::FromSeconds(MaxLifetimeSeconds))
	{
		OutErrorCode = TEXT("invalid_authorization_lifetime");
		OutError = TEXT("A viewport-capture grant lifetime must be positive and no longer than five minutes.");
		return false;
	}

	const FString CanonicalProjectId =
		FHyperAIStudioViewportCaptureContracts::ComputeCanonicalProjectId();
	const FString EffectHash =
		FHyperAIStudioViewportCaptureContracts::ComputeAuthorizationEffectHash(
			Validated, CanonicalProjectId);
	if (CanonicalProjectId.IsEmpty() || EffectHash.IsEmpty())
	{
		OutErrorCode = TEXT("authorization_binding_unavailable");
		OutError = TEXT("The active project and requested external effect could not be bound.");
		return false;
	}

	FScopeLock Lock(&Mutex);
	if (!bAccepting)
	{
		OutErrorCode = TEXT("authorization_issuer_shutdown");
		OutError = TEXT("The viewport-capture grant issuer is shut down.");
		return false;
	}
	const FDateTime NowUtc = UtcNow();
	PruneExpiredLocked(NowUtc);
	if (Grants.Num() >= MaxActiveGrants)
	{
		OutErrorCode = TEXT("authorization_grant_capacity_reached");
		OutError = TEXT("The bounded viewport-capture grant store is full.");
		return false;
	}
	const FString Token = MakeUniqueTokenLocked();
	if (Token.IsEmpty())
	{
		OutErrorCode = TEXT("authorization_token_generation_failed");
		OutError = TEXT("A unique opaque viewport-capture token could not be generated.");
		return false;
	}
	const FDateTime ExpiresAtUtc = NowUtc + Lifetime;
	if (ExpiresAtUtc <= NowUtc)
	{
		OutErrorCode = TEXT("invalid_authorization_expiry");
		OutError = TEXT("The viewport-capture grant expiry was not later than issuance.");
		return false;
	}

	FStoredGrant Stored;
	Stored.OperationId = Validated.OperationId;
	Stored.CanonicalProjectId = CanonicalProjectId;
	Stored.EffectHash = EffectHash;
	Stored.ExpiresAtUtc = ExpiresAtUtc;
	Grants.Add(Token, MoveTemp(Stored));

	OutGrant.Token = Token;
	OutGrant.OperationId = Validated.OperationId;
	OutGrant.CanonicalProjectId = CanonicalProjectId;
	OutGrant.EffectHash = EffectHash;
	OutGrant.ExpiresAtUtc = ExpiresAtUtc;
	return true;
}

bool FHyperAIStudioViewportCaptureGrantIssuer::ConsumeExact(
	const FHyperAIStudioViewportCaptureAuthorizationRequest& Request,
	FHyperAIStudioViewportCaptureAuthorizationReceipt& OutReceipt,
	FString& OutError)
{
	OutReceipt = FHyperAIStudioViewportCaptureAuthorizationReceipt();
	OutError.Reset();
	FScopeLock Lock(&Mutex);
	if (!bAccepting)
	{
		OutError = TEXT("The viewport-capture authorization issuer is shut down.");
		return false;
	}

	FStoredGrant* Stored = Grants.Find(Request.Token);
	if (!Stored)
	{
		OutError = TEXT("The viewport-capture grant is unknown or was already consumed.");
		return false;
	}
	const FDateTime NowUtc = UtcNow();
	if (Stored->ExpiresAtUtc <= NowUtc)
	{
		Grants.Remove(Request.Token);
		OutError = TEXT("The viewport-capture grant expired before it could be consumed.");
		return false;
	}
	if (Stored->OperationId != Request.OperationId
		|| Stored->CanonicalProjectId != Request.CanonicalProjectId
		|| Stored->EffectHash != Request.EffectHash)
	{
		OutError = TEXT("The viewport-capture grant did not match the exact operation, project, and effect binding.");
		return false;
	}

	// Remove before returning success: no later write path can replay this token.
	const FStoredGrant Consumed = *Stored;
	Grants.Remove(Request.Token);
	OutReceipt.OperationId = Consumed.OperationId;
	OutReceipt.CanonicalProjectId = Consumed.CanonicalProjectId;
	OutReceipt.EffectHash = Consumed.EffectHash;
	OutReceipt.bConsumed = true;
	return true;
}

void FHyperAIStudioViewportCaptureGrantIssuer::Shutdown()
{
	FScopeLock Lock(&Mutex);
	bAccepting = false;
	Grants.Reset();
}

int32 FHyperAIStudioViewportCaptureGrantIssuer::GetActiveGrantCount() const
{
	FScopeLock Lock(&Mutex);
	return Grants.Num();
}

void FHyperAIStudioViewportCaptureGrantIssuer::PruneExpiredLocked(const FDateTime& NowUtc)
{
	for (auto It = Grants.CreateIterator(); It; ++It)
	{
		if (It.Value().ExpiresAtUtc <= NowUtc)
		{
			It.RemoveCurrent();
		}
	}
}

FString FHyperAIStudioViewportCaptureGrantIssuer::MakeUniqueTokenLocked()
{
	for (int32 Attempt = 0; Attempt < 4; ++Attempt)
	{
		const FString Candidate = TEXT("vcg_")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits)
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
		if (!Grants.Contains(Candidate))
		{
			return Candidate;
		}
	}
	return FString();
}

void FHyperAIStudioViewportCaptureAuthorization::SetGate(
	TSharedPtr<IHyperAIStudioViewportCaptureAuthorizationGate, ESPMode::ThreadSafe> Gate)
{
	FScopeLock Lock(&HyperAIStudio::ViewportCapture::Private::AuthorizationMutex);
	HyperAIStudio::ViewportCapture::Private::AuthorizationGate = MoveTemp(Gate);
}

void FHyperAIStudioViewportCaptureAuthorization::ResetGate()
{
	SetGate(nullptr);
}

bool FHyperAIStudioViewportCaptureAuthorization::ConsumeExact(
	const FHyperAIStudioViewportCaptureAuthorizationRequest& Request,
	FString& OutErrorCode,
	FString& OutError)
{
	using namespace HyperAIStudio::ViewportCapture::Private;
	OutErrorCode.Reset();
	OutError.Reset();
	if (Request.Token.IsEmpty() || Request.OperationId.IsEmpty()
		|| Request.CanonicalProjectId.IsEmpty() || Request.EffectHash.IsEmpty())
	{
		OutErrorCode = TEXT("authorization_request_incomplete");
		OutError = TEXT("The trusted authorization request was not fully bound.");
		return false;
	}

	TSharedPtr<IHyperAIStudioViewportCaptureAuthorizationGate, ESPMode::ThreadSafe> Gate;
	{
		FScopeLock Lock(&AuthorizationMutex);
		Gate = AuthorizationGate;
	}
	if (!Gate.IsValid())
	{
		OutErrorCode = TEXT("authorization_gate_unavailable");
		OutError = TEXT("No trusted non-AICallable viewport-capture authorization issuer is installed; the file write was denied.");
		return false;
	}

	FHyperAIStudioViewportCaptureAuthorizationReceipt Receipt;
	FString GateError;
	if (!Gate->ConsumeExact(Request, Receipt, GateError))
	{
		OutErrorCode = TEXT("authorization_denied");
		OutError = GateError.IsEmpty()
			? TEXT("The trusted authorization gate denied or could not consume the single-use grant.")
			: FHyperAIStudioDiagnosticsCommon::Clip(GateError, 512);
		return false;
	}
	if (!Receipt.bConsumed
		|| Receipt.OperationId != Request.OperationId
		|| Receipt.CanonicalProjectId != Request.CanonicalProjectId
		|| Receipt.EffectHash != Request.EffectHash)
	{
		OutErrorCode = TEXT("authorization_receipt_mismatch");
		OutError = TEXT("The trusted gate receipt did not exactly match the operation, project, and external effect; the file write was denied.");
		return false;
	}
	return true;
}

bool FHyperAIStudioViewportCaptureContracts::NormalizeRequest(
	const FHyperAIStudioDiagnosticsViewportCaptureRequest& Request,
	FHyperAIStudioDiagnosticsNormalizedViewportCaptureRequest& OutRequest,
	FString& OutErrorCode,
	FString& OutError)
{
	OutRequest = FHyperAIStudioDiagnosticsNormalizedViewportCaptureRequest();
	if (!Request.bConfirmFileWrite)
	{
		OutErrorCode = TEXT("file_write_confirmation_required");
		OutError = TEXT("bConfirmFileWrite must be true because capture creates one project-contained PNG file.");
		return false;
	}
	if (!FHyperAIStudioDiagnosticsCommon::IsSafeIdentifier(Request.OperationId, 8, 64))
	{
		OutErrorCode = TEXT("invalid_operation_id");
		OutError = TEXT("OperationId must be 8..64 ASCII letters, digits, underscore, or hyphen characters.");
		return false;
	}
	if (Request.AuthorizationToken.Len() < 16 || Request.AuthorizationToken.Len() > 256
		|| FHyperAIStudioDiagnosticsCommon::ContainsEmbeddedNull(Request.AuthorizationToken)
		|| !FHyperAIStudioDiagnosticsCommon::HasWellFormedUtf16(Request.AuthorizationToken))
	{
		OutErrorCode = TEXT("invalid_authorization_token");
		OutError = TEXT("AuthorizationToken must be an opaque, well-formed 16..256 character value issued by the trusted server flow.");
		return false;
	}
	if (Request.CaptureMode != TEXT("color"))
	{
		OutErrorCode = TEXT("unsupported_capture_mode");
		OutError = TEXT("CaptureMode must be exactly color.");
		return false;
	}
	if (Request.MaxWidth < 1 || Request.MaxWidth > HardMaxWidth
		|| Request.MaxHeight < 1 || Request.MaxHeight > HardMaxHeight
		|| Request.MaxPixels < 1 || Request.MaxPixels > HardMaxPixels)
	{
		OutErrorCode = TEXT("invalid_pixel_bounds");
		OutError = TEXT("Requested viewport dimension or pixel bound is outside the hard supported range.");
		return false;
	}
	if (Request.MaxCompressedBytes < MinCompressedBytes
		|| Request.MaxCompressedBytes > HardMaxCompressedBytes)
	{
		OutErrorCode = TEXT("invalid_compressed_byte_bound");
		OutError = TEXT("MaxCompressedBytes is outside the hard supported range.");
		return false;
	}
	OutRequest.OperationId = Request.OperationId;
	OutRequest.AuthorizationToken = Request.AuthorizationToken;
	OutRequest.MaxWidth = Request.MaxWidth;
	OutRequest.MaxHeight = Request.MaxHeight;
	OutRequest.MaxPixels = Request.MaxPixels;
	OutRequest.MaxCompressedBytes = Request.MaxCompressedBytes;
	return true;
}

bool FHyperAIStudioViewportCaptureContracts::ValidateViewportDimensions(
	const int32 Width,
	const int32 Height,
	const FHyperAIStudioDiagnosticsNormalizedViewportCaptureRequest& Request,
	FString& OutErrorCode,
	FString& OutError)
{
	if (Width <= 0 || Height <= 0)
	{
		OutErrorCode = TEXT("viewport_size_unavailable");
		OutError = TEXT("Current level-editor viewport has no positive drawable size.");
		return false;
	}
	const int64 PixelCount = static_cast<int64>(Width) * static_cast<int64>(Height);
	if (Width > Request.MaxWidth || Height > Request.MaxHeight || PixelCount > Request.MaxPixels
		|| Width > HardMaxWidth || Height > HardMaxHeight || PixelCount > HardMaxPixels)
	{
		OutErrorCode = TEXT("viewport_exceeds_pixel_bound");
		OutError = TEXT("Current viewport exceeds a requested or hard capture bound; no resize or ReadPixels allocation was attempted.");
		return false;
	}
	return true;
}

FString FHyperAIStudioViewportCaptureContracts::MakeServerFileName(
	const FString& UtcTimestamp,
	const FString& ContentHash)
{
	FString Timestamp = UtcTimestamp;
	Timestamp.ReplaceInline(TEXT("-"), TEXT(""));
	Timestamp.ReplaceInline(TEXT(":"), TEXT(""));
	Timestamp.ReplaceInline(TEXT("."), TEXT(""));
	Timestamp.ReplaceInline(TEXT("Z"), TEXT("z"));
	const FString HashHex = ContentHash.StartsWith(TEXT("sha1:"))
		? ContentHash.RightChop(5) : ContentHash;
	if (Timestamp.IsEmpty() || HashHex.Len() != 40)
	{
		return FString();
	}
	for (const TCHAR Character : Timestamp)
	{
		if (!(FChar::IsDigit(Character) || Character == TEXT('T') || Character == TEXT('z')))
		{
			return FString();
		}
	}
	for (const TCHAR Character : HashHex)
	{
		if (!FChar::IsHexDigit(Character))
		{
			return FString();
		}
	}
	return FString::Printf(TEXT("viewport-%s-%s.png"), *Timestamp, *HashHex.Left(16).ToLower());
}

bool FHyperAIStudioViewportCaptureContracts::IsServerFileName(const FString& FileName)
{
	if (!FileName.StartsWith(TEXT("viewport-")) || !FileName.EndsWith(TEXT(".png"))
		|| FileName.Len() < 40 || FileName.Len() > 80
		|| FileName.Contains(TEXT("/")) || FileName.Contains(TEXT("\\"))
		|| FileName.Contains(TEXT("..")))
	{
		return false;
	}
	for (const TCHAR Character : FileName)
	{
		if (!(FChar::IsAlnum(Character) || Character == TEXT('-') || Character == TEXT('.')))
		{
			return false;
		}
	}
	return true;
}

FString FHyperAIStudioViewportCaptureContracts::ComputeCanonicalProjectId()
{
	FString ProjectFile = FPaths::GetProjectFilePath();
	if (ProjectFile.IsEmpty())
	{
		return FString();
	}
	ProjectFile = FPaths::ConvertRelativePathToFull(ProjectFile);
	FPaths::NormalizeFilename(ProjectFile);
#if PLATFORM_WINDOWS
	ProjectFile.ToLowerInline();
#endif
	return FHyperAIStudioDiagnosticsCommon::HashTokens({
		TEXT("hyperai.viewport-capture.project.v1"),
		ProjectFile
	});
}

FString FHyperAIStudioViewportCaptureContracts::ComputeAuthorizationEffectHash(
	const FHyperAIStudioDiagnosticsNormalizedViewportCaptureRequest& Request,
	const FString& CanonicalProjectId)
{
	if (CanonicalProjectId.IsEmpty())
	{
		return FString();
	}
	return FHyperAIStudioDiagnosticsCommon::HashTokens({
		TEXT("hyperai.viewport-capture.effect.v1"),
		CanonicalProjectId,
		Request.OperationId,
		TEXT("color"),
		LexToString(Request.MaxWidth),
		LexToString(Request.MaxHeight),
		LexToString(Request.MaxPixels),
		LexToString(Request.MaxCompressedBytes),
		TEXT("Saved/HyperAIStudio/Captures/<server-generated-name>.png"),
		TEXT("create-new;no-overwrite;at-most-one-file;fixed-directories-may-be-created")
	});
}

FHyperAIStudioDiagnosticsViewportCaptureResult UHyperAIStudioViewportCaptureToolset::hyper_viewport_capture(
	const FHyperAIStudioDiagnosticsViewportCaptureRequest& Request)
{
	using namespace HyperAIStudio::ViewportCapture::Private;
	FHyperAIStudioDiagnosticsNormalizedViewportCaptureRequest Normalized;
	FString ErrorCode;
	FString Error;
	if (!FHyperAIStudioViewportCaptureContracts::NormalizeRequest(
		Request, Normalized, ErrorCode, Error))
	{
		return ErrorResult(TEXT("invalid_request"), ErrorCode, TEXT("request"), Error);
	}
	const FString CanonicalProjectId =
		FHyperAIStudioViewportCaptureContracts::ComputeCanonicalProjectId();
	const FString EffectHash =
		FHyperAIStudioViewportCaptureContracts::ComputeAuthorizationEffectHash(
			Normalized, CanonicalProjectId);
	if (CanonicalProjectId.IsEmpty() || EffectHash.IsEmpty())
	{
		FHyperAIStudioDiagnosticsViewportCaptureResult Result = ErrorResult(
			TEXT("unavailable"),
			TEXT("authorization_binding_unavailable"),
			TEXT("project"),
			TEXT("The active project could not be bound to a trusted external-effect authorization request."));
		Result.OperationId = Normalized.OperationId;
		Result.AuthorizationState = TEXT("unavailable");
		return Result;
	}

	const auto PreflightError = [&Normalized, &EffectHash](
		const FString& Status,
		const FString& Code,
		const FString& Field,
		const FString& Message)
	{
		FHyperAIStudioDiagnosticsViewportCaptureResult Result = ErrorResult(
			Status, Code, Field, Message);
		Result.OperationId = Normalized.OperationId;
		Result.AuthorizationEffectHash = EffectHash;
		Result.AuthorizationState = TEXT("required");
		return Result;
	};
#if !PLATFORM_WINDOWS
	return PreflightError(
		TEXT("unavailable"),
		TEXT("same_handle_capture_write_unavailable"),
		TEXT("platform"),
		TEXT("The audited same-handle capture writer is currently available only on the Win64 shipping platform."));
#else
	if (!GCurrentLevelEditingViewportClient || !GCurrentLevelEditingViewportClient->Viewport)
	{
		return PreflightError(
			TEXT("unavailable"),
			TEXT("level_editor_viewport_unavailable"),
			TEXT("viewport"),
			TEXT("No current level-editor viewport is available."));
	}
	FViewport* Viewport = GCurrentLevelEditingViewportClient->Viewport;
	const FIntPoint Size = Viewport->GetSizeXY();
	if (!FHyperAIStudioViewportCaptureContracts::ValidateViewportDimensions(
		Size.X, Size.Y, Normalized, ErrorCode, Error))
	{
		return PreflightError(TEXT("unavailable"), ErrorCode, TEXT("viewport"), Error);
	}

	FHyperAIStudioViewportCaptureAuthorizationRequest AuthorizationRequest;
	AuthorizationRequest.Token = Normalized.AuthorizationToken;
	AuthorizationRequest.OperationId = Normalized.OperationId;
	AuthorizationRequest.CanonicalProjectId = CanonicalProjectId;
	AuthorizationRequest.EffectHash = EffectHash;
	if (!FHyperAIStudioViewportCaptureAuthorization::ConsumeExact(
		AuthorizationRequest, ErrorCode, Error))
	{
		FHyperAIStudioDiagnosticsViewportCaptureResult Result = ErrorResult(
			ErrorCode == TEXT("authorization_gate_unavailable")
				? TEXT("unavailable") : TEXT("failed"),
			ErrorCode,
			TEXT("authorization_token"),
			Error);
		Result.OperationId = Normalized.OperationId;
		Result.AuthorizationEffectHash = EffectHash;
		Result.AuthorizationState = ErrorCode == TEXT("authorization_gate_unavailable")
			? TEXT("unavailable") : TEXT("failed");
		return Result;
	}
	const auto AuthorizedError = [&Normalized, &EffectHash](
		const FString& Status,
		const FString& Code,
		const FString& Field,
		const FString& Message)
	{
		FHyperAIStudioDiagnosticsViewportCaptureResult Result = ErrorResult(
			Status, Code, Field, Message);
		Result.OperationId = Normalized.OperationId;
		Result.AuthorizationEffectHash = EffectHash;
		Result.AuthorizationState = TEXT("consumed");
		return Result;
	};

	const int64 PixelCount = static_cast<int64>(Size.X) * static_cast<int64>(Size.Y);
	TArray<FColor> Pixels;
	Pixels.Reserve(static_cast<int32>(PixelCount));
	if (!Viewport->ReadPixels(Pixels) || Pixels.Num() != PixelCount)
	{
		return AuthorizedError(
			TEXT("failed"),
			TEXT("viewport_read_pixels_failed"),
			TEXT("viewport"),
			TEXT("Bounded viewport ReadPixels failed or returned an unexpected pixel count."));
	}

	TArray64<uint8> Compressed64;
	FImageUtils::PNGCompressImageArray(
		Size.X,
		Size.Y,
		TArrayView64<const FColor>(Pixels.GetData(), Pixels.Num()),
		Compressed64);
	Pixels.Reset();
	if (Compressed64.IsEmpty() || Compressed64.Num() > Normalized.MaxCompressedBytes
		|| Compressed64.Num() > FHyperAIStudioViewportCaptureContracts::HardMaxCompressedBytes)
	{
		return AuthorizedError(
			TEXT("failed"),
			TEXT("compressed_output_bound_exceeded"),
			TEXT("max_compressed_bytes"),
			TEXT("PNG compression failed or exceeded the requested/hard byte bound; no file was created."));
	}
	TArray<uint8> Compressed;
	Compressed.Append(Compressed64.GetData(), static_cast<int32>(Compressed64.Num()));
	Compressed64.Reset();

	const FString ContentHash = Sha1Bytes(Compressed);
	const FString FileName = FHyperAIStudioViewportCaptureContracts::MakeServerFileName(
		FDateTime::UtcNow().ToIso8601(), ContentHash);
	if (!FHyperAIStudioViewportCaptureContracts::IsServerFileName(FileName))
	{
		return AuthorizedError(
			TEXT("failed"),
			TEXT("server_file_name_failed"),
			TEXT("file_name"),
			TEXT("Could not derive a bounded server-generated capture name."));
	}

	const FString SavedRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir());
	FString CaptureDirectory;
	if (!EnsureCaptureDirectory(SavedRoot, CaptureDirectory, ErrorCode))
	{
		FHyperAIStudioDiagnosticsViewportCaptureResult Failure = AuthorizedError(
			TEXT("failed"),
			ErrorCode.IsEmpty() ? TEXT("capture_directory_create_failed") : ErrorCode,
			TEXT("capture_directory"),
			TEXT("Could not create or verify the fixed project Saved capture directory."));
		Failure.FilesystemEffect = TEXT("fixed_capture_directory_may_be_partially_created_or_verified");
		FHyperAIStudioDiagnosticsCommon::AddDiagnosticOnce(
			Failure.Diagnostics,
			TEXT("filesystem_side_effect_disclosed"),
			TEXT("warning"),
			TEXT("filesystem_effect"),
			TEXT("A fixed Saved/HyperAIStudio directory component may have been created before containment verification failed."));
		return Failure;
	}

	FString AbsolutePath;
	if (!WriteCaptureSameHandle(
		SavedRoot, CaptureDirectory, FileName, Compressed, AbsolutePath, ErrorCode))
	{
		FHyperAIStudioDiagnosticsViewportCaptureResult Failure = ErrorResult(
			TEXT("failed"),
			ErrorCode.IsEmpty() ? TEXT("capture_write_failed") : ErrorCode,
			TEXT("capture_file"),
			TEXT("Same-handle final-path containment, create-new, write, flush, or size verification failed; no successful file receipt is claimed."));
		Failure.OperationId = Normalized.OperationId;
		Failure.AuthorizationEffectHash = EffectHash;
		Failure.AuthorizationState = TEXT("consumed");
		const bool bCleanupUnconfirmed =
			ErrorCode == TEXT("capture_partial_file_cleanup_unconfirmed");
		Failure.FilesystemEffect = bCleanupUnconfirmed
			? TEXT("fixed_capture_directory_created_or_verified;partial_capture_file_may_remain")
			: TEXT("fixed_capture_directory_created_or_verified;capture_file_not_retained");
		FHyperAIStudioDiagnosticsCommon::AddDiagnosticOnce(
			Failure.Diagnostics,
			TEXT("filesystem_side_effect_disclosed"),
			TEXT("warning"),
			TEXT("filesystem_effect"),
			bCleanupUnconfirmed
				? TEXT("The fixed capture directory exists and a partial server-named file may remain because same-handle deletion could not be confirmed.")
				: TEXT("The fixed Saved/HyperAIStudio/Captures directory may have been created before the file write failed; same-handle cleanup was scheduled."));
		return Failure;
	}

	FHyperAIStudioDiagnosticsViewportCaptureResult Result;
	Result.Status = TEXT("captured");
	Result.CaptureMode = TEXT("color");
	Result.OperationId = Normalized.OperationId;
	Result.AuthorizationEffectHash = EffectHash;
	Result.AuthorizationState = TEXT("consumed");
	Result.bFileCreated = true;
	Result.FilesystemEffect = TEXT("created_one_new_project_saved_png");
	Result.ProjectRelativePath = FPaths::Combine(
		TEXT("Saved"), TEXT("HyperAIStudio"), TEXT("Captures"), FileName);
	Result.AbsolutePath = AbsolutePath;
	Result.FileName = FileName;
	Result.ContentHash = ContentHash;
	Result.Width = Size.X;
	Result.Height = Size.Y;
	Result.PixelCount = PixelCount;
	Result.CompressedBytes = Compressed.Num();
	FHyperAIStudioDiagnosticsCommon::AddDiagnostic(
		Result.Diagnostics,
		TEXT("filesystem_side_effect_created"),
		TEXT("info"),
		TEXT("project_relative_path"),
		TEXT("Created exactly one new server-named PNG under Saved/HyperAIStudio/Captures; no client path or overwrite was accepted."));
	return Result;
#endif
}
