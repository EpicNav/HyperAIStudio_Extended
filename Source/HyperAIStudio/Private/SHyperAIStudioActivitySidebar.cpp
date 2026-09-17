// Games by Hyper 2026.

#include "SHyperAIStudioActivitySidebar.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "ContentBrowserModule.h"
#include "HyperAIStudioStyle.h"
#include "IContentBrowserSingleton.h"
#include "Modules/ModuleManager.h"
#include "Styling/AppStyle.h"
#include "Styling/StyleColors.h"
#include "UObject/SoftObjectPath.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "SHyperAIStudioChatActivitySidebar"

namespace HyperAIStudio::ActivitySidebar
{
	FText RelativeTime(const FDateTime& Utc)
	{
		const FTimespan Age = FDateTime::UtcNow() - Utc;
		if (Age.GetTotalSeconds() < 60.0)
		{
			return LOCTEXT("JustNow", "just now");
		}
		if (Age.GetTotalMinutes() < 60.0)
		{
			return FText::Format(LOCTEXT("MinutesAgo", "{0}m ago"), FMath::FloorToInt(Age.GetTotalMinutes()));
		}
		return FText::Format(LOCTEXT("HoursAgo", "{0}h ago"), FMath::FloorToInt(Age.GetTotalHours()));
	}

	FText AssetName(const FString& Target)
	{
		FString Name = Target;
		int32 Dot = INDEX_NONE;
		if (Name.FindLastChar(TEXT('.'), Dot))
		{
			Name.LeftInline(Dot);
		}
		int32 Slash = INDEX_NONE;
		return FText::FromString(Name.FindLastChar(TEXT('/'), Slash) ? Name.RightChop(Slash + 1) : Name);
	}

	FSlateColor StatusColor(const EHyperAIStudioActivityKind Kind)
	{
		switch (Kind)
		{
		case EHyperAIStudioActivityKind::Failed: return FStyleColors::AccentRed;
		case EHyperAIStudioActivityKind::ApprovalRequested: return FStyleColors::AccentYellow;
		case EHyperAIStudioActivityKind::Completed:
		case EHyperAIStudioActivityKind::JobFinished: return FStyleColors::AccentGreen;
		default: return FSlateColor::UseSubduedForeground();
		}
	}
}

void SHyperAIStudioChatActivitySidebar::Construct(const FArguments& InArgs)
{
	OnClose = InArgs._OnClose;
	OnApprovalResolved = InArgs._OnApprovalResolved;

	ActivityChangedHandle = FHyperAIStudioAgentActivityLog::OnChanged().AddSP(this, &SHyperAIStudioChatActivitySidebar::Rebuild);
	ApprovalChangedHandle = FHyperAIStudioApprovalGate::OnChanged().AddSP(this, &SHyperAIStudioChatActivitySidebar::Rebuild);
	JobChangedHandle = FHyperAIStudioAsyncJobHost::OnChanged().AddSP(this, &SHyperAIStudioChatActivitySidebar::Rebuild);

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FHyperAIStudioStyle::Get().GetBrush("HyperAIStudio.Panel"))
		.Padding(8.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("Title", "Activity"))
					.TextStyle(FHyperAIStudioStyle::Get(), "HyperAIStudio.Text.Title")
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SButton)
					.ButtonStyle(FAppStyle::Get(), "SimpleButton")
					.ToolTipText(LOCTEXT("ClearTooltip", "Clear the finished entries. Plans waiting for approval stay."))
					.OnClicked_Lambda([]()
					{
						FHyperAIStudioAgentActivityLog::Clear();
						return FReply::Handled();
					})
					[
						SNew(SImage)
						.Image(FAppStyle::GetBrush(TEXT("Icons.Delete")))
						.ColorAndOpacity(FSlateColor::UseForeground())
					]
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SButton)
					.ButtonStyle(FAppStyle::Get(), "SimpleButton")
					.ToolTipText(LOCTEXT("CloseTooltip", "Hide the Activity panel"))
					.OnClicked_Lambda([this]()
					{
						OnClose.ExecuteIfBound();
						return FReply::Handled();
					})
					[
						SNew(SImage)
						.Image(FAppStyle::GetBrush(TEXT("Icons.X")))
						.ColorAndOpacity(FSlateColor::UseForeground())
					]
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 8.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Visibility_Lambda([this]() { return Rows.IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed; })
				.Text(LOCTEXT("Empty", "No agent edits yet. Plans appear here for approval before anything changes."))
			]
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			.Padding(0.0f, 6.0f, 0.0f, 0.0f)
			[
				SAssignNew(List, SListView<FRowPtr>)
				.ListItemsSource(&Rows)
				.SelectionMode(ESelectionMode::None)
				.OnGenerateRow(this, &SHyperAIStudioChatActivitySidebar::GenerateRow)
			]
		]
	];

	Rebuild();
}

SHyperAIStudioChatActivitySidebar::~SHyperAIStudioChatActivitySidebar()
{
	FHyperAIStudioAgentActivityLog::OnChanged().Remove(ActivityChangedHandle);
	FHyperAIStudioApprovalGate::OnChanged().Remove(ApprovalChangedHandle);
	FHyperAIStudioAsyncJobHost::OnChanged().Remove(JobChangedHandle);
}

int32 SHyperAIStudioChatActivitySidebar::GetPendingApprovalCount()
{
	return FHyperAIStudioApprovalGate::GetPending().Num();
}

void SHyperAIStudioChatActivitySidebar::Rebuild()
{
	Rows.Reset();
	auto AddHeading = [this](const FText& Heading)
	{
		FRow Row;
		Row.Heading = Heading;
		Rows.Add(MakeShared<FRow>(MoveTemp(Row)));
	};

	const TArray<FHyperAIStudioPendingApproval> Pending = FHyperAIStudioApprovalGate::GetPending();
	if (!Pending.IsEmpty())
	{
		AddHeading(LOCTEXT("PendingHeading", "Waiting for you"));
		for (const FHyperAIStudioPendingApproval& Approval : Pending)
		{
			FRow Row;
			Row.Approval = MakeShared<FHyperAIStudioPendingApproval>(Approval);
			Rows.Add(MakeShared<FRow>(MoveTemp(Row)));
		}
	}

	TArray<FHyperAIStudioAsyncJobStatus> Jobs = FHyperAIStudioAsyncJobHost::GetSnapshot();
	Jobs.RemoveAll([](const FHyperAIStudioAsyncJobStatus& Job) { return Job.State != EHyperAIStudioAsyncJobState::Running; });
	if (!Jobs.IsEmpty())
	{
		AddHeading(LOCTEXT("RunningHeading", "Running"));
		for (const FHyperAIStudioAsyncJobStatus& Job : Jobs)
		{
			FRow Row;
			Row.Job = MakeShared<FHyperAIStudioAsyncJobStatus>(Job);
			Rows.Add(MakeShared<FRow>(MoveTemp(Row)));
		}
	}

	const TArray<FHyperAIStudioActivityEntry> Entries = FHyperAIStudioAgentActivityLog::GetSnapshot();
	if (!Entries.IsEmpty())
	{
		AddHeading(LOCTEXT("HistoryHeading", "History"));
		for (const FHyperAIStudioActivityEntry& Entry : Entries)
		{
			FRow Row;
			Row.Entry = MakeShared<FHyperAIStudioActivityEntry>(Entry);
			Rows.Add(MakeShared<FRow>(MoveTemp(Row)));
		}
	}

	if (List.IsValid())
	{
		List->RequestListRefresh();
	}
}

TSharedRef<ITableRow> SHyperAIStudioChatActivitySidebar::GenerateRow(FRowPtr Row, const TSharedRef<STableViewBase>& OwnerTable)
{
	TSharedPtr<SWidget> Content;
	if (Row->Approval.IsValid())
	{
		Content = BuildApprovalCard(*Row->Approval);
	}
	else if (Row->Job.IsValid())
	{
		Content = BuildJobRow(*Row->Job);
	}
	else if (Row->Entry.IsValid())
	{
		Content = BuildEntryRow(*Row->Entry);
	}
	else
	{
		Content = SNew(STextBlock)
			.Text(Row->Heading)
			.Font(FAppStyle::GetFontStyle("SmallFontBold"))
			.ColorAndOpacity(FSlateColor::UseSubduedForeground());
	}

	return SNew(STableRow<FRowPtr>, OwnerTable)
		.Style(FAppStyle::Get(), "TableView.NoHoverTableRow")
		.ShowSelection(false)
		.Padding(FMargin(2.0f, Row->Heading.IsEmpty() ? 3.0f : 10.0f, 2.0f, 3.0f))
		[
			Content.ToSharedRef()
		];
}

TSharedRef<SWidget> SHyperAIStudioChatActivitySidebar::BuildApprovalCard(const FHyperAIStudioPendingApproval& Approval)
{
	using namespace HyperAIStudio::ActivitySidebar;
	const FString OperationId = Approval.OperationId;
	const FString Target = Approval.Summary.EffectTarget;

	TSharedRef<SVerticalBox> Effects = SNew(SVerticalBox);
	for (int32 Index = 0; Index < Approval.Summary.Effects.Num() && Index < 8; ++Index)
	{
		Effects->AddSlot()
		.AutoHeight()
		[
			SNew(STextBlock)
			.Text(FText::FromString(TEXT("- ") + Approval.Summary.Effects[Index]))
			.AutoWrapText(true)
			.Font(FAppStyle::GetFontStyle("SmallFont"))
		];
	}
	if (Approval.Summary.Effects.Num() > 8)
	{
		Effects->AddSlot()
		.AutoHeight()
		[
			SNew(STextBlock)
			.Text(FText::Format(LOCTEXT("MoreEffects", "- and {0} more"), Approval.Summary.Effects.Num() - 8))
			.Font(FAppStyle::GetFontStyle("SmallFont"))
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
		];
	}

	return SNew(SBorder)
		.BorderImage(FHyperAIStudioStyle::Get().GetBrush("HyperAIStudio.Badge"))
		.Padding(FMargin(8.0f, 6.0f))
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(STextBlock)
				.Text(FText::FromString(Approval.Summary.ToolName))
				.AutoWrapText(true)
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 2.0f, 0.0f, 4.0f)
			[
				SNew(STextBlock)
				.Text(AssetName(Target))
				.ToolTipText(FText::FromString(Target))
				.Font(FAppStyle::GetFontStyle("SmallFont"))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				Effects
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 6.0f, 0.0f, 0.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("Approve", "Approve"))
					.ToolTipText(LOCTEXT("ApproveTooltip", "Run this plan now."))
					.OnClicked_Lambda([this, OperationId]()
					{
						FString Error;
						FHyperAIStudioApprovalGate::Approve(OperationId, Error);
						OnApprovalResolved.ExecuteIfBound();
						return FReply::Handled();
					})
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(6.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("Reject", "Reject"))
					.ToolTipText(LOCTEXT("RejectTooltip", "Discard this plan. Nothing changes."))
					.OnClicked_Lambda([this, OperationId]()
					{
						FHyperAIStudioApprovalGate::Reject(OperationId, TEXT("Rejected in the Activity panel."));
						OnApprovalResolved.ExecuteIfBound();
						return FReply::Handled();
					})
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.HAlign(HAlign_Right)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(RelativeTime(Approval.RequestedUtc))
					.Font(FAppStyle::GetFontStyle("SmallFont"))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
			]
		];
}

TSharedRef<SWidget> SHyperAIStudioChatActivitySidebar::BuildJobRow(const FHyperAIStudioAsyncJobStatus& Job)
{
	using namespace HyperAIStudio::ActivitySidebar;
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(STextBlock)
			.Text(FText::Format(LOCTEXT("JobLine", "{0} - {1}"), AssetName(Job.Target), FText::FromString(Job.Progress)))
			.ToolTipText(FText::FromString(Job.ToolName))
			.AutoWrapText(true)
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(STextBlock)
			.Text(FText::Format(LOCTEXT("JobElapsed", "running for {0}s"), static_cast<int32>(Job.ElapsedMs / 1000)))
			.Font(FAppStyle::GetFontStyle("SmallFont"))
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
		];
}

TSharedRef<SWidget> SHyperAIStudioChatActivitySidebar::BuildEntryRow(const FHyperAIStudioActivityEntry& Entry)
{
	using namespace HyperAIStudio::ActivitySidebar;
	const FString Target = Entry.Target;
	TSharedRef<SHorizontalBox> Line = SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		[
			SNew(STextBlock)
			.Text(FText::Format(LOCTEXT("EntryLine", "{0}: {1}"),
				FText::FromString(FHyperAIStudioAgentActivityLog::LexToString(Entry.Kind)), FText::FromString(Entry.ToolName)))
			.ToolTipText(FText::FromString(Entry.Detail.IsEmpty() ? Entry.StatusCode : Entry.Detail))
			.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
			.ColorAndOpacity(StatusColor(Entry.Kind))
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(RelativeTime(Entry.Utc))
			.Font(FAppStyle::GetFontStyle("SmallFont"))
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
		];

	if (Target.IsEmpty())
	{
		return Line;
	}
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			Line
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "SimpleButton")
			.HAlign(HAlign_Left)
			.ContentPadding(FMargin(0.0f, 1.0f))
			.ToolTipText(FText::Format(LOCTEXT("ShowAssetTooltip", "Show {0} in the Content Browser"), FText::FromString(Target)))
			.OnClicked_Lambda([Target]()
			{
				ShowTargetInContentBrowser(Target);
				return FReply::Handled();
			})
			[
				SNew(STextBlock)
				.Text(AssetName(Target))
				.Font(FAppStyle::GetFontStyle("SmallFont"))
				.ColorAndOpacity(FStyleColors::AccentBlue)
			]
		];
}

void SHyperAIStudioChatActivitySidebar::ShowTargetInContentBrowser(const FString& Target)
{
	const FSoftObjectPath Path(Target);
	if (Path.IsNull())
	{
		return;
	}
	const FAssetData Asset = IAssetRegistry::GetChecked().GetAssetByObjectPath(Path);
	if (!Asset.IsValid())
	{
		return;
	}
	FContentBrowserModule& ContentBrowser = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
	ContentBrowser.Get().SyncBrowserToAssets({Asset});
}

#undef LOCTEXT_NAMESPACE
