// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "Animators/PropertyAnimatorCoreBase.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "GameFramework/Actor.h"
#include "Properties/PropertyAnimatorCoreContext.h"
#include "Properties/PropertyAnimatorCoreData.h"
#include "Subsystems/PropertyAnimatorCoreSubsystem.h"

#include <type_traits>

namespace HyperAIStudio::PropertyAnimation::UE58ApiAudit
{
	using FGetExisting = TSet<UPropertyAnimatorCoreBase*>
		(UPropertyAnimatorCoreSubsystem::*)(const AActor*) const;
	using FIsRegistered = bool (UPropertyAnimatorCoreSubsystem::*)(const UClass*) const;
	using FGetContexts = TConstArrayView<UPropertyAnimatorCoreContext*>
		(UPropertyAnimatorCoreBase::*)() const;
	using FGetAnimatedProperty = const FPropertyAnimatorCoreData&
		(UPropertyAnimatorCoreContext::*)() const;
	using FGetLocator = FString (FPropertyAnimatorCoreData::*)() const;
	using FTryExactPackage = UE::AssetRegistry::EExists (IAssetRegistry::*)(
		FName, FAssetPackageData&, bool) const;

	static_assert(std::is_same_v<decltype(static_cast<FGetExisting>(
		&UPropertyAnimatorCoreSubsystem::GetExistingAnimators)), FGetExisting>);
	static_assert(std::is_same_v<decltype(static_cast<FIsRegistered>(
		&UPropertyAnimatorCoreSubsystem::IsAnimatorClassRegistered)), FIsRegistered>);
	static_assert(std::is_same_v<decltype(static_cast<FGetContexts>(
		&UPropertyAnimatorCoreBase::GetLinkedPropertiesContext)), FGetContexts>);
	static_assert(std::is_same_v<decltype(static_cast<FGetAnimatedProperty>(
		&UPropertyAnimatorCoreContext::GetAnimatedProperty)), FGetAnimatedProperty>);
	static_assert(std::is_same_v<decltype(static_cast<FGetLocator>(
		&FPropertyAnimatorCoreData::GetLocatorPath)), FGetLocator>);
	static_assert(std::is_same_v<decltype(static_cast<FTryExactPackage>(
		&IAssetRegistry::TryGetAssetPackageData)), FTryExactPackage>);
}

#endif
