// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioAnimBlueprintToolset.h"

class UAnimBlueprint;
class USkeleton;

/** The only code that reads or writes Animation Blueprint graphs; the toolset handles contracts and execution. */
namespace HyperAIStudio::AnimBlueprint::Gate
{
	UAnimBlueprint* Resolve(const FString& Path, bool bLoad);
	USkeleton* ResolveSkeleton(const FString& Path);
	/** Empty path is AnimInstance; otherwise an AnimInstance subclass, or null with OutError. */
	UClass* ResolveParentClass(const FString& Path, FString& OutError);
	bool IsOpenInEditor(const UAnimBlueprint& Blueprint);
	/** up_to_date, warnings, dirty or error. */
	FString CompileStatusOf(const UAnimBlueprint& Blueprint);
	/** Skeleton, parent, variables and every graph's nodes, pins, links and node settings; not node positions. */
	FString ComputeRevision(const UAnimBlueprint& Blueprint);
	void Read(const UAnimBlueprint& Blueprint, TArray<FHyperAIAnimBlueprintVariable>& OutVariables, TArray<FHyperAIAnimBlueprintMachine>& OutMachines);

	/** Checks every op against the graph as it would be at that point; Blueprint is null when the plan creates it. */
	bool ValidateOps(const UAnimBlueprint* Blueprint, const USkeleton* Skeleton, const TArray<FHyperAIAnimBlueprintOp>& Ops, FString& OutError);
	UAnimBlueprint* Create(const FString& Path, USkeleton& Skeleton, UClass& ParentClass, FString& OutError);
	/** Applies ops in order; the caller holds the transaction. */
	bool ApplyOps(UAnimBlueprint& Blueprint, const TArray<FHyperAIAnimBlueprintOp>& Ops, FString& OutError);
	bool Compile(UAnimBlueprint& Blueprint, FString& OutFirstError, int32& OutErrors, int32& OutWarnings);

	/** Structural and quality checks that do not need a compile. */
	TArray<FHyperAIAnimBlueprintIssue> Check(const UAnimBlueprint& Blueprint);
}
