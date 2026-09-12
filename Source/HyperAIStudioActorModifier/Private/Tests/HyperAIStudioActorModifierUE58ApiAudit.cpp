// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/IAssetRegistry.h"
#include "GameFramework/Actor.h"
#include "Modifiers/ActorModifierCoreBase.h"
#include "Modifiers/ActorModifierCoreStack.h"
#include "Subsystems/ActorModifierCoreSubsystem.h"

#include <type_traits>

namespace HyperAIStudio::ActorModifier::UE58ApiAudit
{
	using FGetStack = UActorModifierCoreStack* (UActorModifierCoreSubsystem::*)(
		const AActor*) const;
	using FGetRegisteredClass = TSubclassOf<UActorModifierCoreBase>
		(UActorModifierCoreSubsystem::*)(FName) const;
	using FGetModifiers = TConstArrayView<UActorModifierCoreBase*>
		(UActorModifierCoreStack::*)() const;
	using FGetModifierName = FName (UActorModifierCoreBase::*)() const;
	using FIsModifierEnabled = bool (UActorModifierCoreBase::*)() const;
	using FTryExactPackage = UE::AssetRegistry::EExists (IAssetRegistry::*)(
		FName, FAssetPackageData&, bool) const;

	static_assert(std::is_same_v<decltype(static_cast<FGetStack>(
		&UActorModifierCoreSubsystem::GetActorModifierStack)), FGetStack>);
	static_assert(std::is_same_v<decltype(static_cast<FGetRegisteredClass>(
		&UActorModifierCoreSubsystem::GetRegisteredModifierClass)), FGetRegisteredClass>);
	static_assert(std::is_same_v<decltype(static_cast<FGetModifiers>(
		&UActorModifierCoreStack::GetModifiers)), FGetModifiers>);
	static_assert(std::is_same_v<decltype(static_cast<FGetModifierName>(
		&UActorModifierCoreBase::GetModifierName)), FGetModifierName>);
	static_assert(std::is_same_v<decltype(static_cast<FIsModifierEnabled>(
		&UActorModifierCoreBase::IsModifierEnabled)), FIsModifierEnabled>);
	static_assert(std::is_same_v<decltype(static_cast<FTryExactPackage>(
		&IAssetRegistry::TryGetAssetPackageData)), FTryExactPackage>);
}

#endif
