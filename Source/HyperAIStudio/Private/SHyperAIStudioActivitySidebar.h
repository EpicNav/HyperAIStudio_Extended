// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioAgentActivity.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioAsyncJobHost.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

/**
 * What the agent is asking to do, what it is doing, and what it did.
 *
 * Plans wait here until approved, jobs report progress here, and finished work stays listed with the
 * asset it touched. Reads core state only; every mutation goes through the approval gate.
 */
class SHyperAIStudioChatActivitySidebar : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SHyperAIStudioChatActivitySidebar) {}
		SLATE_EVENT(FSimpleDelegate, OnClose)
		/** Notified when an approval is granted or rejected, so the chat panel can report it. */
		SLATE_EVENT(FSimpleDelegate, OnApprovalResolved)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SHyperAIStudioChatActivitySidebar() override;

	static int32 GetPendingApprovalCount();

private:
	struct FRow
	{
		/** Exactly one of these is set. */
		TSharedPtr<FHyperAIStudioPendingApproval> Approval;
		TSharedPtr<FHyperAIStudioAsyncJobStatus> Job;
		TSharedPtr<FHyperAIStudioActivityEntry> Entry;
		TSharedPtr<FHyperAIStudioAgentScore> Score;
		FText Heading;
	};
	using FRowPtr = TSharedPtr<FRow>;

	void Rebuild();
	TSharedRef<ITableRow> GenerateRow(FRowPtr Row, const TSharedRef<STableViewBase>& OwnerTable);
	TSharedRef<SWidget> BuildApprovalCard(const FHyperAIStudioPendingApproval& Approval);
	TSharedRef<SWidget> BuildJobRow(const FHyperAIStudioAsyncJobStatus& Job);
	TSharedRef<SWidget> BuildEntryRow(const FHyperAIStudioActivityEntry& Entry);
	TSharedRef<SWidget> BuildScoreRow(const FHyperAIStudioAgentScore& Score);
	static void ShowTargetInContentBrowser(const FString& Target);

	FSimpleDelegate OnClose;
	FSimpleDelegate OnApprovalResolved;
	TSharedPtr<SListView<FRowPtr>> List;
	TArray<FRowPtr> Rows;
	FDelegateHandle ActivityChangedHandle;
	FDelegateHandle ApprovalChangedHandle;
	FDelegateHandle JobChangedHandle;
};
