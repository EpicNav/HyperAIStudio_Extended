// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioDiagnosticsCommon.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "HyperAIStudioViewportCaptureToolset.generated.h"

/** Explicit bounded request for one current level-editor viewport PNG. */
USTRUCT(BlueprintType)
struct FHyperAIStudioDiagnosticsViewportCaptureRequest
{
	GENERATED_BODY()

	/** Must be true to declare intent; this never replaces the trusted single-use authorization grant. */
	UPROPERTY()
	bool bConfirmFileWrite = false;

	/** Client idempotency key bound by the trusted server grant. */
	UPROPERTY()
	FString OperationId;

	/** Opaque single-use grant issued by a trusted non-AICallable server/UI flow. */
	UPROPERTY()
	FString AuthorizationToken;

	/** Closed mode; currently only color is supported. */
	UPROPERTY()
	FString CaptureMode = TEXT("color");

	/** Reject instead of resizing when the current viewport exceeds this width. Hard maximum: 1920. */
	UPROPERTY()
	int32 MaxWidth = 1920;

	/** Reject instead of resizing when the current viewport exceeds this height. Hard maximum: 1080. */
	UPROPERTY()
	int32 MaxHeight = 1080;

	/** Reject before ReadPixels when the current viewport exceeds this count. Hard maximum: 2073600. */
	UPROPERTY()
	int32 MaxPixels = 1920 * 1080;

	/** Reject before file creation when the PNG exceeds this size. Range: 1048576..16777216. */
	UPROPERTY()
	int32 MaxCompressedBytes = 16 * 1024 * 1024;
};

/** One server-named, project-contained capture receipt. */
USTRUCT(BlueprintType)
struct FHyperAIStudioDiagnosticsViewportCaptureResult
{
	GENERATED_BODY()

	UPROPERTY()
	FString SchemaVersion = TEXT("hyperai.viewport-capture.v1");

	/** captured, invalid_request, unavailable, or failed. */
	UPROPERTY()
	FString Status = TEXT("unavailable");

	UPROPERTY()
	FString CaptureMode = TEXT("color");

	UPROPERTY()
	FString OperationId;

	/** Exact project/effect hash a trusted issuer must bind. */
	UPROPERTY()
	FString AuthorizationEffectHash;

	/** unavailable, required, consumed, or failed. */
	UPROPERTY()
	FString AuthorizationState = TEXT("unavailable");

	UPROPERTY()
	bool bFileCreated = false;

	UPROPERTY()
	FString FilesystemEffect = TEXT("none");

	/** Always Saved/HyperAIStudio/Captures/<server-generated-name>.png. */
	UPROPERTY()
	FString ProjectRelativePath;

	UPROPERTY()
	FString AbsolutePath;

	UPROPERTY()
	FString FileName;

	UPROPERTY()
	FString ContentHash;

	UPROPERTY()
	int32 Width = 0;

	UPROPERTY()
	int32 Height = 0;

	UPROPERTY()
	int64 PixelCount = 0;

	UPROPERTY()
	int64 CompressedBytes = 0;

	UPROPERTY()
	TArray<FHyperAIStudioDiagnosticsDiagnostic> Diagnostics;
};

struct FHyperAIStudioDiagnosticsNormalizedViewportCaptureRequest
{
	FString OperationId;
	FString AuthorizationToken;
	int32 MaxWidth = 0;
	int32 MaxHeight = 0;
	int32 MaxPixels = 0;
	int32 MaxCompressedBytes = 0;
};

struct FHyperAIStudioViewportCaptureAuthorizationRequest
{
	FString Token;
	FString OperationId;
	FString CanonicalProjectId;
	FString EffectHash;
};

struct FHyperAIStudioViewportCaptureAuthorizationReceipt
{
	FString OperationId;
	FString CanonicalProjectId;
	FString EffectHash;
	bool bConsumed = false;
};

/** Opaque grant returned only by the trusted, non-AICallable issuance flow. */
struct FHyperAIStudioViewportCaptureGrant
{
	FString Token;
	FString OperationId;
	FString CanonicalProjectId;
	FString EffectHash;
	FDateTime ExpiresAtUtc;
};

/** Implemented only by a trusted non-AICallable issuer; client fields never self-authorize. */
class IHyperAIStudioViewportCaptureAuthorizationGate
{
public:
	virtual ~IHyperAIStudioViewportCaptureAuthorizationGate() = default;
	virtual bool ConsumeExact(
		const FHyperAIStudioViewportCaptureAuthorizationRequest& Request,
		FHyperAIStudioViewportCaptureAuthorizationReceipt& OutReceipt,
		FString& OutError) = 0;
};

/**
 * Lifecycle-owned, bounded single-use grant issuer and gate.
 *
 * IssueForRequest computes the canonical project and external-effect fingerprint itself. It is a
 * normal C++ API (never reflected/AICallable), so MCP request fields cannot mint or widen grants.
 */
class FHyperAIStudioViewportCaptureGrantIssuer final
	: public IHyperAIStudioViewportCaptureAuthorizationGate
{
public:
	static constexpr int32 MaxActiveGrants = 64;
	static constexpr int32 MaxLifetimeSeconds = 5 * 60;

	FHyperAIStudioViewportCaptureGrantIssuer();
	explicit FHyperAIStudioViewportCaptureGrantIssuer(TFunction<FDateTime()> InUtcNow);
	virtual ~FHyperAIStudioViewportCaptureGrantIssuer() override;

	FHyperAIStudioViewportCaptureGrantIssuer(
		const FHyperAIStudioViewportCaptureGrantIssuer&) = delete;
	FHyperAIStudioViewportCaptureGrantIssuer& operator=(
		const FHyperAIStudioViewportCaptureGrantIssuer&) = delete;

	bool IssueForRequest(
		const FHyperAIStudioDiagnosticsNormalizedViewportCaptureRequest& Request,
		FTimespan Lifetime,
		FHyperAIStudioViewportCaptureGrant& OutGrant,
		FString& OutErrorCode,
		FString& OutError);

	virtual bool ConsumeExact(
		const FHyperAIStudioViewportCaptureAuthorizationRequest& Request,
		FHyperAIStudioViewportCaptureAuthorizationReceipt& OutReceipt,
		FString& OutError) override;

	/** Permanently closes this instance and securely forgets every unconsumed grant. */
	void Shutdown();

	/** Test/health seam; never reveals tokens or bindings. */
	int32 GetActiveGrantCount() const;

private:
	struct FStoredGrant
	{
		FString OperationId;
		FString CanonicalProjectId;
		FString EffectHash;
		FDateTime ExpiresAtUtc;
	};

	void PruneExpiredLocked(const FDateTime& NowUtc);
	FString MakeUniqueTokenLocked();

	mutable FCriticalSection Mutex;
	TMap<FString, FStoredGrant> Grants;
	TFunction<FDateTime()> UtcNow;
	bool bAccepting = true;
};

/** Process-local gate seam. It defaults to no gate and therefore denies every write. */
class FHyperAIStudioViewportCaptureAuthorization final
{
public:
	static void SetGate(TSharedPtr<IHyperAIStudioViewportCaptureAuthorizationGate, ESPMode::ThreadSafe> Gate);
	static void ResetGate();
	static bool ConsumeExact(
		const FHyperAIStudioViewportCaptureAuthorizationRequest& Request,
		FString& OutErrorCode,
		FString& OutError);
};

/** Pure validation/naming seams; capture/write itself stays game-thread and same-handle. */
class FHyperAIStudioViewportCaptureContracts final
{
public:
	static constexpr int32 HardMaxWidth = 1920;
	static constexpr int32 HardMaxHeight = 1080;
	static constexpr int32 HardMaxPixels = HardMaxWidth * HardMaxHeight;
	static constexpr int32 MinCompressedBytes = 1024 * 1024;
	static constexpr int32 HardMaxCompressedBytes = 16 * 1024 * 1024;

	static bool NormalizeRequest(
		const FHyperAIStudioDiagnosticsViewportCaptureRequest& Request,
		FHyperAIStudioDiagnosticsNormalizedViewportCaptureRequest& OutRequest,
		FString& OutErrorCode,
		FString& OutError);

	static bool ValidateViewportDimensions(
		int32 Width,
		int32 Height,
		const FHyperAIStudioDiagnosticsNormalizedViewportCaptureRequest& Request,
		FString& OutErrorCode,
		FString& OutError);

	static FString MakeServerFileName(
		const FString& UtcTimestamp,
		const FString& ContentHash);

	static bool IsServerFileName(const FString& FileName);

	static FString ComputeCanonicalProjectId();
	static FString ComputeAuthorizationEffectHash(
		const FHyperAIStudioDiagnosticsNormalizedViewportCaptureRequest& Request,
		const FString& CanonicalProjectId);
};

/** Source candidate; production registration remains admission-evidence controlled. */
UCLASS()
class HYPERAISTUDIO_API UHyperAIStudioViewportCaptureToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	/** Capture the current level-editor viewport into one server-named project Saved PNG with exact bounds and receipt. */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Diagnostics")
	static FHyperAIStudioDiagnosticsViewportCaptureResult hyper_viewport_capture(
		const FHyperAIStudioDiagnosticsViewportCaptureRequest& Request);
};
