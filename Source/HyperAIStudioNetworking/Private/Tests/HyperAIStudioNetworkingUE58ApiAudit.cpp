// Games by Hyper 2026.

/**
 * Compile-time UE 5.8 audit for the exact public, no-load APIs used by this pack.
 * No OnlineSubsystem provider module, socket API, config/file API, or dynamic invocation
 * header is included here: those effects are deliberately outside the module contract.
 */

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Components/ActorComponent.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/NetDriver.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/GameSession.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/WorldSettings.h"
#include "GameMapsSettings.h"
#include "K2Node_CustomEvent.h"
#include "Misc/AutomationTest.h"
#include "Net/OnlineEngineInterface.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS

static_assert(TIsDerivedFrom<AGameModeBase, AActor>::Value,
	"UE 5.8 audit: GameMode defaults remain actor class defaults.");
static_assert(TIsDerivedFrom<AGameStateBase, AActor>::Value,
	"UE 5.8 audit: GameState remains a replicated actor role.");
static_assert(TIsDerivedFrom<APlayerController, AActor>::Value,
	"UE 5.8 audit: PlayerController remains a replicated actor role.");
static_assert(TIsDerivedFrom<APlayerState, AActor>::Value,
	"UE 5.8 audit: PlayerState remains a replicated actor role.");
static_assert(TIsDerivedFrom<APawn, AActor>::Value,
	"UE 5.8 audit: Pawn remains a replicated actor role.");
static_assert(TIsDerivedFrom<UGameInstance, UObject>::Value,
	"UE 5.8 audit: GameInstance remains a non-actor Framework role.");

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioNetworkingUE58ApiAuditTest,
	"HyperAIStudio.NativeTools.NetworkingGameFramework.UE58ApiAudit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioNetworkingUE58ApiAuditTest::RunTest(const FString& Parameters)
{
	bool (AActor::*ActorReplicated)() const = &AActor::GetIsReplicated;
	float (AActor::*UpdateFrequency)() const = &AActor::GetNetUpdateFrequency;
	float (AActor::*MinUpdateFrequency)() const = &AActor::GetMinNetUpdateFrequency;
	bool (UActorComponent::*ComponentReplicated)() const = &UActorComponent::GetIsReplicated;
	ELifetimeCondition (UActorComponent::*ComponentCondition)() const =
		&UActorComponent::GetReplicationCondition;
	ELifetimeCondition (FProperty::*PropertyCondition)() const =
		&FProperty::GetBlueprintReplicationCondition;
	UNetDriver* (UWorld::*WorldNetDriver)() const = &UWorld::GetNetDriver;
	ENetMode (UWorld::*WorldNetMode)() const = &UWorld::GetNetMode;
	FString (*DefaultMap)(EDefaultMapRequestType) = &UGameMapsSettings::GetGameDefaultMap;
	FString (*GlobalGameMode)() = &UGameMapsSettings::GetGlobalDefaultGameMode;
	FName (UOnlineEngineInterface::*DefaultProvider)() const =
		&UOnlineEngineInterface::GetDefaultOnlineSubsystemName;
	bool (UOnlineEngineInterface::*ProviderLoaded)(FName) = &UOnlineEngineInterface::IsLoaded;
	UE::AssetRegistry::EExists (IAssetRegistry::*TryPackageNonBlocking)(
		FName, FAssetPackageData&, bool) const = &IAssetRegistry::TryGetAssetPackageData;
	bool (IAssetRegistry::*RegistryGathering)() const = &IAssetRegistry::IsGathering;
	bool (IAssetRegistry::*RegistryLoading)() const = &IAssetRegistry::IsLoadingAssets;
	IAssetRegistry* (*RegistrySingletonNoLoad)() = &IAssetRegistry::Get;
	UObject* (UClass::*ExistingDefaultOnly)(bool) const = &UClass::GetDefaultObject;
	bool (FAssetData::*AssetLoadedProbe)() const = &FAssetData::IsAssetLoaded;
	FString (FAssetData::*AssetObjectPath)() const = &FAssetData::GetObjectPathString;
	AWorldSettings* (UWorld::*LoadedWorldSettings)(bool, bool) const = &UWorld::GetWorldSettings;
	TSubclassOf<AGameModeBase> AWorldSettings::*WorldGameModeOverride =
		&AWorldSettings::DefaultGameMode;
	const TArray<USCS_Node*>& (USimpleConstructionScript::*DeclaredSCSNodes)() const =
		&USimpleConstructionScript::GetAllNodes;
	bool (UK2Node_CustomEvent::*CustomEventOverride)() const =
		&UK2Node_CustomEvent::IsOverride;
	FProperty* (UFunction::*FunctionReturnProperty)() const = &UFunction::GetReturnProperty;

	TestTrue(TEXT("Actor replication accessor exists"), ActorReplicated != nullptr);
	TestTrue(TEXT("Actor update-frequency accessors exist"),
		UpdateFrequency != nullptr && MinUpdateFrequency != nullptr);
	TestTrue(TEXT("Component replication accessors exist"),
		ComponentReplicated != nullptr && ComponentCondition != nullptr);
	TestTrue(TEXT("Blueprint replication condition accessor exists"),
		PropertyCondition != nullptr);
	TestTrue(TEXT("World network diagnostics exist"),
		WorldNetDriver != nullptr && WorldNetMode != nullptr);
	TestTrue(TEXT("GameMapsSettings public inspection APIs exist"),
		DefaultMap != nullptr && GlobalGameMode != nullptr);
	TestTrue(TEXT("Loaded-only online observation APIs exist"),
		DefaultProvider != nullptr && ProviderLoaded != nullptr);
	TestTrue(TEXT("No-load Asset Registry nonblocking package/completeness APIs exist"),
		TryPackageNonBlocking != nullptr && RegistryGathering != nullptr && RegistryLoading != nullptr
		&& RegistrySingletonNoLoad != nullptr);
	TestTrue(TEXT("Loaded-only CDO and cached asset-data probes exist"),
		ExistingDefaultOnly != nullptr && AssetLoadedProbe != nullptr
		&& AssetObjectPath != nullptr);
	TestTrue(TEXT("WorldSettings CAS uses exact loaded world and override fields"),
		LoadedWorldSettings != nullptr && WorldGameModeOverride != nullptr);
	TestTrue(TEXT("Blueprint component selectors bind to authoritative SCS nodes"),
		DeclaredSCSNodes != nullptr && UK2Node_CustomEvent::StaticClass() != nullptr
		&& UBlueprintGeneratedClass::StaticClass() != nullptr);
	TestTrue(TEXT("RPC/RepNotify declaration signature APIs remain exact"),
		CustomEventOverride != nullptr && FunctionReturnProperty != nullptr);
	return true;
}

#endif
