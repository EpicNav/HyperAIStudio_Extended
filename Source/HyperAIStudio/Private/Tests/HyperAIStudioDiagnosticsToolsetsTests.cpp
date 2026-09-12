// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HyperAIStudioAuditToolset.h"
#include "HyperAIStudioCapabilityPackRegistry.h"
#include "HyperAIStudioDiagnoseToolset.h"
#include "HyperAIStudioDiagnosticsCommon.h"
#include "HyperAIStudioDiagnosticsRegistration.h"
#include "HyperAIStudioLogTailToolset.h"
#include "HyperAIStudioViewportCaptureToolset.h"

#include "Misc/AutomationTest.h"
#include "ToolsetRegistry/ToolsetDefinition.h"
#include "UObject/UnrealType.h"

namespace HyperAIStudio::DiagnosticsTests::Private
{
	constexpr EAutomationTestFlags Flags =
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	FHyperAIStudioDiagnosticsFinding Finding(
		const FString& Kind,
		const FString& Subject,
		const FString& Code)
	{
		FHyperAIStudioDiagnosticsFinding Result;
		Result.Kind = Kind;
		Result.Subject = Subject;
		Result.Code = Code;
		Result.Severity = TEXT("warning");
		Result.Message = TEXT("bounded test finding");
		Result.RecordId = FHyperAIStudioDiagnosticsCommon::MakeRecordId(Kind, Subject, Code);
		return Result;
	}

	const FHyperAIStudioCapabilityToolDefinition* CatalogTool(const FString& Name)
	{
		return FHyperAIStudioCapabilityPackRegistry::GetCatalog().Tools.FindByPredicate(
			[&Name](const FHyperAIStudioCapabilityToolDefinition& Tool)
			{
				return Tool.Name == Name;
			});
	}

	class FCountingViewportAuthorizationGate final
		: public IHyperAIStudioViewportCaptureAuthorizationGate
	{
	public:
		virtual bool ConsumeExact(
			const FHyperAIStudioViewportCaptureAuthorizationRequest& Request,
			FHyperAIStudioViewportCaptureAuthorizationReceipt& OutReceipt,
			FString& OutError) override
		{
			++Calls;
			OutReceipt.OperationId = Request.OperationId;
			OutReceipt.CanonicalProjectId = Request.CanonicalProjectId;
			OutReceipt.EffectHash = bReturnMismatchedReceipt
				? TEXT("sha1:0000000000000000000000000000000000000000")
				: Request.EffectHash;
			OutReceipt.bConsumed = true;
			return true;
		}

		int32 Calls = 0;
		bool bReturnMismatchedReceipt = false;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioDiagnosticsCommonContractTest,
	"HyperAIStudio.NativeTools.Diagnostics.CommonContract",
	HyperAIStudio::DiagnosticsTests::Private::Flags)

bool FHyperAIStudioDiagnosticsCommonContractTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::DiagnosticsTests::Private;
	const FString EmbeddedNull(6, TEXT("ok\0bad"));
	TestTrue(TEXT("Embedded null is detected"),
		FHyperAIStudioDiagnosticsCommon::ContainsEmbeddedNull(EmbeddedNull));
	TestTrue(TEXT("Safe id accepts bounded token"),
		FHyperAIStudioDiagnosticsCommon::IsSafeIdentifier(TEXT("audit_123"), 8, 64));
	TestFalse(TEXT("Safe id rejects separators"),
		FHyperAIStudioDiagnosticsCommon::IsSafeIdentifier(TEXT("../audit"), 8, 64));
	TestFalse(TEXT("Safe id rejects non-ASCII alphanumeric text"),
		FHyperAIStudioDiagnosticsCommon::IsSafeIdentifier(TEXT("audit_éé"), 8, 64));

	TArray<FHyperAIStudioDiagnosticsFinding> Findings = {
		Finding(TEXT("test"), TEXT("A"), TEXT("a")),
		Finding(TEXT("test"), TEXT("B"), TEXT("b")),
		Finding(TEXT("test"), TEXT("C"), TEXT("c"))
	};
	Findings.Sort([](const auto& Left, const auto& Right) { return Left.RecordId < Right.RecordId; });
	const FString RequestFingerprint = FHyperAIStudioDiagnosticsCommon::HashTokens({ TEXT("request") });
	const FString SnapshotFingerprint = FHyperAIStudioDiagnosticsCommon::ComputeFindingsFingerprint(
		Findings, { TEXT("state") });
	TArray<FString> MaterializedFingerprintTokens = {
		TEXT("hyperai.diagnostics.snapshot.v1"), TEXT("state")
	};
	for (const FHyperAIStudioDiagnosticsFinding& FindingValue : Findings)
	{
		MaterializedFingerprintTokens.Append({
			FindingValue.RecordId,
			FindingValue.Kind,
			FindingValue.Severity,
			FindingValue.Code,
			FindingValue.Subject,
			FindingValue.Message
		});
	}
	TestEqual(
		TEXT("Streaming findings fingerprint stays byte-identical to materialized tokens"),
		SnapshotFingerprint,
		FHyperAIStudioDiagnosticsCommon::HashTokens(MaterializedFingerprintTokens));
	FHyperAIStudioDiagnosticsProjectedPage First;
	FString ErrorCode;
	FString Error;
	TestTrue(TEXT("First page projects"), FHyperAIStudioDiagnosticsCommon::ProjectFindings(
		Findings, RequestFingerprint, SnapshotFingerprint, 1, FString(), 8192,
		First, ErrorCode, Error));
	TestEqual(TEXT("First page contains one"), First.Findings.Num(), 1);
	TestTrue(TEXT("First page has continuation"), First.bHasMore && !First.NextCursor.IsEmpty());

	FHyperAIStudioDiagnosticsProjectedPage Second;
	TestTrue(TEXT("Continuation cursor is accepted"), FHyperAIStudioDiagnosticsCommon::ProjectFindings(
		Findings, RequestFingerprint, SnapshotFingerprint, 2, First.NextCursor, 8192,
		Second, ErrorCode, Error));
	TestEqual(TEXT("Continuation returns remainder"), Second.Findings.Num(), 2);
	TestEqual(TEXT("Cursor is marked accepted"), Second.CursorStatus, FString(TEXT("accepted")));

	FHyperAIStudioDiagnosticsProjectedPage Changed;
	TestFalse(TEXT("Changed snapshot rejects cursor"), FHyperAIStudioDiagnosticsCommon::ProjectFindings(
		Findings, RequestFingerprint,
		FHyperAIStudioDiagnosticsCommon::HashTokens({ TEXT("changed") }),
		2, First.NextCursor, 8192, Changed, ErrorCode, Error));
	TestEqual(TEXT("Changed snapshot error is explicit"), ErrorCode, FString(TEXT("cursor_snapshot_changed")));

	FHyperAIStudioDiagnosticsFinding Oversized = Finding(TEXT("test"), TEXT("large"), TEXT("large"));
	FString LargeFieldValue;
	LargeFieldValue.Reserve(FHyperAIStudioDiagnosticsCommon::MaxFieldValueCharacters);
	while (LargeFieldValue.Len() < FHyperAIStudioDiagnosticsCommon::MaxFieldValueCharacters)
	{
		LargeFieldValue.AppendChar(TEXT('x'));
	}
	for (int32 Index = 0; Index < FHyperAIStudioDiagnosticsCommon::MaxFindingFields; ++Index)
	{
		FHyperAIStudioDiagnosticsCommon::AddFindingField(
			Oversized,
			FString::Printf(TEXT("field_%d"), Index),
			LargeFieldValue);
	}
	FHyperAIStudioDiagnosticsProjectedPage OverBudget;
	TestTrue(TEXT("Individually over-budget finding projects explicit omission"),
		FHyperAIStudioDiagnosticsCommon::ProjectFindings(
			{ Oversized }, RequestFingerprint,
			FHyperAIStudioDiagnosticsCommon::ComputeFindingsFingerprint({ Oversized }, {}),
			1, FString(), FHyperAIStudioDiagnosticsCommon::MinOutputBytes,
			OverBudget, ErrorCode, Error));
	TestTrue(TEXT("Over-budget omission is disclosed and cursor advances"),
		OverBudget.bOutputBudgetReached && !OverBudget.NextCursor.IsEmpty());
	FHyperAIStudioDiagnosticsProjectedPage AfterOmission;
	TestTrue(TEXT("Advanced omission cursor is accepted"),
		FHyperAIStudioDiagnosticsCommon::ProjectFindings(
			{ Oversized }, RequestFingerprint,
			FHyperAIStudioDiagnosticsCommon::ComputeFindingsFingerprint({ Oversized }, {}),
			1, OverBudget.NextCursor, FHyperAIStudioDiagnosticsCommon::MinOutputBytes,
			AfterOmission, ErrorCode, Error));
	TestEqual(TEXT("Advanced omission cursor records terminal offset"),
		AfterOmission.PageOffset, 1);
	TestTrue(TEXT("Advanced omission cursor cannot repeat skipped record"),
		AfterOmission.Findings.IsEmpty() && !AfterOmission.bHasMore
			&& !AfterOmission.bOutputBudgetReached);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioDiagnoseContractTest,
	"HyperAIStudio.NativeTools.Diagnostics.DiagnoseContract",
	HyperAIStudio::DiagnosticsTests::Private::Flags)

bool FHyperAIStudioDiagnoseContractTest::RunTest(const FString& Parameters)
{
	FHyperAIStudioDiagnosticsDiagnoseRequest Request;
	Request.Checks = { TEXT("plugin_readiness"), TEXT("current_map_state") };
	Request.PageSize = 1;
	Request.MaxObjectsScanned = 64;
	Request.MaxOutputBytes = 8192;
	FHyperAIStudioDiagnosticsNormalizedDiagnoseRequest Normalized;
	FString ErrorCode;
	FString Error;
	TestTrue(TEXT("Closed diagnose request normalizes"),
		FHyperAIStudioDiagnoseContracts::NormalizeRequest(Request, Normalized, ErrorCode, Error));

	FHyperAIStudioDiagnosticsDiagnoseRequest InvalidPath = Request;
	InvalidPath.Checks = { TEXT("asset_context") };
	InvalidPath.AssetPaths = { TEXT("C:/outside.uasset") };
	TestFalse(TEXT("Non-/Game path rejected"),
		FHyperAIStudioDiagnoseContracts::NormalizeRequest(InvalidPath, Normalized, ErrorCode, Error));
	TestEqual(TEXT("Asset path error stable"), ErrorCode, FString(TEXT("invalid_asset_path")));
	FHyperAIStudioDiagnosticsDiagnoseRequest BarePackagePath = Request;
	BarePackagePath.Checks = { TEXT("asset_context") };
	BarePackagePath.AssetPaths = { TEXT("/Game/Folder/Asset") };
	TestFalse(TEXT("Package-only path cannot masquerade as an exact object path"),
		FHyperAIStudioDiagnoseContracts::NormalizeRequest(
			BarePackagePath, Normalized, ErrorCode, Error));

	FHyperAIStudioDiagnosticsDiagnoseRequest Unsupported = Request;
	Unsupported.Checks = { TEXT("shell") };
	TestFalse(TEXT("Unsupported diagnostic kind rejected"),
		FHyperAIStudioDiagnoseContracts::NormalizeRequest(Unsupported, Normalized, ErrorCode, Error));

	TestTrue(TEXT("Normalize valid request again"),
		FHyperAIStudioDiagnoseContracts::NormalizeRequest(Request, Normalized, ErrorCode, Error));
	FHyperAIStudioDiagnosticsValueSnapshot Snapshot;
	Snapshot.RequestFingerprint = Normalized.RequestFingerprint;
	Snapshot.CapturedUtc = TEXT("2026-08-14T00:00:00Z");
	Snapshot.bEditorWorldAvailable = true;
	Snapshot.CurrentMapPackage = TEXT("/Game/Maps/Test");
	FHyperAIStudioDiagnosticsPluginSnapshot Plugin;
	Plugin.Name = TEXT("EditorToolset");
	Plugin.bInstalled = true;
	Plugin.bEnabled = false;
	Snapshot.Plugins.Add(Plugin);
	FHyperAIStudioDiagnosticsDiagnoseResult First =
		FHyperAIStudioDiagnoseContracts::Analyze(Snapshot, Request);
	TestEqual(TEXT("Fake diagnostic snapshot is complete"), First.Status, FString(TEXT("complete")));
	TestEqual(TEXT("Page is bounded"), First.ReturnedRecords, 1);
	TestTrue(TEXT("Page emits cursor"), First.bHasMore && !First.NextCursor.IsEmpty());
	TestEqual(TEXT("No delete inference"), First.DeletionAssessment, FString(TEXT("not_performed")));

	Request.Cursor = First.NextCursor;
	Snapshot.CapturedUtc = TEXT("2026-08-14T01:00:00Z");
	FHyperAIStudioDiagnosticsDiagnoseResult Second =
		FHyperAIStudioDiagnoseContracts::Analyze(Snapshot, Request);
	TestEqual(TEXT("Timestamp-only recapture keeps semantic cursor stable"),
		Second.CursorStatus, FString(TEXT("accepted")));
	TestFalse(TEXT("Second page ends result"), Second.bHasMore);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioAuditContractTest,
	"HyperAIStudio.NativeTools.Diagnostics.AuditContract",
	HyperAIStudio::DiagnosticsTests::Private::Flags)

bool FHyperAIStudioAuditContractTest::RunTest(const FString& Parameters)
{
	FHyperAIStudioDiagnosticsAuditRequest Request;
	Request.AuditId = TEXT("audit_123456");
	Request.Kinds = { TEXT("dirty_loaded_packages") };
	Request.MaxObjectsScanned = 64;
	Request.PageSize = 16;
	Request.MaxOutputBytes = 8192;
	FHyperAIStudioDiagnosticsNormalizedAuditRequest Normalized;
	FString ErrorCode;
	FString Error;
	TestTrue(TEXT("Audit start request normalizes"),
		FHyperAIStudioAuditContracts::NormalizeStartRequest(Request, Normalized, ErrorCode, Error));

	FHyperAIStudioDiagnosticsAuditRequest Invalid = Request;
	Invalid.AuditId = TEXT("../bad");
	TestFalse(TEXT("Unsafe audit id rejected"),
		FHyperAIStudioAuditContracts::NormalizeStartRequest(Invalid, Normalized, ErrorCode, Error));
	Invalid = Request;
	Invalid.Cursor = TEXT("not-allowed");
	TestFalse(TEXT("Start cursor rejected"),
		FHyperAIStudioAuditContracts::NormalizeStartRequest(Invalid, Normalized, ErrorCode, Error));

	TestTrue(TEXT("Valid audit request normalizes again"),
		FHyperAIStudioAuditContracts::NormalizeStartRequest(Request, Normalized, ErrorCode, Error));
	FHyperAIStudioDiagnosticsValueSnapshot Snapshot;
	Snapshot.RequestFingerprint = Normalized.RequestFingerprint;
	Snapshot.CapturedUtc = TEXT("2026-08-14T00:00:00Z");
	for (int32 Index = 0; Index < FHyperAIStudioAuditContracts::MaxStoredFindings + 8; ++Index)
	{
		Snapshot.DirtyLoadedPackages.Add(FString::Printf(TEXT("/Game/P%04d"), Index));
	}
	FHyperAIStudioDiagnosticsStoredAudit Audit = FHyperAIStudioAuditContracts::AnalyzeSnapshot(
		Snapshot, Normalized, TEXT("2026-08-14T00:00:00Z"));
	TestEqual(TEXT("Audit retains exact finding bound"), Audit.Findings.Num(),
		FHyperAIStudioAuditContracts::MaxStoredFindings);
	TestTrue(TEXT("Finding bound is explicit"), Audit.bFindingBoundReached && Audit.bIncomplete);
	TestEqual(TEXT("Bounded audit becomes partial"), Audit.Status, FString(TEXT("partial")));
	TestFalse(TEXT("Audit snapshot is fingerprinted"), Audit.SnapshotFingerprint.IsEmpty());
	TestTrue(TEXT("No-delete diagnostic retained"), Audit.Diagnostics.ContainsByPredicate(
		[](const FHyperAIStudioDiagnosticsDiagnostic& Diagnostic)
		{
			return Diagnostic.Code == TEXT("no_delete_inference");
		}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioLogTailContractTest,
	"HyperAIStudio.NativeTools.Diagnostics.LogTailContract",
	HyperAIStudio::DiagnosticsTests::Private::Flags)

bool FHyperAIStudioLogTailContractTest::RunTest(const FString& Parameters)
{
	FHyperAIStudioDiagnosticsLogSnapshot Snapshot;
	Snapshot.bAttached = true;
	Snapshot.Epoch = TEXT("0123456789abcdef0123456789abcdef");
	for (int32 Index = 1; Index <= 5; ++Index)
	{
		FHyperAIStudioDiagnosticsLogEntry& Entry = Snapshot.Entries.AddDefaulted_GetRef();
		Entry.Sequence = Index;
		Entry.Category = Index % 2 ? TEXT("LogBlueprint") : TEXT("LogTemp");
		Entry.Verbosity = Index == 5 ? TEXT("error") : TEXT("warning");
		Entry.Message = FString::Printf(TEXT("message-%d"), Index);
	}
	Snapshot.EarliestSequence = 1;
	Snapshot.LatestSequence = 5;
	FHyperAIStudioDiagnosticsLogTailRequest Request;
	Request.PageSize = 2;
	Request.MaxOutputBytes = 8192;
	FHyperAIStudioDiagnosticsLogTailResult Tail =
		FHyperAIStudioLogTailContracts::Analyze(Snapshot, Request);
	TestEqual(TEXT("Initial tail returns newest page"), Tail.Entries.Num(), 2);
	TestEqual(TEXT("Initial tail newest sequence"), Tail.Entries.Last().Sequence, static_cast<int64>(5));
	TestTrue(TEXT("Older initial matches disclosed"), Tail.bInitialTailOmittedOlderMatches);
	TestFalse(TEXT("Polling cursor emitted"), Tail.NextCursor.IsEmpty());

	FHyperAIStudioDiagnosticsLogTailRequest Poll = Request;
	Poll.Cursor = Tail.NextCursor;
	FHyperAIStudioDiagnosticsLogEntry& NewEntry = Snapshot.Entries.AddDefaulted_GetRef();
	NewEntry.Sequence = 6;
	NewEntry.Category = TEXT("LogTemp");
	NewEntry.Verbosity = TEXT("warning");
	NewEntry.Message = TEXT("message-6");
	Snapshot.LatestSequence = 6;
	FHyperAIStudioDiagnosticsLogTailResult NewLogs =
		FHyperAIStudioLogTailContracts::Analyze(Snapshot, Poll);
	TestEqual(TEXT("Cursor returns only newer entry"), NewLogs.Entries.Num(), 1);
	TestEqual(TEXT("Newer sequence preserved"), NewLogs.Entries[0].Sequence, static_cast<int64>(6));
	TestEqual(TEXT("Cursor accepted"), NewLogs.CursorStatus, FString(TEXT("accepted")));

	FHyperAIStudioDiagnosticsLogTailRequest ChangedFilter = Poll;
	ChangedFilter.Contains = TEXT("message");
	FHyperAIStudioDiagnosticsLogTailResult Rejected =
		FHyperAIStudioLogTailContracts::Analyze(Snapshot, ChangedFilter);
	TestEqual(TEXT("Filter mismatch rejects cursor"), Rejected.Status, FString(TEXT("invalid_request")));
	TestEqual(TEXT("Filter mismatch code stable"), Rejected.CursorDiagnosticCode,
		FString(TEXT("cursor_request_mismatch")));

	FHyperAIStudioDiagnosticsLogTailRequest Invalid = Request;
	Invalid.MaxVerbosity = TEXT("all_and_more");
	FHyperAIStudioDiagnosticsNormalizedLogTailRequest Normalized;
	FString ErrorCode;
	FString Error;
	TestFalse(TEXT("Open-ended verbosity rejected"),
		FHyperAIStudioLogTailContracts::NormalizeRequest(Invalid, Normalized, ErrorCode, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioViewportCaptureGrantIssuerTest,
	"HyperAIStudio.NativeTools.Diagnostics.ViewportCaptureGrantIssuer",
	HyperAIStudio::DiagnosticsTests::Private::Flags)

bool FHyperAIStudioViewportCaptureGrantIssuerTest::RunTest(const FString& Parameters)
{
	FDateTime FakeNow(2026, 8, 15, 12, 0, 0);
	FHyperAIStudioViewportCaptureGrantIssuer Issuer([&FakeNow] { return FakeNow; });

	FHyperAIStudioDiagnosticsViewportCaptureRequest SourceRequest;
	SourceRequest.bConfirmFileWrite = true;
	SourceRequest.OperationId = TEXT("capture_grant_0001");
	SourceRequest.AuthorizationToken = TEXT("normalization-placeholder-token");
	FHyperAIStudioDiagnosticsNormalizedViewportCaptureRequest Normalized;
	FString ErrorCode;
	FString Error;
	if (!TestTrue(TEXT("Grant test request normalizes"),
		FHyperAIStudioViewportCaptureContracts::NormalizeRequest(
			SourceRequest, Normalized, ErrorCode, Error)))
	{
		return false;
	}

	const auto MakeRequest = [](const FHyperAIStudioViewportCaptureGrant& Grant)
	{
		FHyperAIStudioViewportCaptureAuthorizationRequest Request;
		Request.Token = Grant.Token;
		Request.OperationId = Grant.OperationId;
		Request.CanonicalProjectId = Grant.CanonicalProjectId;
		Request.EffectHash = Grant.EffectHash;
		return Request;
	};
	const auto Issue = [&Issuer, &Normalized, &ErrorCode, &Error](
		FHyperAIStudioViewportCaptureGrant& OutGrant)
	{
		return Issuer.IssueForRequest(
			Normalized, FTimespan::FromSeconds(30), OutGrant, ErrorCode, Error);
	};

	FHyperAIStudioViewportCaptureGrant ReplayGrant;
	if (!TestTrue(TEXT("Trusted issuer creates one exact opaque grant"), Issue(ReplayGrant)))
	{
		return false;
	}
	TestFalse(TEXT("Issuer binds a canonical project"), ReplayGrant.CanonicalProjectId.IsEmpty());
	TestFalse(TEXT("Issuer binds an effect fingerprint"), ReplayGrant.EffectHash.IsEmpty());
	TestEqual(TEXT("Issuer binds the operation id"), ReplayGrant.OperationId, Normalized.OperationId);
	FHyperAIStudioViewportCaptureAuthorizationReceipt Receipt;
	FString GateError;
	const FHyperAIStudioViewportCaptureAuthorizationRequest ExactReplayRequest =
		MakeRequest(ReplayGrant);
	TestTrue(TEXT("Exact binding consumes grant once"),
		Issuer.ConsumeExact(ExactReplayRequest, Receipt, GateError));
	TestTrue(TEXT("Successful receipt records consumption"), Receipt.bConsumed);
	TestFalse(TEXT("Consumed token cannot be replayed"),
		Issuer.ConsumeExact(ExactReplayRequest, Receipt, GateError));

	FHyperAIStudioViewportCaptureGrant OperationGrant;
	TestTrue(TEXT("Operation-binding grant issues"), Issue(OperationGrant));
	FHyperAIStudioViewportCaptureAuthorizationRequest WrongOperation = MakeRequest(OperationGrant);
	WrongOperation.OperationId = TEXT("capture_grant_wrong_operation");
	TestFalse(TEXT("Wrong operation id is denied"),
		Issuer.ConsumeExact(WrongOperation, Receipt, GateError));
	TestTrue(TEXT("Wrong operation attempt cannot consume the valid binding"),
		Issuer.ConsumeExact(MakeRequest(OperationGrant), Receipt, GateError));

	FHyperAIStudioViewportCaptureGrant ProjectGrant;
	TestTrue(TEXT("Project-binding grant issues"), Issue(ProjectGrant));
	FHyperAIStudioViewportCaptureAuthorizationRequest WrongProject = MakeRequest(ProjectGrant);
	WrongProject.CanonicalProjectId = TEXT("sha1:0000000000000000000000000000000000000000");
	TestFalse(TEXT("Wrong canonical project is denied"),
		Issuer.ConsumeExact(WrongProject, Receipt, GateError));
	TestTrue(TEXT("Wrong project attempt cannot consume the valid binding"),
		Issuer.ConsumeExact(MakeRequest(ProjectGrant), Receipt, GateError));

	FHyperAIStudioViewportCaptureGrant EffectGrant;
	TestTrue(TEXT("Effect-binding grant issues"), Issue(EffectGrant));
	FHyperAIStudioViewportCaptureAuthorizationRequest WrongEffect = MakeRequest(EffectGrant);
	WrongEffect.EffectHash = TEXT("sha1:1111111111111111111111111111111111111111");
	TestFalse(TEXT("Wrong effect fingerprint is denied"),
		Issuer.ConsumeExact(WrongEffect, Receipt, GateError));
	TestTrue(TEXT("Wrong effect attempt cannot consume the valid binding"),
		Issuer.ConsumeExact(MakeRequest(EffectGrant), Receipt, GateError));

	FHyperAIStudioViewportCaptureGrant ExpiringGrant;
	TestTrue(TEXT("Expiring grant issues"),
		Issuer.IssueForRequest(
			Normalized, FTimespan::FromSeconds(1), ExpiringGrant, ErrorCode, Error));
	FakeNow = FakeNow + FTimespan::FromSeconds(1);
	TestFalse(TEXT("Grant is denied at its exact expiry"),
		Issuer.ConsumeExact(MakeRequest(ExpiringGrant), Receipt, GateError));

	FHyperAIStudioViewportCaptureGrant TooLongGrant;
	TestFalse(TEXT("Unbounded lifetime is rejected"),
		Issuer.IssueForRequest(
			Normalized,
			FTimespan::FromSeconds(FHyperAIStudioViewportCaptureGrantIssuer::MaxLifetimeSeconds + 1),
			TooLongGrant,
			ErrorCode,
			Error));
	TestEqual(TEXT("Lifetime rejection is explicit"), ErrorCode,
		FString(TEXT("invalid_authorization_lifetime")));

	FHyperAIStudioViewportCaptureGrantIssuer BoundedIssuer([&FakeNow] { return FakeNow; });
	FHyperAIStudioViewportCaptureGrant FirstBoundedGrant;
	for (int32 Index = 0;
		Index < FHyperAIStudioViewportCaptureGrantIssuer::MaxActiveGrants;
		++Index)
	{
		FHyperAIStudioViewportCaptureGrant Grant;
		if (!TestTrue(TEXT("Bounded grant slot issues"),
			BoundedIssuer.IssueForRequest(
				Normalized, FTimespan::FromSeconds(30), Grant, ErrorCode, Error)))
		{
			return false;
		}
		if (Index == 0)
		{
			FirstBoundedGrant = Grant;
		}
	}
	FHyperAIStudioViewportCaptureGrant Overflow;
	TestFalse(TEXT("Grant store never exceeds its hard bound"),
		BoundedIssuer.IssueForRequest(
			Normalized, FTimespan::FromSeconds(30), Overflow, ErrorCode, Error));
	TestEqual(TEXT("Capacity rejection is explicit"), ErrorCode,
		FString(TEXT("authorization_grant_capacity_reached")));

	FHyperAIStudioViewportCaptureGrant WipedGrant;
	BoundedIssuer.Shutdown();
	TestEqual(TEXT("Shutdown wipes every outstanding grant"),
		BoundedIssuer.GetActiveGrantCount(), 0);
	TestFalse(TEXT("A grant retained by the caller is invalid after shutdown wipe"),
		BoundedIssuer.ConsumeExact(MakeRequest(FirstBoundedGrant), Receipt, GateError));
	TestFalse(TEXT("Shutdown issuer cannot issue another grant"),
		BoundedIssuer.IssueForRequest(
			Normalized, FTimespan::FromSeconds(30), WipedGrant, ErrorCode, Error));

	FHyperAIStudioDiagnosticsRegistration InactiveRegistration;
	FHyperAIStudioDiagnosticsViewportCaptureRequest TrustedRequest = SourceRequest;
	TrustedRequest.AuthorizationToken.Reset();
	TestFalse(TEXT("Inactive lifecycle defaults to denying issuance"),
		InactiveRegistration.IssueViewportCaptureGrant(
			TrustedRequest,
			FTimespan::FromSeconds(30),
			WipedGrant,
			ErrorCode,
			Error));
	TestTrue(TEXT("Denied lifecycle cannot inject a token"),
		TrustedRequest.AuthorizationToken.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioViewportCaptureContractTest,
	"HyperAIStudio.NativeTools.Diagnostics.ViewportCaptureContract",
	HyperAIStudio::DiagnosticsTests::Private::Flags)

bool FHyperAIStudioViewportCaptureContractTest::RunTest(const FString& Parameters)
{
	FHyperAIStudioDiagnosticsViewportCaptureRequest Request;
	FHyperAIStudioDiagnosticsNormalizedViewportCaptureRequest Normalized;
	FString ErrorCode;
	FString Error;
	TestFalse(TEXT("File write requires explicit confirmation"),
		FHyperAIStudioViewportCaptureContracts::NormalizeRequest(
			Request, Normalized, ErrorCode, Error));
	TestEqual(TEXT("Confirmation error stable"), ErrorCode,
		FString(TEXT("file_write_confirmation_required")));
	Request.bConfirmFileWrite = true;
	Request.OperationId = TEXT("capture_123456");
	Request.AuthorizationToken = TEXT("opaque-test-grant-1234567890");
	TestTrue(TEXT("Bounded capture request normalizes"),
		FHyperAIStudioViewportCaptureContracts::NormalizeRequest(
			Request, Normalized, ErrorCode, Error));
	TestFalse(TEXT("Oversized viewport rejected before pixels"),
		FHyperAIStudioViewportCaptureContracts::ValidateViewportDimensions(
			1921, 1080, Normalized, ErrorCode, Error));
	TestEqual(TEXT("Pixel bound error stable"), ErrorCode,
		FString(TEXT("viewport_exceeds_pixel_bound")));
	const FString Name = FHyperAIStudioViewportCaptureContracts::MakeServerFileName(
		TEXT("2026-08-14T12:34:56.000Z"),
		TEXT("sha1:0123456789abcdef0123456789abcdef01234567"));
	TestTrue(TEXT("Server filename validates"),
		FHyperAIStudioViewportCaptureContracts::IsServerFileName(Name));
	TestFalse(TEXT("Traversal filename rejected"),
		FHyperAIStudioViewportCaptureContracts::IsServerFileName(TEXT("viewport-../outside.png")));

	const FString ProjectId = FHyperAIStudioViewportCaptureContracts::ComputeCanonicalProjectId();
	const FString EffectHash = FHyperAIStudioViewportCaptureContracts::ComputeAuthorizationEffectHash(
		Normalized, ProjectId);
	TestFalse(TEXT("Project binding is hashed"), ProjectId.IsEmpty());
	TestFalse(TEXT("External effect is exactly hashed"), EffectHash.IsEmpty());
	FHyperAIStudioViewportCaptureAuthorizationRequest AuthorizationRequest;
	AuthorizationRequest.Token = Normalized.AuthorizationToken;
	AuthorizationRequest.OperationId = Normalized.OperationId;
	AuthorizationRequest.CanonicalProjectId = ProjectId;
	AuthorizationRequest.EffectHash = EffectHash;

	FHyperAIStudioViewportCaptureAuthorization::ResetGate();
	TestFalse(TEXT("Client confirmation and token do not self-authorize"),
		FHyperAIStudioViewportCaptureAuthorization::ConsumeExact(
			AuthorizationRequest, ErrorCode, Error));
	TestEqual(TEXT("Missing trusted issuer fails closed"), ErrorCode,
		FString(TEXT("authorization_gate_unavailable")));
	const FHyperAIStudioDiagnosticsViewportCaptureResult DefaultDenied =
		UHyperAIStudioViewportCaptureToolset::hyper_viewport_capture(Request);
	TestFalse(TEXT("No trusted gate never creates a file"), DefaultDenied.bFileCreated);
	TestEqual(TEXT("Denied/preflight capture has no filesystem effect"),
		DefaultDenied.FilesystemEffect, FString(TEXT("none")));

	using FCountingGate = HyperAIStudio::DiagnosticsTests::Private::FCountingViewportAuthorizationGate;
	TSharedPtr<FCountingGate, ESPMode::ThreadSafe> CountingGate =
		MakeShared<FCountingGate, ESPMode::ThreadSafe>();
	FHyperAIStudioViewportCaptureAuthorization::SetGate(CountingGate);
	FHyperAIStudioDiagnosticsViewportCaptureRequest PreflightFailure = Request;
	PreflightFailure.MaxWidth = 1;
	PreflightFailure.MaxHeight = 1;
	PreflightFailure.MaxPixels = 1;
	const FHyperAIStudioDiagnosticsViewportCaptureResult PreflightResult =
		UHyperAIStudioViewportCaptureToolset::hyper_viewport_capture(PreflightFailure);
	TestFalse(TEXT("Preflight failure creates no file"), PreflightResult.bFileCreated);
	TestEqual(TEXT("Preflight failure does not consume grant"), CountingGate->Calls, 0);

	CountingGate->bReturnMismatchedReceipt = true;
	TestFalse(TEXT("Mismatched trusted receipt fails closed"),
		FHyperAIStudioViewportCaptureAuthorization::ConsumeExact(
			AuthorizationRequest, ErrorCode, Error));
	TestEqual(TEXT("Receipt mismatch code stable"), ErrorCode,
		FString(TEXT("authorization_receipt_mismatch")));
	TestEqual(TEXT("Mismatched receipt invokes gate once"), CountingGate->Calls, 1);
	CountingGate->Calls = 0;
	CountingGate->bReturnMismatchedReceipt = false;
	TestTrue(TEXT("Valid post-preflight authorization consumes exactly once"),
		FHyperAIStudioViewportCaptureAuthorization::ConsumeExact(
			AuthorizationRequest, ErrorCode, Error));
	TestEqual(TEXT("Trusted gate called exactly once"), CountingGate->Calls, 1);
	FHyperAIStudioViewportCaptureAuthorization::ResetGate();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioDiagnosticsRegistrationContractTest,
	"HyperAIStudio.NativeTools.Diagnostics.RegistrationContract",
	HyperAIStudio::DiagnosticsTests::Private::Flags)

bool FHyperAIStudioDiagnosticsRegistrationContractTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::DiagnosticsTests::Private;
	const TArray<FHyperAIStudioDiagnosticsToolBinding>& Bindings =
		FHyperAIStudioDiagnosticsRegistration::GetBindings();
	TestEqual(TEXT("Exactly four remaining diagnostics tools bound"), Bindings.Num(), 4);
	TSet<FString> Names;
	for (const FHyperAIStudioDiagnosticsToolBinding& Binding : Bindings)
	{
		TestNotNull(TEXT("Binding owns exact class"), Binding.ToolsetClass);
		TestEqual(TEXT("Qualified name matches reflected class"),
			Binding.QualifiedToolset,
			TEXT("HyperAIStudio.") + Binding.ToolsetClass->GetName());
		Names.Add(Binding.ToolName);

		int32 AICallableCount = 0;
		for (TFieldIterator<UFunction> It(Binding.ToolsetClass, EFieldIteratorFlags::ExcludeSuper); It; ++It)
		{
			if (!It->HasMetaData(TEXT("AICallable")))
			{
				continue;
			}
			++AICallableCount;
			TestEqual(TEXT("Reflected callable name matches exact binding"), It->GetName(), Binding.ToolName);
			UFunction* Function = *It;
			const TValueOrError<bool, FString> Callability =
				UToolsetDefinition::IsFunctionAICallable(Function);
			TestTrue(TEXT("Epic accepts callable schema"), Callability.HasValue() && Callability.GetValue());
		}
		TestEqual(TEXT("Each cohort exposes one callable"), AICallableCount, 1);

		const FHyperAIStudioCapabilityToolDefinition* Tool = CatalogTool(Binding.ToolName);
		TestNotNull(TEXT("Generated catalog owns tool"), Tool);
		if (Tool)
		{
			TestEqual(TEXT("Generated pack id exact"), Tool->PackId, FString(TEXT("diagnostics")));
			const bool ExpectedDefault =
				Tool->AdmissionState == EHyperAIStudioCapabilityAdmissionState::Admitted;
			const bool ExpectedPending = ExpectedDefault
				|| Tool->AdmissionState == EHyperAIStudioCapabilityAdmissionState::SourceCandidate;
			TestEqual(TEXT("Default registration follows generated admission"),
				FHyperAIStudioDiagnosticsRegistration::IsRegistrationAllowed(Binding.ToolName, false),
				ExpectedDefault);
			TestEqual(TEXT("Dev registration follows generated source-candidate state"),
				FHyperAIStudioDiagnosticsRegistration::IsRegistrationAllowed(Binding.ToolName, true),
				ExpectedPending);
		}
	}
	TestTrue(TEXT("Diagnose bound"), Names.Contains(TEXT("hyper_diagnose")));
	TestTrue(TEXT("Audit bound"), Names.Contains(TEXT("hyper_run_audit")));
	TestTrue(TEXT("Log bound"), Names.Contains(TEXT("hyper_log_tail")));
	TestTrue(TEXT("Viewport bound"), Names.Contains(TEXT("hyper_viewport_capture")));
	TestFalse(TEXT("Existing dependency graph is not duplicated"),
		Names.Contains(TEXT("hyper_asset_dependency_graph")));
	const FHyperAIStudioCapabilityToolDefinition* Viewport = CatalogTool(TEXT("hyper_viewport_capture"));
	TestTrue(TEXT("Generated catalog marks viewport file effect"),
		Viewport && Viewport->bMayCauseExternalEffects);
	return true;
}

#endif
