// Games by Hyper 2026.

#include "HyperAIStudioStyle.h"

#include "Brushes/SlateImageBrush.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Interfaces/IPluginManager.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateStyle.h"
#include "Styling/SlateStyleRegistry.h"
#include "Styling/StyleColors.h"

TSharedPtr<FSlateStyleSet> FHyperAIStudioStyle::Instance;

void FHyperAIStudioStyle::Register()
{
	if (Instance.IsValid())
	{
		return;
	}

	TSharedRef<FSlateStyleSet> Style = MakeShared<FSlateStyleSet>(GetStyleSetName());
	Style->SetParentStyleName(FAppStyle::GetAppStyleSetName());
	if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("HyperAIStudio")))
	{
		Style->SetContentRoot(Plugin->GetBaseDir() / TEXT("Resources"));
	}

	FTextBlockStyle Title = FAppStyle::Get().GetWidgetStyle<FTextBlockStyle>("NormalText");
	Title.SetFont(FCoreStyle::GetDefaultFontStyle("Bold", 12));
	Style->Set("HyperAIStudio.Text.Title", Title);

	// A badge is state, not chrome: a rounded outlined pill instead of the flat recessed panel brush.
	Style->Set("HyperAIStudio.Badge", new FSlateRoundedBoxBrush(FStyleColors::Input, 4.0f, FStyleColors::InputOutline, 1.0f));
	Style->Set("HyperAIStudio.Panel", new FSlateRoundedBoxBrush(FStyleColors::Recessed, 4.0f));
	Style->Set("HyperAIStudio.TabIcon", new FSlateVectorImageBrush(Style->RootToContentDir(TEXT("HyperAIStudioTab"), TEXT(".svg")), FVector2f(16.0f, 16.0f)));

	FSlateStyleRegistry::RegisterSlateStyle(*Style);
	Instance = Style;
}

void FHyperAIStudioStyle::Unregister()
{
	if (Instance.IsValid())
	{
		FSlateStyleRegistry::UnRegisterSlateStyle(*Instance);
		Instance.Reset();
	}
}

const ISlateStyle& FHyperAIStudioStyle::Get()
{
	check(Instance.IsValid());
	return *Instance;
}

FName FHyperAIStudioStyle::GetStyleSetName()
{
	static const FName Name(TEXT("HyperAIStudioStyle"));
	return Name;
}
