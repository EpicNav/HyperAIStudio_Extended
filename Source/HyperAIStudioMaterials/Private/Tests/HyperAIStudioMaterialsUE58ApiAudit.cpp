// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/IAssetRegistry.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialFunction.h"

#include <type_traits>

namespace HyperAIStudio::Materials::UE58ApiAudit
{
	using FCountInputs = int32 (UMaterialExpression::*)() const;
	using FGetInput = FExpressionInput* (UMaterialExpression::*)(int32);
	using FGetOutputs = TArray<FExpressionOutput>& (UMaterialExpression::*)();
	using FGetInputValueType = EMaterialValueType (UMaterialExpression::*)(int32);
	using FGetOutputValueType = EMaterialValueType (UMaterialExpression::*)(int32);
	using FGetParameterExpressionId = FGuid& (UMaterialExpression::*)();
	using FGetExpressionInputDescription = bool (UMaterial::*)(
		EMaterialProperty, FMaterialInputDescription&);
	using FGetExpressionCollection = const FMaterialExpressionCollection& (UMaterial::*)() const;
	using FIsCompiling = bool (UMaterial::*)() const;
	using FGetMaterialExpressionsView =
		TConstArrayView<TObjectPtr<UMaterialExpression>> (UMaterial::*)() const;
	using FGetFunctionExpressionsView =
		TConstArrayView<TObjectPtr<UMaterialExpression>> (UMaterialFunction::*)() const;
	using FTryExactPackage = UE::AssetRegistry::EExists (IAssetRegistry::*)(
		FName, FAssetPackageData&, bool) const;

	static_assert(std::is_same_v<decltype(&UMaterialExpression::CountInputs), FCountInputs>,
		"UE 5.8 CountInputs signature changed");
	static_assert(std::is_same_v<decltype(static_cast<FGetInput>(&UMaterialExpression::GetInput)), FGetInput>,
		"UE 5.8 GetInput signature changed");
	static_assert(std::is_same_v<decltype(static_cast<FGetOutputs>(&UMaterialExpression::GetOutputs)),
		FGetOutputs>, "UE 5.8 bounded output reference signature changed");
	static_assert(std::is_same_v<decltype(static_cast<FGetInputValueType>(
		&UMaterialExpression::GetInputValueType)), FGetInputValueType>,
		"UE 5.8 material input type signature changed");
	static_assert(std::is_same_v<decltype(static_cast<FGetOutputValueType>(
		&UMaterialExpression::GetOutputValueType)), FGetOutputValueType>,
		"UE 5.8 material output type signature changed");
	static_assert(std::is_same_v<decltype(static_cast<FGetParameterExpressionId>(
		&UMaterialExpression::GetParameterExpressionId)), FGetParameterExpressionId>,
		"UE 5.8 parameter GUID signature changed");
	static_assert(std::is_same_v<decltype(static_cast<FGetExpressionInputDescription>(
		&UMaterial::GetExpressionInputDescription)), FGetExpressionInputDescription>,
		"UE 5.8 material property projection signature changed");
	static_assert(std::is_same_v<decltype(static_cast<FGetExpressionCollection>(
		&UMaterial::GetExpressionCollection)), FGetExpressionCollection>,
		"UE 5.8 expression collection reference signature changed");
	static_assert(std::is_same_v<decltype(static_cast<FIsCompiling>(&UMaterial::IsCompiling)), FIsCompiling>,
		"UE 5.8 material compile-state signature changed");
	static_assert(std::is_same_v<decltype(static_cast<FGetMaterialExpressionsView>(
		&UMaterial::GetExpressions)), FGetMaterialExpressionsView>,
		"UE 5.8 Material bounded expression-view signature changed");
	static_assert(std::is_same_v<decltype(static_cast<FGetFunctionExpressionsView>(
		&UMaterialFunction::GetExpressions)), FGetFunctionExpressionsView>,
		"UE 5.8 MaterialFunction bounded expression-view signature changed");
	static_assert(std::is_same_v<decltype(static_cast<FTryExactPackage>(
		&IAssetRegistry::TryGetAssetPackageData)), FTryExactPackage>,
		"UE 5.8 Asset Registry nonblocking exact-package signature changed");
}

#endif // WITH_DEV_AUTOMATION_TESTS
