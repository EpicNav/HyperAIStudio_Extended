// Games by Hyper 2026.

#include "HyperAIStudioCapabilityRuntimeIndex.h"

#include "HyperAIStudioAuditToolset.h"
#include "HyperAIStudioBlueprintWorkflowToolset.h"
#include "HyperAIStudioCapabilityPackRegistry.h"
#include "HyperAIStudioContextSearchToolsets.h"
#include "HyperAIStudioDependencyGraphToolset.h"
#include "HyperAIStudioDiagnoseToolset.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioLogTailToolset.h"
#include "HyperAIStudioNativeReadToolset.h"
#include "HyperAIStudioPIEPlaytestToolset.h"
#include "HyperAIStudioPlanExecuteToolset.h"
#include "HyperAIStudioPlanValidateToolset.h"
#include "HyperAIStudioViewportCaptureToolset.h"
#include "Misc/ScopeLock.h"
#include "ToolsetRegistry/Toolset.h"
#include "ToolsetRegistry/ToolsetDefinition.h"
#include "ToolsetRegistry/ToolsetRegistry.h"
#include "ToolsetRegistry/ToolsetRegistrySubsystem.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "UObject/Class.h"
#include "UObject/FieldIterator.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

namespace HyperAIStudio::CapabilityRuntimeIndex::Private
{
	struct FPublishedToolset
	{
		FString QualifiedToolset;
		FString ToolsetClassPath;
		TArray<FHyperAIStudioRuntimeToolBinding> Bindings;
	};

	struct FOptionalPublicationState
	{
		FCriticalSection Mutex;
		TMap<FString, FPublishedToolset> Toolsets;
	};

	enum class EOwnedRegistrationPhase : uint8
	{
		Registering,
		Registered,
		Unregistering
	};

	struct FOwnedToolset
	{
		FString ToolsetClassPath;
		EOwnedRegistrationPhase Phase = EOwnedRegistrationPhase::Registering;
		// Keep identity across filter changes without extending the Epic handler/module lifetime.
		TWeakPtr<UE::ToolsetRegistry::FToolset> RetainedVisibleHandler;
	};

	struct FOwnedRegistrationState
	{
		FCriticalSection Mutex;
		TMap<FString, FOwnedToolset> Toolsets;
	};

	struct FCoreToolsetClass
	{
		UClass* ToolsetClass = nullptr;
	};

	FOptionalPublicationState& OptionalState()
	{
		static FOptionalPublicationState State;
		return State;
	}

	FOwnedRegistrationState& OwnedState()
	{
		static FOwnedRegistrationState State;
		return State;
	}

	void AddDiagnostic(TArray<FString>& Diagnostics, const FString& Value)
	{
		if (Diagnostics.Num() < FHyperAIStudioCapabilityRuntimeIndex::MaxDiagnostics)
		{
			Diagnostics.Add(Value.Left(512));
		}
	}

	const FHyperAIStudioCapabilityToolDefinition* FindUniqueCatalogTool(
		const FHyperAIStudioCapabilityCatalog& Catalog,
		const FString& ToolName)
	{
		const FHyperAIStudioCapabilityToolDefinition* Match = nullptr;
		for (const FHyperAIStudioCapabilityToolDefinition& Tool : Catalog.Tools)
		{
			if (Tool.Name != ToolName)
			{
				continue;
			}
			if (Match)
			{
				return nullptr;
			}
			Match = &Tool;
		}
		return Match;
	}

	FString MakeQualifiedToolsetName(const UClass* ToolsetClass)
	{
		const UObject* ClassOuter = ToolsetClass ? ToolsetClass->GetOuter() : nullptr;
		const FString PackageName = ClassOuter ? ClassOuter->GetName() : FString();
		int32 LastSlash = INDEX_NONE;
		return PackageName.FindLastChar(TEXT('/'), LastSlash)
			? PackageName.RightChop(LastSlash + 1) + TEXT(".") + ToolsetClass->GetName()
			: FString();
	}

	bool BuildBindingsForClass(
		UClass* ToolsetClass,
		const FString& QualifiedToolset,
		const FHyperAIStudioCapabilityCatalog& Catalog,
		TArray<FHyperAIStudioRuntimeToolBinding>& OutBindings,
		FString& OutError)
	{
		OutBindings.Reset();
		OutError.Reset();
		if (!IsInGameThread())
		{
			OutError = TEXT("runtime_index_requires_game_thread_publication");
			return false;
		}
		if (!ToolsetClass || !ToolsetClass->IsChildOf(UToolsetDefinition::StaticClass())
			|| QualifiedToolset.IsEmpty() || QualifiedToolset.Len() > 256)
		{
			OutError = TEXT("invalid_runtime_toolset_identity");
			return false;
		}
		const FString ExpectedQualifiedToolset = MakeQualifiedToolsetName(ToolsetClass);
		if (ExpectedQualifiedToolset.IsEmpty())
		{
			OutError = TEXT("invalid_runtime_toolset_package");
			return false;
		}
		if (!ExpectedQualifiedToolset.StartsWith(TEXT("HyperAIStudio"), ESearchCase::CaseSensitive)
			|| QualifiedToolset != ExpectedQualifiedToolset)
		{
			OutError = TEXT("runtime_toolset_qualified_name_mismatch");
			return false;
		}

		TSet<FString> SeenNames;
		for (TFieldIterator<UFunction> FunctionIt(ToolsetClass, EFieldIterationFlags::None);
			FunctionIt; ++FunctionIt)
		{
			const TObjectPtr<const UFunction> Function = *FunctionIt;
			if (!Function || !Function->HasMetaData(TEXT("AICallable")))
			{
				continue;
			}
			const FString ToolName = Function->GetName();
			const TValueOrError<bool, FString> EpicCallable =
				UToolsetDefinition::IsFunctionAICallable(Function);
			if (!EpicCallable.HasValue() || !EpicCallable.GetValue())
			{
				OutBindings.Reset();
				OutError = TEXT("runtime_ai_callable_rejected_by_epic:") + ToolName;
				return false;
			}
			const FHyperAIStudioCapabilityToolDefinition* CatalogTool =
				FindUniqueCatalogTool(Catalog, ToolName);
			if (!CatalogTool
				|| CatalogTool->AdmissionState == EHyperAIStudioCapabilityAdmissionState::Planned
				|| CatalogTool->SourceArtifactCount <= 0
				|| SeenNames.Contains(ToolName))
			{
				OutBindings.Reset();
				OutError = TEXT("runtime_tool_not_authorized_by_generated_catalog:") + ToolName;
				return false;
			}
			SeenNames.Add(ToolName);
			FHyperAIStudioRuntimeToolBinding& Binding = OutBindings.AddDefaulted_GetRef();
			Binding.ToolName = ToolName;
			Binding.QualifiedToolset = QualifiedToolset;
			Binding.Description = Function->GetMetaData(TEXT("ToolTip")).Left(256);
			Binding.bImplementationLoaded = true;
		}
		if (OutBindings.IsEmpty())
		{
			OutError = TEXT("runtime_toolset_has_no_ai_callable_contracts");
			return false;
		}
		OutBindings.Sort([](
			const FHyperAIStudioRuntimeToolBinding& A,
			const FHyperAIStudioRuntimeToolBinding& B)
		{
			return A.ToolName < B.ToolName;
		});
		return true;
	}

	TArray<FCoreToolsetClass> CoreToolsetClasses()
	{
		return {
			{ UHyperAIStudioPlanValidateToolset::StaticClass() },
			{ UHyperAIStudioPlanExecuteToolset::StaticClass() },
			{ UHyperAIStudioNativeReadToolset::StaticClass() },
			{ UHyperAIStudioDependencyGraphToolset::StaticClass() },
			{ UHyperAIStudioBatchQueryToolset::StaticClass() },
			{ UHyperAIStudioContextSnapshotToolset::StaticClass() },
			{ UHyperAIStudioSceneInspectToolset::StaticClass() },
			{ UHyperAIStudioProjectSearchToolset::StaticClass() },
			{ UHyperAIStudioProjectIndexStatusToolset::StaticClass() },
			{ UHyperAIStudioBlueprintWorkflowToolset::StaticClass() },
			{ UHyperAIStudioDiagnoseToolset::StaticClass() },
			{ UHyperAIStudioAuditToolset::StaticClass() },
			{ UHyperAIStudioLogTailToolset::StaticClass() },
			{ UHyperAIStudioViewportCaptureToolset::StaticClass() },
			{ UHyperAIStudioPIEPlaytestToolset::StaticClass() }
		};
	}

	bool AddBindingsWithoutDuplicates(
		const TArray<FHyperAIStudioRuntimeToolBinding>& Source,
		TMap<FString, FHyperAIStudioRuntimeToolBinding>& BindingsByName,
		TArray<FString>& Diagnostics)
	{
		for (const FHyperAIStudioRuntimeToolBinding& Binding : Source)
		{
			if (BindingsByName.Contains(Binding.ToolName))
			{
				AddDiagnostic(Diagnostics, TEXT("duplicate_runtime_binding:") + Binding.ToolName);
				return false;
			}
			if (BindingsByName.Num() >= FHyperAIStudioCapabilityRuntimeIndex::MaxRuntimeToolBindings)
			{
				AddDiagnostic(Diagnostics, TEXT("runtime_binding_limit_exceeded"));
				return false;
			}
			BindingsByName.Add(Binding.ToolName, Binding);
		}
		return true;
	}

	bool ValidateOwnedToolsetIdentity(
		UClass* ToolsetClass,
		const FString& QualifiedToolset,
		FString& OutError)
	{
		if (!IsInGameThread() || !UObjectInitialized())
		{
			OutError = TEXT("owned_toolset_requires_initialized_game_thread");
			return false;
		}
		TArray<FString> CatalogErrors;
		if (!FHyperAIStudioCapabilityPackRegistry::ValidateBuiltInCatalog(CatalogErrors))
		{
			OutError = TEXT("generated_capability_catalog_invalid");
			return false;
		}
		TArray<FHyperAIStudioRuntimeToolBinding> Bindings;
		return BuildBindingsForClass(
			ToolsetClass,
			QualifiedToolset,
			FHyperAIStudioCapabilityPackRegistry::GetCatalog(),
			Bindings,
			OutError);
	}

	TMap<FString, FString> SnapshotOwnedToolsets()
	{
		TMap<FString, FString> Result;
		FOwnedRegistrationState& State = OwnedState();
		FScopeLock Lock(&State.Mutex);
		for (const TPair<FString, FOwnedToolset>& Pair : State.Toolsets)
		{
			if (Pair.Value.Phase == EOwnedRegistrationPhase::Registered)
			{
				Result.Add(Pair.Key, Pair.Value.ToolsetClassPath);
			}
		}
		return Result;
	}
}

bool FHyperAIStudioCapabilityRuntimeIndex::RegisterOwnedToolsetClass(
	UClass* ToolsetClass,
	const FString& QualifiedToolset,
	FString& OutError)
{
	using namespace HyperAIStudio::CapabilityRuntimeIndex::Private;
	OutError.Reset();
	if (!FHyperAIStudioExtensionRuntime::AreExtendedHyperToolsEnabled())
	{
		OutError = TEXT("hyperai_tools_disabled_by_product_mode");
		return false;
	}
	if (!ValidateOwnedToolsetIdentity(ToolsetClass, QualifiedToolset, OutError))
	{
		return false;
	}
	if (!UToolsetRegistry::IsAvailable())
	{
		OutError = TEXT("toolset_registry_unavailable");
		return false;
	}
	auto RegistrySubsystem = UToolsetRegistrySubsystem::Get();
	if (RegistrySubsystem.HasError())
	{
		OutError = TEXT("toolset_registry_subsystem_unavailable");
		return false;
	}

	const FString ToolsetClassPath = ToolsetClass->GetPathName();
	FOwnedRegistrationState& State = OwnedState();
	UE::ToolsetRegistry::FToolsetRegistry& Registry =
		RegistrySubsystem.GetValue()->ToolsetRegistry;
	{
		FScopeLock Lock(&State.Mutex);
		if (const FOwnedToolset* Existing = State.Toolsets.Find(QualifiedToolset))
		{
			if (Existing->ToolsetClassPath == ToolsetClassPath
				&& Existing->Phase == EOwnedRegistrationPhase::Registered)
			{
				return true;
			}
			OutError = Existing->ToolsetClassPath == ToolsetClassPath
				? TEXT("owned_toolset_registration_in_progress")
				: TEXT("owned_toolset_identity_conflict");
			return false;
		}
	}

	// UE 5.8 exposes no public factory/return value for its private function-library handler,
	// while Find/ForEach hide a handler disabled at registration time. Never create an owned
	// registration that cannot later be identified and removed. Existing owned registrations
	// remain idempotent above and may safely become filtered after their handler was captured.
	if (!Registry.GetBlockedNames().IsEmpty() || !Registry.GetAllowedNames().IsEmpty())
	{
		OutError = TEXT("owned_registration_requires_unfiltered_registry");
		return false;
	}
	if (Registry.Find(QualifiedToolset).IsValid())
	{
		OutError = TEXT("visible_toolset_already_registered_without_owner");
		return false;
	}
	{
		FScopeLock Lock(&State.Mutex);
		if (State.Toolsets.Contains(QualifiedToolset))
		{
			OutError = TEXT("owned_toolset_registration_in_progress");
			return false;
		}
		if (State.Toolsets.Num() >= FHyperAIStudioCapabilityRuntimeIndex::MaxRuntimeToolBindings)
		{
			OutError = TEXT("owned_toolset_limit_exceeded");
			return false;
		}
		FOwnedToolset Reservation;
		Reservation.ToolsetClassPath = ToolsetClassPath;
		Reservation.Phase = EOwnedRegistrationPhase::Registering;
		State.Toolsets.Add(QualifiedToolset, MoveTemp(Reservation));
	}

	bool bRegistryChanged = false;
	bool bObservedHandlerMismatch = false;
	TSharedPtr<UE::ToolsetRegistry::FToolset> VisibleHandler;
	const FDelegateHandle ProbeHandle = Registry.OnToolsetRegistered().AddLambda(
		[&Registry, &QualifiedToolset, ToolsetClass, &bRegistryChanged,
			&bObservedHandlerMismatch, &VisibleHandler]()
		{
			bRegistryChanged = true;
			if (!VisibleHandler.IsValid())
			{
				const TSharedPtr<UE::ToolsetRegistry::FToolset> Candidate =
					Registry.Find(QualifiedToolset);
				if (Candidate.IsValid())
				{
					bObservedHandlerMismatch |= Candidate->GetToolsetClass() != ToolsetClass;
					if (!bObservedHandlerMismatch)
					{
						VisibleHandler = Candidate;
					}
				}
			}
		});
	UToolsetRegistry::RegisterToolsetClass(ToolsetClass);
	Registry.OnToolsetRegistered().Remove(ProbeHandle);

	if (bRegistryChanged && !VisibleHandler.IsValid())
	{
		VisibleHandler = Registry.Find(QualifiedToolset);
	}
	const bool bHandlerMatches = !bObservedHandlerMismatch
		&& VisibleHandler.IsValid()
		&& VisibleHandler->GetToolsetClass() == ToolsetClass;
	if (!bRegistryChanged || !bHandlerMatches)
	{
		if (bRegistryChanged && VisibleHandler.IsValid()
			&& VisibleHandler->GetToolsetClass() == ToolsetClass)
		{
			Registry.UnregisterToolset(VisibleHandler);
		}
		{
			FScopeLock Lock(&State.Mutex);
			const FOwnedToolset* Reservation = State.Toolsets.Find(QualifiedToolset);
			if (Reservation && Reservation->ToolsetClassPath == ToolsetClassPath
				&& Reservation->Phase == EOwnedRegistrationPhase::Registering)
			{
				State.Toolsets.Remove(QualifiedToolset);
			}
		}
		OutError = bRegistryChanged
			? TEXT("owned_toolset_registered_handler_mismatch")
			: TEXT("owned_toolset_registration_rejected");
		return false;
	}

	{
		FScopeLock Lock(&State.Mutex);
		FOwnedToolset* Registration = State.Toolsets.Find(QualifiedToolset);
		if (!Registration || Registration->ToolsetClassPath != ToolsetClassPath
			|| Registration->Phase != EOwnedRegistrationPhase::Registering)
		{
			OutError = TEXT("owned_toolset_registration_state_lost");
		}
		else
		{
			Registration->Phase = EOwnedRegistrationPhase::Registered;
			Registration->RetainedVisibleHandler = VisibleHandler;
		}
	}
	if (!OutError.IsEmpty())
	{
		if (VisibleHandler.IsValid())
		{
			Registry.UnregisterToolset(VisibleHandler);
		}
		return false;
	}
	return true;
}

bool FHyperAIStudioCapabilityRuntimeIndex::UnregisterOwned(
	UClass* ToolsetClass,
	const FString& QualifiedToolset,
	FString& OutError)
{
	using namespace HyperAIStudio::CapabilityRuntimeIndex::Private;
	OutError.Reset();
	if (!IsInGameThread() || !UObjectInitialized() || !ToolsetClass
		|| MakeQualifiedToolsetName(ToolsetClass) != QualifiedToolset)
	{
		OutError = TEXT("invalid_owned_toolset_unregistration_context");
		return false;
	}
	if (!UToolsetRegistry::IsAvailable())
	{
		OutError = TEXT("toolset_registry_unavailable");
		return false;
	}
	auto RegistrySubsystem = UToolsetRegistrySubsystem::Get();
	if (RegistrySubsystem.HasError())
	{
		OutError = TEXT("toolset_registry_subsystem_unavailable");
		return false;
	}

	const FString ToolsetClassPath = ToolsetClass->GetPathName();
	FOwnedRegistrationState& State = OwnedState();
	TSharedPtr<UE::ToolsetRegistry::FToolset> Handler;
	{
		FScopeLock Lock(&State.Mutex);
		FOwnedToolset* Registration = State.Toolsets.Find(QualifiedToolset);
		if (!Registration || Registration->ToolsetClassPath != ToolsetClassPath)
		{
			OutError = TEXT("owned_toolset_not_owned");
			return false;
		}
		if (Registration->Phase != EOwnedRegistrationPhase::Registered)
		{
			OutError = TEXT("owned_toolset_transition_in_progress");
			return false;
		}
		Registration->Phase = EOwnedRegistrationPhase::Unregistering;
		Handler = Registration->RetainedVisibleHandler.Pin();
	}

	UE::ToolsetRegistry::FToolsetRegistry& Registry =
		RegistrySubsystem.GetValue()->ToolsetRegistry;
	if (!Handler.IsValid())
	{
		Handler = Registry.Find(QualifiedToolset);
	}
	const bool bHandlerMatches = Handler.IsValid()
		&& Handler->GetToolsetClass() == ToolsetClass;
	const bool bUnregistered = bHandlerMatches && Registry.UnregisterToolset(Handler);
	{
		FScopeLock Lock(&State.Mutex);
		FOwnedToolset* Registration = State.Toolsets.Find(QualifiedToolset);
		if (Registration && Registration->ToolsetClassPath == ToolsetClassPath
			&& Registration->Phase == EOwnedRegistrationPhase::Unregistering)
		{
			if (bUnregistered)
			{
				// Handler remains locally pinned until after the owner-state lock is released.
				State.Toolsets.Remove(QualifiedToolset);
			}
			else
			{
				Registration->Phase = EOwnedRegistrationPhase::Registered;
			}
		}
	}
	if (!bUnregistered)
	{
		OutError = Handler.IsValid()
			? TEXT("owned_toolset_handler_mismatch_or_unregistration_rejected")
			: TEXT("owned_toolset_hidden_unregistration_unavailable");
		return false;
	}
	return true;
}

bool FHyperAIStudioCapabilityRuntimeIndex::IsOwned(
	UClass* ToolsetClass,
	const FString& QualifiedToolset)
{
	using namespace HyperAIStudio::CapabilityRuntimeIndex::Private;
	if (!IsInGameThread() || !UObjectInitialized() || !ToolsetClass
		|| MakeQualifiedToolsetName(ToolsetClass) != QualifiedToolset)
	{
		return false;
	}
	const FString ToolsetClassPath = ToolsetClass->GetPathName();
	FOwnedRegistrationState& State = OwnedState();
	FScopeLock Lock(&State.Mutex);
	const FOwnedToolset* Registration = State.Toolsets.Find(QualifiedToolset);
	return Registration && Registration->ToolsetClassPath == ToolsetClassPath
		&& Registration->Phase == EOwnedRegistrationPhase::Registered;
}

bool FHyperAIStudioCapabilityRuntimeIndex::PublishOptionalToolset(
	UClass* ToolsetClass,
	const FString& QualifiedToolset,
	FString& OutError)
{
	using namespace HyperAIStudio::CapabilityRuntimeIndex::Private;
	if (!FHyperAIStudioExtensionRuntime::AreExtendedHyperToolsEnabled())
	{
		OutError = TEXT("hyperai_tools_disabled_by_product_mode");
		return false;
	}
	TArray<FString> CatalogErrors;
	if (!FHyperAIStudioCapabilityPackRegistry::ValidateBuiltInCatalog(CatalogErrors))
	{
		OutError = TEXT("generated_capability_catalog_invalid");
		return false;
	}
	TArray<FHyperAIStudioRuntimeToolBinding> Bindings;
	if (!BuildBindingsForClass(
		ToolsetClass,
		QualifiedToolset,
		FHyperAIStudioCapabilityPackRegistry::GetCatalog(),
		Bindings,
		OutError))
	{
		return false;
	}
	if (QualifiedToolset.StartsWith(TEXT("HyperAIStudio."), ESearchCase::CaseSensitive))
	{
		OutError = TEXT("optional_runtime_toolset_must_use_separate_module");
		return false;
	}
	FOptionalPublicationState& State = OptionalState();
	FScopeLock Lock(&State.Mutex);
	if (const FPublishedToolset* Existing = State.Toolsets.Find(QualifiedToolset))
	{
		if (Existing->ToolsetClassPath != ToolsetClass->GetPathName()
			|| Existing->Bindings.Num() != Bindings.Num())
		{
			OutError = TEXT("optional_runtime_toolset_publication_conflict");
			return false;
		}
		for (int32 Index = 0; Index < Bindings.Num(); ++Index)
		{
			if (Existing->Bindings[Index].ToolName != Bindings[Index].ToolName)
			{
				OutError = TEXT("optional_runtime_toolset_publication_conflict");
				return false;
			}
		}
		return true;
	}
	int32 ExistingBindingCount = 0;
	for (const TPair<FString, FPublishedToolset>& PublishedPair : State.Toolsets)
	{
		ExistingBindingCount += PublishedPair.Value.Bindings.Num();
		for (const FHyperAIStudioRuntimeToolBinding& NewBinding : Bindings)
		{
			if (PublishedPair.Value.Bindings.ContainsByPredicate(
				[&NewBinding](const FHyperAIStudioRuntimeToolBinding& ExistingBinding)
				{
					return ExistingBinding.ToolName == NewBinding.ToolName;
				}))
			{
				OutError = TEXT("optional_runtime_tool_name_conflict:") + NewBinding.ToolName;
				return false;
			}
		}
	}
	if (ExistingBindingCount + Bindings.Num() > MaxRuntimeToolBindings)
	{
		OutError = TEXT("optional_runtime_binding_limit_exceeded");
		return false;
	}
	FPublishedToolset Published;
	Published.QualifiedToolset = QualifiedToolset;
	Published.ToolsetClassPath = ToolsetClass->GetPathName();
	Published.Bindings = MoveTemp(Bindings);
	State.Toolsets.Add(QualifiedToolset, MoveTemp(Published));
	return true;
}

void FHyperAIStudioCapabilityRuntimeIndex::WithdrawOptionalToolset(
	UClass* ToolsetClass,
	const FString& QualifiedToolset)
{
	using namespace HyperAIStudio::CapabilityRuntimeIndex::Private;
	FOptionalPublicationState& State = OptionalState();
	FScopeLock Lock(&State.Mutex);
	const FPublishedToolset* Existing = State.Toolsets.Find(QualifiedToolset);
	if (ToolsetClass && Existing
		&& Existing->ToolsetClassPath == ToolsetClass->GetPathName()
		&& MakeQualifiedToolsetName(ToolsetClass) == QualifiedToolset)
	{
		State.Toolsets.Remove(QualifiedToolset);
	}
}

bool FHyperAIStudioCapabilityRuntimeIndex::BuildSnapshot(
	TArray<FHyperAIStudioRuntimeToolBinding>& OutBindings,
	TArray<FString>& OutDiagnostics)
{
	using namespace HyperAIStudio::CapabilityRuntimeIndex::Private;
	OutBindings.Reset();
	OutDiagnostics.Reset();
	if (!IsInGameThread() || !UObjectInitialized())
	{
		AddDiagnostic(OutDiagnostics, TEXT("runtime_index_requires_initialized_game_thread"));
		return false;
	}
	TArray<FString> CatalogErrors;
	if (!FHyperAIStudioCapabilityPackRegistry::ValidateBuiltInCatalog(CatalogErrors))
	{
		AddDiagnostic(OutDiagnostics, TEXT("generated_capability_catalog_invalid"));
		return false;
	}
	const FHyperAIStudioCapabilityCatalog& Catalog =
		FHyperAIStudioCapabilityPackRegistry::GetCatalog();

	TMap<FString, FHyperAIStudioRuntimeToolBinding> BindingsByName;
	TMap<FString, FString> ExpectedToolsetClassPaths;
	for (const FCoreToolsetClass& Core : CoreToolsetClasses())
	{
		const FString QualifiedToolset = MakeQualifiedToolsetName(Core.ToolsetClass);
		ExpectedToolsetClassPaths.Add(
			QualifiedToolset,
			Core.ToolsetClass ? Core.ToolsetClass->GetPathName() : FString());
		TArray<FHyperAIStudioRuntimeToolBinding> ClassBindings;
		FString Error;
		if (!BuildBindingsForClass(
			Core.ToolsetClass,
			QualifiedToolset,
			Catalog,
			ClassBindings,
			Error))
		{
			AddDiagnostic(OutDiagnostics, Error);
			continue;
		}
		AddBindingsWithoutDuplicates(ClassBindings, BindingsByName, OutDiagnostics);
	}

	TArray<FPublishedToolset> OptionalCopies;
	{
		FOptionalPublicationState& State = OptionalState();
		FScopeLock Lock(&State.Mutex);
		State.Toolsets.GenerateValueArray(OptionalCopies);
	}
	OptionalCopies.Sort([](const FPublishedToolset& A, const FPublishedToolset& B)
	{
		return A.QualifiedToolset < B.QualifiedToolset;
	});
	for (const FPublishedToolset& Optional : OptionalCopies)
	{
		ExpectedToolsetClassPaths.Add(Optional.QualifiedToolset, Optional.ToolsetClassPath);
		AddBindingsWithoutDuplicates(Optional.Bindings, BindingsByName, OutDiagnostics);
	}

	BindingsByName.GenerateValueArray(OutBindings);
	OutBindings.Sort([](
		const FHyperAIStudioRuntimeToolBinding& A,
		const FHyperAIStudioRuntimeToolBinding& B)
	{
		return A.ToolName < B.ToolName;
	});
	const bool bRegistryAvailable = UToolsetRegistry::IsAvailable();
	struct FObservedToolset
	{
		bool bPresent = false;
		bool bEnabled = false;
		FString ToolsetClassPath;
		TSet<FString> ToolNames;
		TSet<FString> EnabledToolNames;
	};
	TSet<FString> DesiredToolsets;
	for (const FHyperAIStudioRuntimeToolBinding& Binding : OutBindings)
	{
		DesiredToolsets.Add(Binding.QualifiedToolset);
	}
	TMap<FString, FObservedToolset> ObservedToolsets;
	if (bRegistryAvailable)
	{
		auto RegistrySubsystem = UToolsetRegistrySubsystem::Get();
		if (RegistrySubsystem.HasError())
		{
			AddDiagnostic(OutDiagnostics, TEXT("toolset_registry_subsystem_unavailable"));
		}
		else
		{
			RegistrySubsystem.GetValue()->ToolsetRegistry.ForEachToolset(
				[&](const FString& Name, const UE::ToolsetRegistry::FToolset& Handler)
			{
				if (!DesiredToolsets.Contains(Name))
				{
					return;
				}
				FObservedToolset& Observed = ObservedToolsets.Add(Name);
				Observed.bPresent = true;
				Observed.bEnabled = Handler.IsEnabled();
				Observed.ToolsetClassPath = Handler.GetToolsetClass()
					? Handler.GetToolsetClass()->GetPathName()
					: FString();
				for (const FString& ToolName : Handler.ListToolNames())
				{
					Observed.ToolNames.Add(ToolName);
					if (Handler.IsToolEnabled(ToolName))
					{
						Observed.EnabledToolNames.Add(ToolName);
					}
				}
			});
		}
	}
	const TMap<FString, FString> OwnedToolsets = SnapshotOwnedToolsets();
	TSet<FString> ReportedIdentityDiagnostics;
	for (FHyperAIStudioRuntimeToolBinding& Binding : OutBindings)
	{
		const FObservedToolset* Observed = ObservedToolsets.Find(Binding.QualifiedToolset);
		const FString* ExpectedClassPath = ExpectedToolsetClassPaths.Find(Binding.QualifiedToolset);
		const FString* OwnedClassPath = OwnedToolsets.Find(Binding.QualifiedToolset);
		const bool bOwnedIdentityMatches = ExpectedClassPath && OwnedClassPath
			&& *OwnedClassPath == *ExpectedClassPath;
		const bool bObservedIdentityMatches = ExpectedClassPath && Observed && Observed->bPresent
			&& Observed->ToolsetClassPath == *ExpectedClassPath;
		Binding.bToolsetRegistered = bRegistryAvailable && bOwnedIdentityMatches;
		if (OwnedClassPath && !bOwnedIdentityMatches
			&& !ReportedIdentityDiagnostics.Contains(Binding.QualifiedToolset))
		{
			AddDiagnostic(OutDiagnostics,
				TEXT("owned_toolset_identity_mismatch:") + Binding.QualifiedToolset);
			ReportedIdentityDiagnostics.Add(Binding.QualifiedToolset);
		}
		if (Observed && Observed->bPresent && !bOwnedIdentityMatches
			&& !ReportedIdentityDiagnostics.Contains(Binding.QualifiedToolset))
		{
			AddDiagnostic(OutDiagnostics,
				TEXT("visible_runtime_toolset_not_owned:") + Binding.QualifiedToolset);
			ReportedIdentityDiagnostics.Add(Binding.QualifiedToolset);
		}
		if (bOwnedIdentityMatches && Observed && Observed->bPresent
			&& !bObservedIdentityMatches
			&& !ReportedIdentityDiagnostics.Contains(Binding.QualifiedToolset))
		{
			AddDiagnostic(OutDiagnostics,
				TEXT("owned_toolset_visible_handler_mismatch:") + Binding.QualifiedToolset);
			ReportedIdentityDiagnostics.Add(Binding.QualifiedToolset);
		}
		const FString FullToolName = Binding.QualifiedToolset + TEXT(".") + Binding.ToolName;
		Binding.bToolEnabled = Binding.bToolsetRegistered && bObservedIdentityMatches
			&& Observed->bEnabled
			&& Observed->ToolNames.Contains(FullToolName)
			&& Observed->EnabledToolNames.Contains(FullToolName);
	}
	return OutDiagnostics.IsEmpty();
}
