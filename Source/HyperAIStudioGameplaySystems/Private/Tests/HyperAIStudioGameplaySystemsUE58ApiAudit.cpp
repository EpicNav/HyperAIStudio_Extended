// Games by Hyper 2026.

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "GameFeatureAction.h"
#include "GameFeatureData.h"
#include "Interfaces/IPluginManager.h"
#include "MassEntityConfigAsset.h"
#include "MassEntityTraitBase.h"
#include "WorldConditionSchema.h"

#include <type_traits>
#include <utility>

namespace HyperAIStudio::GameplaySystems::UE58ApiAudit
{
	static_assert(std::is_same_v<decltype(std::declval<const UGameFeatureData&>().GetActions()),
		const TArray<UGameFeatureAction*>&>,
		"UE 5.8 GameFeatureData action view changed");
	static_assert(std::is_same_v<decltype(std::declval<const UMassEntityConfigAsset&>().GetConfig()),
		const FMassEntityConfig&>,
		"UE 5.8 MassEntityConfigAsset config view changed");
	static_assert(std::is_same_v<decltype(std::declval<const FMassEntityConfig&>().GetTraits()),
		TConstArrayView<UMassEntityTraitBase*>>,
		"UE 5.8 Mass entity trait view changed");
	static_assert(std::is_same_v<decltype(std::declval<const UWorldConditionSchema&>().GetContextDataDescs()),
		TConstArrayView<FWorldConditionContextDataDesc>>,
		"UE 5.8 World Condition context view changed");
	static_assert(std::is_same_v<decltype(std::declval<const IPlugin&>().IsEnabled()), bool>,
		"UE 5.8 plugin enabled observation changed");
	static_assert(std::is_same_v<decltype(std::declval<const IPlugin&>().GetLoadedFrom()),
		EPluginLoadedFrom>,
		"UE 5.8 plugin ownership observation changed");
	static_assert(std::is_same_v<decltype(std::declval<const IPlugin&>().GetMountedAssetPath()),
		FString>,
		"UE 5.8 plugin mount observation changed");
	static_assert(std::is_same_v<decltype(std::declval<const IAssetRegistry&>().
		TryGetAssetPackageData(FName(), std::declval<FAssetPackageData&>(), true)),
		UE::AssetRegistry::EExists>,
		"UE 5.8 fail-fast Asset Registry package query changed");
}
