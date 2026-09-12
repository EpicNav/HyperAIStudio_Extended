// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HyperAIStudioOperationJournal.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

namespace HyperAIStudio::OperationJournal::Tests
{
	FString Sha256Token(const TCHAR HexDigit)
	{
		FString Result(TEXT("sha256:"));
		for (int32 Index = 0; Index < 64; ++Index)
		{
			Result.AppendChar(HexDigit);
		}
		return Result;
	}

	FHyperAIStudioOperationEvidence MakeEvidence(
		const FString& CanonicalProjectId,
		const FString& OperationId,
		const FString& PlanHash,
		const FString& CapabilityHash,
		const FString& ValidatorId = TEXT("hyperai.verify_fresh"))
	{
		const FDateTime Now = FDateTime::UtcNow();
		const int64 NowMs = Now.ToUnixTimestamp() * 1000 + Now.GetMillisecond();
		FHyperAIStudioOperationEvidence Evidence;
		Evidence.Version = FHyperAIStudioOperationEvidence::CurrentVersion;
		Evidence.CanonicalProjectId = CanonicalProjectId;
		Evidence.OperationId = OperationId;
		Evidence.PlanHash = PlanHash;
		Evidence.CapabilityHash = CapabilityHash;
		Evidence.EffectFingerprint = Sha256Token(TEXT('5'));
		Evidence.ActionNonce = TEXT("action-receipt-nonce-001");
		Evidence.ValidatorId = ValidatorId;
		Evidence.ApprovedValidatorFingerprint = Sha256Token(TEXT('6'));
		Evidence.PostconditionHash = Sha256Token(TEXT('7'));
		Evidence.ReceiptFingerprint = Sha256Token(TEXT('8'));
		Evidence.IssuedUtcMs = NowMs - 1000;
		Evidence.ExpiresUtcMs = NowMs + 60000;
		return Evidence;
	}

	bool IsLowerHexSha1(const FString& Value)
	{
		if (Value.Len() != 40)
		{
			return false;
		}
		for (const TCHAR Character : Value)
		{
			if (!(Character >= TEXT('0') && Character <= TEXT('9'))
				&& !(Character >= TEXT('a') && Character <= TEXT('f')))
			{
				return false;
			}
		}
		return true;
	}

#if PLATFORM_WINDOWS
	bool TryCreateDirectorySymlink(const FString& LinkPath, const FString& TargetPath, uint32& OutWin32Error)
	{
		constexpr DWORD DirectoryFlag = 0x1;
		constexpr DWORD AllowUnprivilegedCreateFlag = 0x2;
		if (::CreateSymbolicLinkW(*LinkPath, *TargetPath, DirectoryFlag | AllowUnprivilegedCreateFlag))
		{
			OutWin32Error = ERROR_SUCCESS;
			return true;
		}
		OutWin32Error = static_cast<uint32>(::GetLastError());
		if (OutWin32Error == ERROR_INVALID_PARAMETER
			&& ::CreateSymbolicLinkW(*LinkPath, *TargetPath, DirectoryFlag))
		{
			OutWin32Error = ERROR_SUCCESS;
			return true;
		}
		OutWin32Error = static_cast<uint32>(::GetLastError());
		return false;
	}

	FString TryGetShortDirectoryPath(const FString& LongPath)
	{
		const DWORD Required = ::GetShortPathNameW(*LongPath, nullptr, 0);
		if (Required == 0 || Required > 32768)
		{
			return {};
		}
		TArray<TCHAR> Buffer;
		Buffer.SetNumZeroed(static_cast<int32>(Required + 1));
		const DWORD Written = ::GetShortPathNameW(*LongPath, Buffer.GetData(), Buffer.Num());
		if (Written == 0 || Written >= static_cast<DWORD>(Buffer.Num()))
		{
			return {};
		}
		FString Result(static_cast<int32>(Written), Buffer.GetData());
		FPaths::NormalizeDirectoryName(Result);
		return Result;
	}
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioOperationJournalTest,
	"HyperAIStudio.NativeTools.OperationJournal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioOperationJournalTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::OperationJournal::Tests;

	const FString TestRoot = FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("HyperAIStudioTests"),
		FGuid::NewGuid().ToString(EGuidFormats::Digits));
	IFileManager::Get().MakeDirectory(*TestRoot, true);
	ON_SCOPE_EXIT
	{
		IFileManager::Get().DeleteDirectory(*TestRoot, false, true);
	};

	const FString PlanA = Sha256Token(TEXT('a'));
	const FString PlanB = Sha256Token(TEXT('b'));
	const FString PlanC = Sha256Token(TEXT('c'));
	const FString PlanD = Sha256Token(TEXT('d'));
	const FString CapabilityA = Sha256Token(TEXT('1'));
	const FString CapabilityB = Sha256Token(TEXT('2'));
	const FString ValidatorHash = Sha256Token(TEXT('3'));
	const FString PostconditionHash = Sha256Token(TEXT('4'));

	const FString CanonicalProjectId = FHyperAIStudioOperationJournal::MakeCanonicalProjectId(TestRoot);
	TestTrue(TEXT("Existing project directory resolves to a path-free lowercase SHA-1 id"), IsLowerHexSha1(CanonicalProjectId));
	TestEqual(
		TEXT("Dot/trailing-separator spelling resolves to the same physical directory identity"),
		FHyperAIStudioOperationJournal::MakeCanonicalProjectId(TestRoot + TEXT("/./")),
		CanonicalProjectId);

	const FString MissingRoot = FPaths::Combine(TestRoot, TEXT("missing-project-root"));
	TestTrue(TEXT("A nonexistent project directory has no canonical identity"),
		FHyperAIStudioOperationJournal::MakeCanonicalProjectId(MissingRoot).IsEmpty());
	FString Error;
	FHyperAIStudioOperationRecord Record;
	{
		FHyperAIStudioOperationJournal Missing(MissingRoot, 16);
		TestFalse(TEXT("Journal ownership fails closed when physical identity cannot be resolved"), Missing.Load(Error));
		TestEqual(
			TEXT("Mutation remains unavailable after identity-resolution failure"),
			Missing.BeginOperation(TEXT("operation-missing"), PlanA, CapabilityA, Record, Error),
			EHyperAIStudioOperationBeginResult::OwnerUnavailable);
	}

	FString SymlinkRoot;
	bool bSymlinkCreated = false;
#if PLATFORM_WINDOWS
	SymlinkRoot = TestRoot + TEXT("-reparse-alias");
	uint32 SymlinkError = ERROR_SUCCESS;
	bSymlinkCreated = TryCreateDirectorySymlink(SymlinkRoot, TestRoot, SymlinkError);
	ON_SCOPE_EXIT
	{
		if (bSymlinkCreated)
		{
			::RemoveDirectoryW(*SymlinkRoot);
		}
	};
	if (bSymlinkCreated)
	{
		TestEqual(
			TEXT("A directory reparse alias resolves to the target directory identity"),
			FHyperAIStudioOperationJournal::MakeCanonicalProjectId(SymlinkRoot),
			CanonicalProjectId);
	}
	else
	{
		AddInfo(FString::Printf(
			TEXT("Directory-symlink alias assertion skipped because this Windows policy denied link creation (error %u)."),
			SymlinkError));
	}

	const FString ShortRoot = TryGetShortDirectoryPath(TestRoot);
	if (!ShortRoot.IsEmpty() && !ShortRoot.Equals(TestRoot, ESearchCase::IgnoreCase))
	{
		TestEqual(
			TEXT("An available 8.3 spelling resolves to the same physical directory identity"),
			FHyperAIStudioOperationJournal::MakeCanonicalProjectId(ShortRoot),
			CanonicalProjectId);
	}
	else
	{
		AddInfo(TEXT("8.3 alias assertion skipped because short-name generation is unavailable for the test directory."));
	}
#endif

	{
		FHyperAIStudioOperationJournal Unloaded(TestRoot, 16);
		TestEqual(
			TEXT("Mutation before Load/reconciliation fails closed"),
			Unloaded.BeginOperation(TEXT("operation-unloaded"), PlanA, CapabilityA, Record, Error),
			EHyperAIStudioOperationBeginResult::OwnerUnavailable);
	}

	FString SlotAPath;
	FString SlotBPath;
	int64 LastGeneration = 0;
	{
		FHyperAIStudioOperationJournal Journal(TestRoot, 16);
		SlotAPath = Journal.GetSlotAPath();
		SlotBPath = Journal.GetSlotBPath();
		TestTrue(TEXT("A missing two-generation journal loads as empty"), Journal.Load(Error));
		TestTrue(TEXT("Successful Load/reconciliation is observable"), Journal.IsLoaded());
		TestEqual(TEXT("Journal starts empty"), Journal.GetRecordsSnapshot().Num(), 0);
		TestEqual(TEXT("Empty Load has nothing to reconcile"), Journal.GetLastLoadReconciledCount(), 0);
		TestTrue(TEXT("Execution identity is generated internally"), Journal.GetTrustedExecutionInstanceId().StartsWith(TEXT("editor-")));

		FHyperAIStudioOperationJournal CompetingOwner(TestRoot, 16);
		TestFalse(TEXT("A second owner for the canonical project is rejected"), CompetingOwner.Load(Error));
#if PLATFORM_WINDOWS
		FString CaseAlias = TestRoot.ToUpper();
		FHyperAIStudioOperationJournal CaseAliasOwner(CaseAlias, 16);
		TestFalse(TEXT("Case-alias spelling cannot acquire a second owner"), CaseAliasOwner.Load(Error));
		if (bSymlinkCreated)
		{
			FHyperAIStudioOperationJournal ReparseAliasOwner(SymlinkRoot, 16);
			TestFalse(TEXT("A reparse alias cannot acquire a second journal owner"), ReparseAliasOwner.Load(Error));
			TestTrue(TEXT("Alias journal paths are rooted at the resolved target"),
				ReparseAliasOwner.GetSlotAPath().Equals(SlotAPath, ESearchCase::IgnoreCase));
		}
		if (!ShortRoot.IsEmpty() && !ShortRoot.Equals(TestRoot, ESearchCase::IgnoreCase))
		{
			FHyperAIStudioOperationJournal ShortAliasOwner(ShortRoot, 16);
			TestFalse(TEXT("An 8.3 alias cannot acquire a second journal owner"), ShortAliasOwner.Load(Error));
		}
#endif

		TestEqual(
			TEXT("Short operation id is rejected"),
			Journal.BeginOperation(TEXT("short"), PlanA, CapabilityA, Record, Error),
			EHyperAIStudioOperationBeginResult::Invalid);
		TestEqual(
			TEXT("Printable arbitrary text is not accepted as a plan hash"),
			Journal.BeginOperation(TEXT("operation-bad-hash"), TEXT("could-be-a-secret"), CapabilityA, Record, Error),
			EHyperAIStudioOperationBeginResult::Invalid);

		const FString OperationId = TEXT("operation-0001");
		TestEqual(
			TEXT("First operation is durably created"),
			Journal.BeginOperation(OperationId, PlanA, CapabilityA, Record, Error),
			EHyperAIStudioOperationBeginResult::Created);
		TestEqual(TEXT("New operation is queued"), Record.State, EHyperAIStudioOperationState::Queued);
		TestEqual(TEXT("Record is bound to the journal's trusted execution instance"),
			Record.ExecutionInstanceId, Journal.GetTrustedExecutionInstanceId());
		TestTrue(TEXT("First save creates a validated generation"), Journal.GetGeneration() > 0);
		{
			FHyperAIStudioOperationJournal ReadOnly(TestRoot, 16);
			TestTrue(TEXT("Read-only status can load while the mutation owner remains active"),
				ReadOnly.LoadReadOnly(Error));
			const TOptional<FHyperAIStudioOperationRecord> ReadOnlyRecord = ReadOnly.Find(OperationId);
			TestTrue(TEXT("Read-only load exposes the validated record"), ReadOnlyRecord.IsSet());
			if (ReadOnlyRecord.IsSet())
			{
				TestEqual(TEXT("Read-only load never reconciles unfinished work"),
					ReadOnlyRecord->State, EHyperAIStudioOperationState::Queued);
			}
			TestEqual(TEXT("Read-only load reports no reconciliation writes"),
				ReadOnly.GetLastLoadReconciledCount(), 0);
			TestEqual(TEXT("Read-only journal cannot admit a mutation"),
				ReadOnly.BeginOperation(TEXT("operation-read-only"), PlanA, CapabilityA, Record, Error),
				EHyperAIStudioOperationBeginResult::OwnerUnavailable);
		}

		TestEqual(
			TEXT("Same id, plan, and capabilities are idempotent"),
			Journal.BeginOperation(OperationId, PlanA, CapabilityA, Record, Error),
			EHyperAIStudioOperationBeginResult::Existing);
		TestEqual(
			TEXT("Same id cannot bind to a different plan"),
			Journal.BeginOperation(OperationId, PlanB, CapabilityA, Record, Error),
			EHyperAIStudioOperationBeginResult::Conflict);
		TestEqual(
			TEXT("Same id cannot bypass inventory revalidation"),
			Journal.BeginOperation(OperationId, PlanA, CapabilityB, Record, Error),
			EHyperAIStudioOperationBeginResult::Conflict);

		TestTrue(TEXT("queued -> running"), Journal.Transition(
			OperationId,
			EHyperAIStudioOperationState::Queued,
			EHyperAIStudioOperationState::Running,
			EHyperAIStudioRollbackState::NotNeeded,
			false,
			false,
			TEXT("validated"),
			Record,
			Error));
		TestFalse(TEXT("A partial/mutating result cannot occur before commit_started"), Journal.Transition(
			OperationId,
			EHyperAIStudioOperationState::Running,
			EHyperAIStudioOperationState::Partial,
			EHyperAIStudioRollbackState::Partial,
			false,
			true,
			TEXT("partial"),
			Record,
			Error));
		TestFalse(TEXT("An unknown mutation outcome cannot occur before commit_started"), Journal.Transition(
			OperationId,
			EHyperAIStudioOperationState::Running,
			EHyperAIStudioOperationState::OutcomeUnknown,
			EHyperAIStudioRollbackState::Unknown,
			false,
			true,
			TEXT("unknown"),
			Record,
			Error));
		TestTrue(TEXT("running -> commit_started"), Journal.Transition(
			OperationId,
			EHyperAIStudioOperationState::Running,
			EHyperAIStudioOperationState::CommitStarted,
			EHyperAIStudioRollbackState::NotNeeded,
			false,
			false,
			TEXT("commit_started"),
			Record,
			Error));
			TestFalse(TEXT("A started commit cannot collapse into retry-safe failed"), Journal.Transition(
			OperationId,
			EHyperAIStudioOperationState::CommitStarted,
			EHyperAIStudioOperationState::Failed,
			EHyperAIStudioRollbackState::Unknown,
			true,
			false,
			TEXT("commit_failed"),
				Record,
				Error));
			TestFalse(TEXT("Completed transition rejects legacy evidence-free finalization"), Journal.Transition(
				OperationId,
				EHyperAIStudioOperationState::CommitStarted,
				EHyperAIStudioOperationState::Completed,
				EHyperAIStudioRollbackState::NotNeeded,
				true,
				false,
				TEXT("postconditions_passed"),
				Record,
				Error));
			FHyperAIStudioOperationEvidence WrongOperationEvidence = MakeEvidence(
				CanonicalProjectId, OperationId, PlanA, CapabilityA);
			WrongOperationEvidence.OperationId = TEXT("operation-other-001");
			TestFalse(TEXT("Completed transition rejects a receipt bound to another operation"), Journal.Transition(
				OperationId,
				EHyperAIStudioOperationState::CommitStarted,
				EHyperAIStudioOperationState::Completed,
				EHyperAIStudioRollbackState::NotNeeded,
				true,
				false,
				TEXT("postconditions_passed"),
				WrongOperationEvidence,
				Record,
				Error));
			TestTrue(TEXT("commit_started -> completed"), Journal.Transition(
			OperationId,
			EHyperAIStudioOperationState::CommitStarted,
			EHyperAIStudioOperationState::Completed,
			EHyperAIStudioRollbackState::NotNeeded,
			true,
				false,
				TEXT("postconditions_passed"),
				MakeEvidence(CanonicalProjectId, OperationId, PlanA, CapabilityA),
				Record,
			Error));
		TestFalse(TEXT("Terminal operations cannot restart"), Journal.Transition(
			OperationId,
			EHyperAIStudioOperationState::Completed,
			EHyperAIStudioOperationState::Running,
			EHyperAIStudioRollbackState::NotNeeded,
			false,
			false,
			FString(),
			Record,
			Error));
		TestEqual(
			TEXT("Completed replay returns the recorded result state"),
			Journal.BeginOperation(OperationId, PlanA, CapabilityA, Record, Error),
			EHyperAIStudioOperationBeginResult::ReplayCompleted);

		FHyperAIStudioOperationRecord Interrupted;
		TestEqual(
			TEXT("Second operation is created"),
			Journal.BeginOperation(TEXT("operation-0002"), PlanC, CapabilityA, Interrupted, Error),
			EHyperAIStudioOperationBeginResult::Created);
		TestTrue(TEXT("Second operation enters pre-commit running"), Journal.Transition(
			TEXT("operation-0002"),
			EHyperAIStudioOperationState::Queued,
			EHyperAIStudioOperationState::Running,
			EHyperAIStudioRollbackState::NotNeeded,
			false,
			false,
			FString(),
			Interrupted,
			Error));

		FHyperAIStudioOperationRecord CommitInterrupted;
		TestEqual(TEXT("Third operation is created"),
			Journal.BeginOperation(TEXT("operation-0003"), PlanD, CapabilityA, CommitInterrupted, Error),
			EHyperAIStudioOperationBeginResult::Created);
		TestTrue(TEXT("Third operation enters running"), Journal.Transition(
			TEXT("operation-0003"),
			EHyperAIStudioOperationState::Queued,
			EHyperAIStudioOperationState::Running,
			EHyperAIStudioRollbackState::NotNeeded,
			false,
			false,
			FString(),
			CommitInterrupted,
			Error));
		TestTrue(TEXT("Third operation starts commit"), Journal.Transition(
			TEXT("operation-0003"),
			EHyperAIStudioOperationState::Running,
			EHyperAIStudioOperationState::CommitStarted,
			EHyperAIStudioRollbackState::NotNeeded,
			false,
			false,
			FString(),
			CommitInterrupted,
			Error));

		FHyperAIStudioOperationRecord EvidenceResolved;
		TestEqual(TEXT("Fourth operation is created"),
			Journal.BeginOperation(TEXT("operation-0004"), PlanB, CapabilityA, EvidenceResolved, Error),
			EHyperAIStudioOperationBeginResult::Created);
		TestTrue(TEXT("Fourth operation enters running"), Journal.Transition(
			TEXT("operation-0004"), EHyperAIStudioOperationState::Queued, EHyperAIStudioOperationState::Running,
			EHyperAIStudioRollbackState::NotNeeded, false, false, FString(), EvidenceResolved, Error));
		TestTrue(TEXT("Fourth operation starts commit"), Journal.Transition(
			TEXT("operation-0004"), EHyperAIStudioOperationState::Running, EHyperAIStudioOperationState::CommitStarted,
			EHyperAIStudioRollbackState::NotNeeded, false, false, FString(), EvidenceResolved, Error));
		TestFalse(TEXT("Unknown outcome must keep sticky may-have-commit state"), Journal.Transition(
			TEXT("operation-0004"), EHyperAIStudioOperationState::CommitStarted, EHyperAIStudioOperationState::OutcomeUnknown,
			EHyperAIStudioRollbackState::Unknown, false, false, TEXT("response_lost"), EvidenceResolved, Error));
		TestTrue(TEXT("Commit-started operation may become retry-unsafe outcome_unknown"), Journal.Transition(
			TEXT("operation-0004"), EHyperAIStudioOperationState::CommitStarted, EHyperAIStudioOperationState::OutcomeUnknown,
			EHyperAIStudioRollbackState::Unknown, false, true, TEXT("response_lost"), EvidenceResolved, Error));
		TestFalse(TEXT("Generic Transition cannot resolve outcome_unknown"), Journal.Transition(
			TEXT("operation-0004"), EHyperAIStudioOperationState::OutcomeUnknown, EHyperAIStudioOperationState::Completed,
			EHyperAIStudioRollbackState::NotNeeded, false, false, TEXT("resolved"), EvidenceResolved, Error));
		TestFalse(TEXT("Unknown resolution rejects arbitrary validator text"), Journal.ResolveUnknownWithEvidence(
			TEXT("operation-0004"), EHyperAIStudioUnknownResolution::Completed, TEXT("validator-secret"), PostconditionHash,
			EHyperAIStudioRollbackState::Unknown, TEXT("resolved"), EvidenceResolved, Error));
			const FHyperAIStudioOperationEvidence CompletedEvidence = MakeEvidence(
				CanonicalProjectId, TEXT("operation-0004"), PlanB, CapabilityA);
			TestTrue(TEXT("Bound server-verified validator evidence resolves unknown outcome"), Journal.ResolveUnknownWithEvidence(
				TEXT("operation-0004"), EHyperAIStudioUnknownResolution::Completed, CompletedEvidence,
				EHyperAIStudioRollbackState::Unknown, TEXT("resolved"), EvidenceResolved, Error));
			TestEqual(TEXT("Resolved outcome persists approved validator fingerprint"),
				EvidenceResolved.ResolutionValidatorHash, CompletedEvidence.ApprovedValidatorFingerprint);
			TestEqual(TEXT("Resolved outcome persists postcondition hash"),
				EvidenceResolved.ResolutionPostconditionHash, CompletedEvidence.PostconditionHash);
			TestEqual(TEXT("Resolved outcome persists its exact action nonce"),
				EvidenceResolved.TerminalEvidence.ActionNonce, CompletedEvidence.ActionNonce);
		TestEqual(TEXT("Resolver identity is the current trusted instance"),
			EvidenceResolved.ResolvedByExecutionInstanceId, Journal.GetTrustedExecutionInstanceId());

		FHyperAIStudioOperationRecord NoOpCompleted;
		TestEqual(TEXT("No-op operation is created"),
			Journal.BeginOperation(TEXT("operation-0005"), PlanA, CapabilityA, NoOpCompleted, Error),
			EHyperAIStudioOperationBeginResult::Created);
		TestTrue(TEXT("No-op operation enters running"), Journal.Transition(
			TEXT("operation-0005"), EHyperAIStudioOperationState::Queued, EHyperAIStudioOperationState::Running,
			EHyperAIStudioRollbackState::NotNeeded, false, false, FString(), NoOpCompleted, Error));
		TestFalse(TEXT("running cannot claim a mutating completion without the commit barrier"), Journal.Transition(
			TEXT("operation-0005"), EHyperAIStudioOperationState::Running, EHyperAIStudioOperationState::Completed,
			EHyperAIStudioRollbackState::NotNeeded, true, false, TEXT("postconditions_passed"), NoOpCompleted, Error));
				TestTrue(TEXT("A proven no-op may complete without entering commit_started"), Journal.Transition(
				TEXT("operation-0005"), EHyperAIStudioOperationState::Running, EHyperAIStudioOperationState::Completed,
				EHyperAIStudioRollbackState::NotNeeded, true, false, TEXT("no_changes"),
					MakeEvidence(CanonicalProjectId, TEXT("operation-0005"), PlanA, CapabilityA), NoOpCompleted, Error));

			FHyperAIStudioOperationRecord CertifiedNoEffect;
			TestEqual(TEXT("Certified-no-effect operation is created"),
				Journal.BeginOperation(TEXT("operation-0006"), PlanB, CapabilityA, CertifiedNoEffect, Error),
				EHyperAIStudioOperationBeginResult::Created);
			TestTrue(TEXT("Certified-no-effect operation enters running"), Journal.Transition(
				TEXT("operation-0006"), EHyperAIStudioOperationState::Queued, EHyperAIStudioOperationState::Running,
				EHyperAIStudioRollbackState::NotNeeded, false, false, TEXT("running"), CertifiedNoEffect, Error));
			TestTrue(TEXT("Certified-no-effect operation writes commit marker"), Journal.Transition(
				TEXT("operation-0006"), EHyperAIStudioOperationState::Running, EHyperAIStudioOperationState::CommitStarted,
				EHyperAIStudioRollbackState::NotNeeded, false, false, TEXT("commit_started"), CertifiedNoEffect, Error));
			TestFalse(TEXT("Malformed no-effect action nonce cannot close commit"),
				Journal.TransitionCertifiedNoEffect(
					TEXT("operation-0006"), Sha256Token(TEXT('9')), TEXT("bad nonce!"), CertifiedNoEffect, Error));
			TestTrue(TEXT("Exact core certificate closes commit as retry-safe failed"),
				Journal.TransitionCertifiedNoEffect(
					TEXT("operation-0006"), Sha256Token(TEXT('9')),
					TEXT("action-certified-no-effect-0006"), CertifiedNoEffect, Error));
			TestEqual(TEXT("Certified no-effect is terminal failed"),
				CertifiedNoEffect.State, EHyperAIStudioOperationState::Failed);
			TestTrue(TEXT("Certified no-effect is retry-safe"), CertifiedNoEffect.bRetrySafe);
			TestEqual(TEXT("Certificate persists exact effect binding"),
				CertifiedNoEffect.TerminalEvidence.EffectFingerprint, Sha256Token(TEXT('9')));
			TestEqual(TEXT("Certificate persists exact action binding"),
				CertifiedNoEffect.TerminalEvidence.ActionNonce,
				FString(TEXT("action-certified-no-effect-0006")));

		LastGeneration = Journal.GetGeneration();
		TestTrue(TEXT("Alternating saves leave both complete generation slots"),
			IFileManager::Get().FileExists(*SlotAPath) && IFileManager::Get().FileExists(*SlotBPath));
		const FString ActivePath = (LastGeneration % 2) == 1 ? SlotAPath : SlotBPath;
		FString ActiveJson;
		TestTrue(TEXT("Active journal generation is readable"), FFileHelper::LoadFileToString(ActiveJson, *ActivePath));
		TestFalse(TEXT("Journal JSON contains no project path"), ActiveJson.Contains(TestRoot, ESearchCase::IgnoreCase));
	}

	{
			FHyperAIStudioOperationJournal Reloaded(TestRoot, 16);
			TestTrue(TEXT("Highest valid generation reloads and reconciles before returning"), Reloaded.Load(Error));
			TestEqual(TEXT("Load automatically reconciles both unfinished operations"), Reloaded.GetLastLoadReconciledCount(), 2);
			TestEqual(TEXT("Reloaded completion replays only after schema-v3 bound evidence validates"),
				Reloaded.BeginOperation(TEXT("operation-0001"), PlanA, CapabilityA, Record, Error),
				EHyperAIStudioOperationBeginResult::ReplayCompleted);
			TestEqual(TEXT("Replay retains the bound effect fingerprint"),
				Record.TerminalEvidence.EffectFingerprint, Sha256Token(TEXT('5')));
			TestEqual(TEXT("Replay retains the trusted receipt fingerprint"),
				Record.TerminalEvidence.ReceiptFingerprint, Sha256Token(TEXT('8')));
			const TOptional<FHyperAIStudioOperationRecord> CertifiedReloaded =
				Reloaded.Find(TEXT("operation-0006"));
			TestTrue(TEXT("Certified no-effect terminal survives reload without reconciliation"),
				CertifiedReloaded.IsSet());
			if (CertifiedReloaded.IsSet())
			{
				TestEqual(TEXT("Reload retains certified failed state"),
					CertifiedReloaded->State, EHyperAIStudioOperationState::Failed);
				TestEqual(TEXT("Reload retains exact certified action nonce"),
					CertifiedReloaded->TerminalEvidence.ActionNonce,
					FString(TEXT("action-certified-no-effect-0006")));
			}
		const TOptional<FHyperAIStudioOperationRecord> PrecommitFailed = Reloaded.Find(TEXT("operation-0002"));
		TestTrue(TEXT("Interrupted pre-commit operation remains in the journal"), PrecommitFailed.IsSet());
		if (PrecommitFailed.IsSet())
		{
			TestEqual(TEXT("Pre-commit interruption becomes failed"), PrecommitFailed->State, EHyperAIStudioOperationState::Failed);
			TestTrue(TEXT("Pre-commit interruption is known retry-safe"), PrecommitFailed->bRetrySafe);
			TestFalse(TEXT("Pre-commit interruption cannot claim committed effects"), PrecommitFailed->bPartialCommit);
		}
		const TOptional<FHyperAIStudioOperationRecord> CommitUnknown = Reloaded.Find(TEXT("operation-0003"));
		TestTrue(TEXT("Commit-started interruption remains in the journal"), CommitUnknown.IsSet());
		if (CommitUnknown.IsSet())
		{
			TestEqual(TEXT("Commit-started interruption becomes outcome_unknown"), CommitUnknown->State, EHyperAIStudioOperationState::OutcomeUnknown);
			TestFalse(TEXT("Unknown outcomes are never retry-safe"), CommitUnknown->bRetrySafe);
			TestTrue(TEXT("Commit-started interruption keeps sticky possible effects"), CommitUnknown->bPartialCommit);
		}
		TestFalse(TEXT("Reloaded owner still cannot resolve unknown via generic Transition"), Reloaded.Transition(
			TEXT("operation-0003"), EHyperAIStudioOperationState::OutcomeUnknown, EHyperAIStudioOperationState::RolledBack,
			EHyperAIStudioRollbackState::Complete, false, false, TEXT("resolved"), Record, Error));
			const FHyperAIStudioOperationEvidence RollbackEvidence = MakeEvidence(
				CanonicalProjectId, TEXT("operation-0003"), PlanD, CapabilityA, TEXT("hyperai.verify_rollback"));
			TestTrue(TEXT("Reloaded owner can resolve unknown only with bound evidence"), Reloaded.ResolveUnknownWithEvidence(
				TEXT("operation-0003"), EHyperAIStudioUnknownResolution::RolledBack, RollbackEvidence,
				EHyperAIStudioRollbackState::Unknown, TEXT("rollback_verified"), Record, Error));
		TestEqual(TEXT("Evidence resolver records the new trusted owner"),
			Record.ResolvedByExecutionInstanceId, Reloaded.GetTrustedExecutionInstanceId());
		LastGeneration = Reloaded.GetGeneration();
	}

	const FString NewestPath = (LastGeneration % 2) == 1 ? SlotAPath : SlotBPath;
	TestTrue(TEXT("Test fixture corrupts only the newest slot"), FFileHelper::SaveStringToFile(TEXT("{torn"), *NewestPath));
	{
		FHyperAIStudioOperationJournal Recovered(TestRoot, 16);
		TestTrue(TEXT("A torn newest write falls back to the previous validated generation"), Recovered.Load(Error));
		TestEqual(TEXT("Fallback selects exactly the previous generation"), Recovered.GetGeneration(), LastGeneration - 1);
	}

	const FString CorruptRoot = FPaths::Combine(TestRoot, TEXT("Corrupt"));
	IFileManager::Get().MakeDirectory(*CorruptRoot, true);
	{
		FHyperAIStudioOperationJournal Seed(CorruptRoot, 16);
		TestTrue(TEXT("Corrupt fixture initially loads"), Seed.Load(Error));
		TestEqual(
			TEXT("Corrupt fixture writes one generation"),
			Seed.BeginOperation(TEXT("operation-corrupt"), PlanA, CapabilityA, Record, Error),
			EHyperAIStudioOperationBeginResult::Created);
		TestTrue(TEXT("Fixture slot can be corrupted"), FFileHelper::SaveStringToFile(TEXT("not-json"), *Seed.GetSlotAPath()));
	}
	{
		FHyperAIStudioOperationJournal Corrupt(CorruptRoot, 16);
		TestFalse(TEXT("A corrupt only generation fails Load"), Corrupt.Load(Error));
		TestEqual(
			TEXT("A caller cannot ignore corrupt Load and overwrite evidence"),
			Corrupt.BeginOperation(TEXT("operation-after-corrupt"), PlanA, CapabilityA, Record, Error),
			EHyperAIStudioOperationBeginResult::OwnerUnavailable);
	}

	return true;
}

#endif
