// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/IAssetRegistry.h"
#include "Components/DMMaterialComponent.h"
#include "Components/DMMaterialLayer.h"
#include "Components/DMMaterialEffectStack.h"
#include "Components/DMMaterialProperty.h"
#include "Components/DMMaterialSlot.h"
#include "Components/DMMaterialStageSource.h"
#include "Components/DMMaterialStageThroughput.h"
#include "Components/DMMaterialStage.h"
#include "Components/MaterialValues/DMMaterialValueBool.h"
#include "Components/MaterialValues/DMMaterialValueFloat.h"
#include "Components/MaterialValues/DMMaterialValueFloat1.h"
#include "Components/MaterialValues/DMMaterialValueFloat2.h"
#include "Components/MaterialValues/DMMaterialValueFloat3RGB.h"
#include "Components/MaterialValues/DMMaterialValueFloat3RPY.h"
#include "Components/MaterialValues/DMMaterialValueFloat3XYZ.h"
#include "Components/MaterialValues/DMMaterialValueFloat4.h"
#include "Model/DynamicMaterialModel.h"
#include "Model/DynamicMaterialModelBase.h"
#include "Model/DynamicMaterialModelEditorOnlyData.h"

#include <type_traits>

namespace HyperAIStudio::DynamicMaterial::UE58ApiAudit
{
	using FGetComponent = UDMMaterialComponent* (UDynamicMaterialModel::*)(const FString&) const;
	using FGetComponentPathComponent = FString (UDMMaterialComponent::*)() const;
	using FGetValueRange = const FFloatInterval& (UDMMaterialValueFloat::*)() const;
	using FGetParentComponent = UDMMaterialComponent* (UDMMaterialComponent::*)() const;
	using FGetValuesRef = const TArray<UDMMaterialValue*>& (UDynamicMaterialModel::*)() const;
	using FGetStagesRef = const TArray<TObjectPtr<UDMMaterialStage>>&
		(UDMMaterialLayerObject::*)() const;
	using FGetSlotsRef = const TArray<UDMMaterialSlot*>&
		(UDynamicMaterialModelEditorOnlyData::*)() const;
	using FGetStageInputsRef = const TArray<UDMMaterialStageInput*>&
		(UDMMaterialStage::*)() const;
	using FGetStageConnectionsRef = const TArray<FDMMaterialStageConnection>&
		(UDMMaterialStage::*)() const;
	using FGetEffectsRef = const TArray<TObjectPtr<UDMMaterialEffect>>&
		(UDMMaterialEffectStack::*)() const;
	using FGetOutputConnectorsRef = const TArray<FDMMaterialStageConnector>&
		(UDMMaterialStageSource::*)() const;
	using FGetInputConnectorsRef = const TArray<FDMMaterialStageConnector>&
		(UDMMaterialStageThroughput::*)() const;
	using FGetEditablePropertiesRef = const TArray<FName>&
		(UDMMaterialComponent::*)() const;
	using FGetPropertyConnectionRef = const FDMMaterialStageConnection&
		(UDMMaterialProperty::*)() const;
	using FGetSlotLayersRef = const TArray<TObjectPtr<UDMMaterialLayerObject>>&
		(UDMMaterialSlot::*)() const;
	using FGetSlotOutputTypesRef = const TArray<EDMValueType>&
		(UDMMaterialSlot::*)(EDMMaterialPropertyType) const;
	using FGetSlotReferencesRef = const TMap<TWeakObjectPtr<UDMMaterialSlot>, int32>&
		(UDMMaterialSlot::*)() const;
	using FGetRuntimeComponentsRef = const TSet<TObjectPtr<UDMMaterialComponent>>&
		(UDynamicMaterialModel::*)() const;
	using FGetMaterialProperty = UDMMaterialProperty*
		(UDynamicMaterialModelEditorOnlyData::*)(EDMMaterialPropertyType) const;
	using FGetSlotForProperty = UDMMaterialSlot*
		(UDynamicMaterialModelEditorOnlyData::*)(EDMMaterialPropertyType) const;
	using FGetDomain = TEnumAsByte<EMaterialDomain>
		(UDynamicMaterialModelEditorOnlyData::*)() const;
	using FGetBlendMode = TEnumAsByte<EBlendMode>
		(UDynamicMaterialModelEditorOnlyData::*)() const;
	using FGetEditorState = EDMState (UDynamicMaterialModelEditorOnlyData::*)() const;
	using FIsPreviewModified = bool (UDynamicMaterialModelBase::*)() const;
	using FTryExactPackage = UE::AssetRegistry::EExists (IAssetRegistry::*)(
		FName, FAssetPackageData&, bool) const;

	static_assert(std::is_same_v<decltype(static_cast<FGetComponent>(&UDynamicMaterialModel::GetComponentByPath)),
		FGetComponent>, "UE 5.8 DynamicMaterial component path signature changed");
	static_assert(std::is_same_v<decltype(static_cast<FGetComponentPathComponent>(
		&UDMMaterialComponent::GetComponentPathComponent)), FGetComponentPathComponent>,
		"UE 5.8 DynamicMaterial bounded component-path-part signature changed");
	static_assert(std::is_same_v<decltype(static_cast<FGetValueRange>(&UDMMaterialValueFloat::GetValueRange)),
		FGetValueRange>, "UE 5.8 DynamicMaterial public setter range signature changed");
	static_assert(std::is_same_v<decltype(static_cast<FGetParentComponent>(
		&UDMMaterialComponent::GetParentComponent)), FGetParentComponent>,
		"UE 5.8 DynamicMaterial parent-component signature changed");
	static_assert(std::is_same_v<decltype(static_cast<FGetValuesRef>(
		&UDynamicMaterialModel::GetValues)), FGetValuesRef>,
		"UE 5.8 DynamicMaterial bounded values reference signature changed");
	static_assert(std::is_same_v<decltype(static_cast<FGetStagesRef>(
		&UDMMaterialLayerObject::GetAllStages)), FGetStagesRef>,
		"UE 5.8 DynamicMaterial bounded stages reference signature changed");
	static_assert(std::is_same_v<decltype(static_cast<FGetSlotsRef>(
		&UDynamicMaterialModelEditorOnlyData::GetSlots)), FGetSlotsRef>,
		"UE 5.8 DynamicMaterial bounded slots reference signature changed");
	static_assert(std::is_same_v<decltype(static_cast<FGetStageInputsRef>(
		&UDMMaterialStage::GetInputs)), FGetStageInputsRef>,
		"UE 5.8 DynamicMaterial bounded stage inputs reference changed");
	static_assert(std::is_same_v<decltype(static_cast<FGetStageConnectionsRef>(
		&UDMMaterialStage::GetInputConnectionMap)), FGetStageConnectionsRef>,
		"UE 5.8 DynamicMaterial bounded stage connection reference changed");
	static_assert(std::is_same_v<decltype(static_cast<FGetEffectsRef>(
		&UDMMaterialEffectStack::GetEffects)), FGetEffectsRef>,
		"UE 5.8 DynamicMaterial bounded effect reference changed");
	static_assert(std::is_same_v<decltype(static_cast<FGetOutputConnectorsRef>(
		&UDMMaterialStageSource::GetOutputConnectors)), FGetOutputConnectorsRef>,
		"UE 5.8 DynamicMaterial bounded output connector reference changed");
	static_assert(std::is_same_v<decltype(static_cast<FGetInputConnectorsRef>(
		&UDMMaterialStageThroughput::GetInputConnectors)), FGetInputConnectorsRef>,
		"UE 5.8 DynamicMaterial bounded input connector reference changed");
	static_assert(std::is_same_v<decltype(static_cast<FGetEditablePropertiesRef>(
		&UDMMaterialComponent::GetEditableProperties)), FGetEditablePropertiesRef>,
		"UE 5.8 DynamicMaterial bounded editable-property reference changed");
	static_assert(std::is_same_v<decltype(static_cast<FGetPropertyConnectionRef>(
		&UDMMaterialProperty::GetInputConnectionMap)), FGetPropertyConnectionRef>,
		"UE 5.8 DynamicMaterial property connection reference changed");
	static_assert(std::is_same_v<decltype(static_cast<FGetSlotLayersRef>(
		&UDMMaterialSlot::GetLayers)), FGetSlotLayersRef>,
		"UE 5.8 DynamicMaterial slot layers reference changed");
	static_assert(std::is_same_v<decltype(static_cast<FGetSlotOutputTypesRef>(
		&UDMMaterialSlot::GetOutputConnectorTypesForMaterialProperty)), FGetSlotOutputTypesRef>,
		"UE 5.8 DynamicMaterial slot output-type reference changed");
	static_assert(std::is_same_v<decltype(static_cast<FGetSlotReferencesRef>(
		&UDMMaterialSlot::GetSlotsReferencedBy)), FGetSlotReferencesRef>,
		"UE 5.8 DynamicMaterial slot reference-map signature changed");
	static_assert(std::is_same_v<decltype(static_cast<FGetRuntimeComponentsRef>(
		&UDynamicMaterialModel::GetRuntimeComponents)), FGetRuntimeComponentsRef>,
		"UE 5.8 DynamicMaterial runtime-component reference changed");
	static_assert(std::is_same_v<decltype(static_cast<FGetMaterialProperty>(
		&UDynamicMaterialModelEditorOnlyData::GetMaterialProperty)), FGetMaterialProperty>,
		"UE 5.8 DynamicMaterial finite property lookup signature changed");
	static_assert(std::is_same_v<decltype(static_cast<FGetSlotForProperty>(
		&UDynamicMaterialModelEditorOnlyData::GetSlotForMaterialProperty)), FGetSlotForProperty>,
		"UE 5.8 DynamicMaterial property-slot lookup signature changed");
	static_assert(std::is_same_v<decltype(static_cast<FGetDomain>(
		&UDynamicMaterialModelEditorOnlyData::GetDomain)), FGetDomain>,
		"UE 5.8 DynamicMaterial domain getter signature changed");
	static_assert(std::is_same_v<decltype(static_cast<FGetBlendMode>(
		&UDynamicMaterialModelEditorOnlyData::GetBlendMode)), FGetBlendMode>,
		"UE 5.8 DynamicMaterial blend getter signature changed");
	static_assert(std::is_same_v<decltype(static_cast<FGetEditorState>(
		&UDynamicMaterialModelEditorOnlyData::GetState)), FGetEditorState>,
		"UE 5.8 DynamicMaterial editor-state getter signature changed");
	static_assert(std::is_same_v<decltype(static_cast<FIsPreviewModified>(
		&UDynamicMaterialModelBase::IsPreviewModified)), FIsPreviewModified>,
		"UE 5.8 DynamicMaterial preview-state getter signature changed");
	static_assert(std::is_same_v<decltype(static_cast<FTryExactPackage>(
		&IAssetRegistry::TryGetAssetPackageData)), FTryExactPackage>,
		"UE 5.8 Asset Registry nonblocking exact-package signature changed");
}

#endif // WITH_DEV_AUTOMATION_TESTS
