// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"

class FSlateStyleSet;
class ISlateStyle;

/**
 * Plugin style set, parented to FAppStyle: names it does not define fall through to the editor style,
 * so it only holds what the editor style lacks. Colours come from FStyleColors and follow the editor theme.
 */
class FHyperAIStudioStyle
{
public:
	/** Call before registering any tab spawner that references the style. */
	static void Register();

	/** Call after unregistering those spawners: they hold raw brush pointers into this set. */
	static void Unregister();

	static const ISlateStyle& Get();
	static FName GetStyleSetName();

private:
	static TSharedPtr<FSlateStyleSet> Instance;
};
