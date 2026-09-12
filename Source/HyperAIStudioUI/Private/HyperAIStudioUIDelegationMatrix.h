// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"

struct FHyperAIStudioUIEpicDelegation
{
	const TCHAR* SourceToolset;
	const TCHAR* CallableName;
	const TCHAR* Lifecycle;
	const TCHAR* Access;
	const TCHAR* DeclarationEvidence;
	const TCHAR* ImplementationEvidence;
	const TCHAR* EffectDomain;
};

class FHyperAIStudioUIEpicDelegationMatrix final
{
public:
	/** Exact access-review-bound matrix: 23 UMG + 9 MVVM + 14 SlateInspector. */
	static TConstArrayView<FHyperAIStudioUIEpicDelegation> Get();
};
