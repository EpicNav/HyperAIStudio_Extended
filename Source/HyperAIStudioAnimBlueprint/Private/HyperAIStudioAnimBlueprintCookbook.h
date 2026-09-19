// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"

/** Animation knowledge agents read before building an Animation Blueprint, kept in the project's .hyperai/docs. */
namespace HyperAIStudio::AnimBlueprint
{
	FString GetCookbook();
	FString GetCookbookPath();
	/** Writes the cookbook when it is missing or out of date. */
	bool EnsureCookbook();
}
