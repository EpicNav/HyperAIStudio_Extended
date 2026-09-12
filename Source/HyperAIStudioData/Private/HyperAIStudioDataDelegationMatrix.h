// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"

struct FHyperAIStudioDataEpicDelegation
{
	const TCHAR* SourceToolset;
	const TCHAR* CallableName;
	const TCHAR* Lifecycle;
	const TCHAR* Access;
	const TCHAR* DeclarationEvidence;
	const TCHAR* ImplementationEvidence;
	const TCHAR* EffectDomain;
};

struct FHyperAIStudioDataRequirementDisposition
{
	const TCHAR* Source;
	const TCHAR* SourceId;
	const TCHAR* Lifecycle;
	const TCHAR* Access;
	const TCHAR* LedgerDisposition;
	const TCHAR* FrozenCoverage;
};

class FHyperAIStudioDataDelegationMatrix final
{
public:
	/** Exact UE 5.8 review slice: 35 Python + 21 native callables. */
	static TConstArrayView<FHyperAIStudioDataEpicDelegation> GetEpic();

	/** Product-owned capability requirements that complement the platform delegate matrix. */
	static TConstArrayView<FHyperAIStudioDataRequirementDisposition> GetRequirements();
};
