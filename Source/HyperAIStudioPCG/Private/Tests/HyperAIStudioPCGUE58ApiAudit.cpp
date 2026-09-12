// Games by Hyper 2026.

#include "HyperAIStudioPCGToolset.h"

#include "AssetRegistry/IAssetRegistry.h"
#include "Misc/AutomationTest.h"
#include "PCGComponent.h"
#include "PCGEdge.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGPin.h"
#include "PCGSettings.h"
#include "StructUtils/PropertyBag.h"
#include "UObject/UnrealType.h"

#include <type_traits>

// Compile-time pins for the UE 5.8 public, non-ProcessEvent API surface used by this pack.
static_assert(std::is_member_function_pointer_v<decltype(&UPCGGraph::GetNodes)>);
static_assert(std::is_member_function_pointer_v<decltype(&UPCGGraph::GetCommentNodes)>);
static_assert(std::is_member_function_pointer_v<decltype(&UPCGNode::GetInputPins)>);
static_assert(std::is_member_function_pointer_v<decltype(&UPCGNode::GetOutputPins)>);
static_assert(std::is_member_function_pointer_v<decltype(&UPCGPin::IsOutputPin)>);
static_assert(std::is_member_function_pointer_v<decltype(&UPCGSettings::GetSettingsCrc)>);
static_assert(std::is_member_function_pointer_v<decltype(&UPCGComponent::IsGenerating)>);
static_assert(std::is_member_function_pointer_v<decltype(&UPCGComponent::IsCleaningUp)>);
static_assert(std::is_member_function_pointer_v<decltype(&UPCGComponent::IsRefreshInProgress)>);
static_assert(std::is_member_function_pointer_v<decltype(&UPCGComponent::GetGenerationTaskId)>);
static_assert(std::is_member_function_pointer_v<decltype(&UPCGComponent::GetCleanupTaskId)>);
static_assert(std::is_member_function_pointer_v<decltype(&UPCGComponent::GetGraphInstance)>);
static_assert(std::is_member_object_pointer_v<decltype(&UPCGGraphInstance::Graph)>);
static_assert(std::is_member_object_pointer_v<decltype(&UPCGGraphInstance::ParametersOverrides)>);
static_assert(std::is_member_object_pointer_v<
	decltype(&FPCGOverrideInstancedPropertyBag::PropertiesIDsOverridden)>);

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioPCGUE58ApiAuditTest,
	"HyperAIStudio.NativeTools.PCG.UE58PublicApiAudit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioPCGUE58ApiAuditTest::RunTest(const FString& Parameters)
{
	TestNotNull(TEXT("PCG graph class is available"), UPCGGraph::StaticClass());
	TestNotNull(TEXT("PCG component class is available"), UPCGComponent::StaticClass());
	TestNotNull(TEXT("PCG node class is available"), UPCGNode::StaticClass());
	TestNotNull(TEXT("PCG pin class is available"), UPCGPin::StaticClass());
	TestNotNull(TEXT("PCG edge class is available"), UPCGEdge::StaticClass());
	TestNotNull(TEXT("Property bag descriptor API is available"), UPropertyBag::StaticClass());
	TestNotNull(TEXT("EmbeddedSubgraphs stays directly inspectable before copies"),
		FindFProperty<FArrayProperty>(UPCGGraph::StaticClass(), TEXT("EmbeddedSubgraphs")));
	TestNotNull(TEXT("Component GraphInstance remains a reflected persisted field"),
		FindFProperty<FObjectPropertyBase>(UPCGComponent::StaticClass(), TEXT("GraphInstance")));
	TestNotNull(TEXT("Component SchedulingPolicy remains a reflected persisted field"),
		FindFProperty<FObjectPropertyBase>(UPCGComponent::StaticClass(), TEXT("SchedulingPolicy")));
	TestNotNull(TEXT("Graph-instance override bag remains reflected"),
		FindFProperty<FStructProperty>(UPCGGraphInstance::StaticClass(), TEXT("ParametersOverrides")));
	TestNotNull(TEXT("Override mask remains a reflected set"),
		FindFProperty<FSetProperty>(FPCGOverrideInstancedPropertyBag::StaticStruct(),
			TEXT("PropertiesIDsOverridden")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
