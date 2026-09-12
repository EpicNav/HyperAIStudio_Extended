// Games by Hyper 2026.

#include "HyperAIStudioUIDelegationMatrix.h"

TConstArrayView<FHyperAIStudioUIEpicDelegation> FHyperAIStudioUIEpicDelegationMatrix::Get()
{
	// Source locations are deliberately pinned to the UE 5.8 declarations and
	// implementations reviewed for this source candidate. These are delegation
	// evidence, not alternate registrations or copied Epic implementations.
	static const FHyperAIStudioUIEpicDelegation Matrix[] = {
		// UMGToolSet: 23.
		{TEXT("UMGToolSet"), TEXT("CreateWidgetBlueprint"), TEXT("create"), TEXT("edit"), TEXT("UMGToolSet.h:286"), TEXT("UMGToolSet.cpp:517"), TEXT("widget_blueprint_state")},
		{TEXT("UMGToolSet"), TEXT("AddWidget"), TEXT("create"), TEXT("edit"), TEXT("UMGToolSet.h:299"), TEXT("UMGToolSet.cpp:607"), TEXT("widget_blueprint_state")},
		{TEXT("UMGToolSet"), TEXT("SetNamedSlotContent"), TEXT("edit"), TEXT("edit"), TEXT("UMGToolSet.h:310"), TEXT("UMGToolSet.cpp:671"), TEXT("widget_blueprint_state")},
		{TEXT("UMGToolSet"), TEXT("GetWidgets"), TEXT("inspect"), TEXT("read"), TEXT("UMGToolSet.h:322"), TEXT("UMGToolSet.cpp:769"), TEXT("widget_blueprint_state")},
		{TEXT("UMGToolSet"), TEXT("GetNamedSlots"), TEXT("inspect"), TEXT("read"), TEXT("UMGToolSet.h:329"), TEXT("UMGToolSet.cpp:917"), TEXT("widget_blueprint_state")},
		{TEXT("UMGToolSet"), TEXT("ListWidgetBlueprints"), TEXT("discover"), TEXT("read"), TEXT("UMGToolSet.h:336"), TEXT("UMGToolSet.cpp:1074"), TEXT("widget_blueprint_catalog")},
		{TEXT("UMGToolSet"), TEXT("ListWidgetClasses"), TEXT("discover"), TEXT("read"), TEXT("UMGToolSet.h:343"), TEXT("UMGToolSet.cpp:1107"), TEXT("widget_blueprint_catalog")},
		{TEXT("UMGToolSet"), TEXT("GetWidgetClassInfo"), TEXT("inspect"), TEXT("read"), TEXT("UMGToolSet.h:353"), TEXT("UMGToolSet.cpp:1134"), TEXT("widget_blueprint_state")},
		{TEXT("UMGToolSet"), TEXT("MoveWidget"), TEXT("edit"), TEXT("edit"), TEXT("UMGToolSet.h:365"), TEXT("UMGToolSet.cpp:1149"), TEXT("widget_blueprint_state")},
		{TEXT("UMGToolSet"), TEXT("RemoveWidget"), TEXT("delete"), TEXT("destructive"), TEXT("UMGToolSet.h:373"), TEXT("UMGToolSet.cpp:1170"), TEXT("widget_blueprint_state")},
		{TEXT("UMGToolSet"), TEXT("RenameWidget"), TEXT("edit"), TEXT("edit"), TEXT("UMGToolSet.h:382"), TEXT("UMGToolSet.cpp:1182"), TEXT("widget_blueprint_state")},
		{TEXT("UMGToolSet"), TEXT("ToggleWidgetAsVariable"), TEXT("edit"), TEXT("edit"), TEXT("UMGToolSet.h:391"), TEXT("UMGToolSet.cpp:1210"), TEXT("widget_blueprint_state")},
		{TEXT("UMGToolSet"), TEXT("BindToEventProperty"), TEXT("edit"), TEXT("edit"), TEXT("UMGToolSet.h:410"), TEXT("UMGToolSet.cpp:573"), TEXT("widget_blueprint_state")},
		{TEXT("UMGToolSet"), TEXT("WrapWidgets"), TEXT("edit"), TEXT("edit"), TEXT("UMGToolSet.h:425"), TEXT("UMGToolSet.cpp:586"), TEXT("widget_blueprint_state")},
		{TEXT("UMGToolSet"), TEXT("GetWidgetDescription"), TEXT("inspect"), TEXT("read"), TEXT("UMGToolSet.h:441"), TEXT("UMGToolSet.cpp:1369"), TEXT("widget_blueprint_state")},
		{TEXT("UMGToolSet"), TEXT("GetWidgetTreeDepth"), TEXT("inspect"), TEXT("read"), TEXT("UMGToolSet.h:449"), TEXT("UMGToolSet.cpp:1416"), TEXT("widget_blueprint_state")},
		{TEXT("UMGToolSet"), TEXT("AddUIComponent"), TEXT("create"), TEXT("edit"), TEXT("UMGToolSet.h:464"), TEXT("UMGToolSet.cpp:1225"), TEXT("widget_blueprint_state")},
		{TEXT("UMGToolSet"), TEXT("RemoveUIComponent"), TEXT("delete"), TEXT("destructive"), TEXT("UMGToolSet.h:474"), TEXT("UMGToolSet.cpp:1260"), TEXT("widget_blueprint_state")},
		{TEXT("UMGToolSet"), TEXT("MoveUIComponent"), TEXT("edit"), TEXT("edit"), TEXT("UMGToolSet.h:486"), TEXT("UMGToolSet.cpp:1277"), TEXT("widget_blueprint_state")},
		{TEXT("UMGToolSet"), TEXT("ReplaceWidgetWithTemplate"), TEXT("edit"), TEXT("destructive"), TEXT("UMGToolSet.h:505"), TEXT("UMGToolSet.cpp:971"), TEXT("widget_replacement_state")},
		{TEXT("UMGToolSet"), TEXT("ReplaceWidgetWithNamedSlot"), TEXT("edit"), TEXT("destructive"), TEXT("UMGToolSet.h:518"), TEXT("UMGToolSet.cpp:1052"), TEXT("widget_replacement_state")},
		{TEXT("UMGToolSet"), TEXT("ReplaceWidgetWithChild"), TEXT("edit"), TEXT("destructive"), TEXT("UMGToolSet.h:529"), TEXT("UMGToolSet.cpp:1063"), TEXT("widget_replacement_state")},
		{TEXT("UMGToolSet"), TEXT("CompileWidgetBlueprint"), TEXT("compile"), TEXT("external"), TEXT("UMGToolSet.h:540"), TEXT("UMGToolSet.cpp:1298"), TEXT("widget_blueprint_compilation")},

		// MVVMToolset: 9. ListWidget* is classified edit because RequestView creates state.
		{TEXT("MVVMToolset"), TEXT("CreateViewModel"), TEXT("create"), TEXT("edit"), TEXT("MVVMToolset.h:42"), TEXT("MVVMToolset.cpp:275"), TEXT("mvvm_blueprint_state")},
		{TEXT("MVVMToolset"), TEXT("AddViewModelProperty"), TEXT("create"), TEXT("edit"), TEXT("MVVMToolset.h:54"), TEXT("MVVMToolset.cpp:324"), TEXT("mvvm_blueprint_state")},
		{TEXT("MVVMToolset"), TEXT("ListViewModels"), TEXT("discover"), TEXT("read"), TEXT("MVVMToolset.h:63"), TEXT("MVVMToolset.cpp:376"), TEXT("mvvm_catalog")},
		{TEXT("MVVMToolset"), TEXT("ListWidgetViewModels"), TEXT("edit"), TEXT("edit"), TEXT("MVVMToolset.h:72"), TEXT("MVVMToolset.cpp:403"), TEXT("mvvm_blueprint_state")},
		{TEXT("MVVMToolset"), TEXT("AddViewModelToWidget"), TEXT("edit"), TEXT("edit"), TEXT("MVVMToolset.h:81"), TEXT("MVVMToolset.cpp:423"), TEXT("mvvm_blueprint_state")},
		{TEXT("MVVMToolset"), TEXT("ListWidgetViewBindings"), TEXT("edit"), TEXT("edit"), TEXT("MVVMToolset.h:90"), TEXT("MVVMToolset.cpp:442"), TEXT("mvvm_blueprint_state")},
		{TEXT("MVVMToolset"), TEXT("RemoveWidgetViewBinding"), TEXT("delete"), TEXT("destructive"), TEXT("MVVMToolset.h:99"), TEXT("MVVMToolset.cpp:454"), TEXT("mvvm_widget_binding")},
		{TEXT("MVVMToolset"), TEXT("CreateViewBinding"), TEXT("create"), TEXT("edit"), TEXT("MVVMToolset.h:122"), TEXT("MVVMToolset.cpp:482"), TEXT("mvvm_blueprint_state")},
		{TEXT("MVVMToolset"), TEXT("ListConversionFunctions"), TEXT("discover"), TEXT("read"), TEXT("MVVMToolset.h:130"), TEXT("MVVMToolset.cpp:521"), TEXT("mvvm_catalog")},

		// SlateInspectorToolset: 14. No live input/render/observer action is invoked here.
		{TEXT("SlateInspectorToolset"), TEXT("Snapshot"), TEXT("inspect"), TEXT("read"), TEXT("SlateInspectorToolset.h:91"), TEXT("SlateInspectorToolset.cpp:167"), TEXT("slate_widget_tree")},
		{TEXT("SlateInspectorToolset"), TEXT("Observe"), TEXT("runtime"), TEXT("external"), TEXT("SlateInspectorToolset.h:101"), TEXT("SlateInspectorToolset.cpp:196"), TEXT("slate_observer_runtime")},
		{TEXT("SlateInspectorToolset"), TEXT("Unobserve"), TEXT("runtime"), TEXT("external"), TEXT("SlateInspectorToolset.h:106"), TEXT("SlateInspectorToolset.cpp:213"), TEXT("slate_observer_runtime")},
		{TEXT("SlateInspectorToolset"), TEXT("ListObservers"), TEXT("discover"), TEXT("read"), TEXT("SlateInspectorToolset.h:112"), TEXT("SlateInspectorToolset.cpp:218"), TEXT("slate_observer_catalog")},
		{TEXT("SlateInspectorToolset"), TEXT("Screenshot"), TEXT("render"), TEXT("external"), TEXT("SlateInspectorToolset.h:118"), TEXT("SlateInspectorToolset.cpp:254"), TEXT("slate_image_capture")},
		{TEXT("SlateInspectorToolset"), TEXT("Click"), TEXT("runtime"), TEXT("external"), TEXT("SlateInspectorToolset.h:128"), TEXT("SlateInspectorToolset.cpp:292"), TEXT("slate_input_and_window_runtime")},
		{TEXT("SlateInspectorToolset"), TEXT("Hover"), TEXT("runtime"), TEXT("external"), TEXT("SlateInspectorToolset.h:133"), TEXT("SlateInspectorToolset.cpp:308"), TEXT("slate_input_and_window_runtime")},
		{TEXT("SlateInspectorToolset"), TEXT("Type"), TEXT("runtime"), TEXT("external"), TEXT("SlateInspectorToolset.h:141"), TEXT("SlateInspectorToolset.cpp:330"), TEXT("slate_input_and_window_runtime")},
		{TEXT("SlateInspectorToolset"), TEXT("PressKey"), TEXT("runtime"), TEXT("external"), TEXT("SlateInspectorToolset.h:147"), TEXT("SlateInspectorToolset.cpp:372"), TEXT("slate_input_and_window_runtime")},
		{TEXT("SlateInspectorToolset"), TEXT("SelectOption"), TEXT("runtime"), TEXT("external"), TEXT("SlateInspectorToolset.h:154"), TEXT("SlateInspectorToolset.cpp:453"), TEXT("slate_input_and_window_runtime")},
		{TEXT("SlateInspectorToolset"), TEXT("Drag"), TEXT("runtime"), TEXT("external"), TEXT("SlateInspectorToolset.h:161"), TEXT("SlateInspectorToolset.cpp:548"), TEXT("slate_input_and_window_runtime")},
		{TEXT("SlateInspectorToolset"), TEXT("Windows"), TEXT("runtime"), TEXT("external"), TEXT("SlateInspectorToolset.h:167"), TEXT("SlateInspectorToolset.cpp:618"), TEXT("slate_input_and_window_runtime")},
		{TEXT("SlateInspectorToolset"), TEXT("WaitFor"), TEXT("inspect"), TEXT("read"), TEXT("SlateInspectorToolset.h:174"), TEXT("SlateInspectorToolset.cpp:673"), TEXT("slate_widget_tree")},
		{TEXT("SlateInspectorToolset"), TEXT("FillForm"), TEXT("runtime"), TEXT("external"), TEXT("SlateInspectorToolset.h:180"), TEXT("SlateInspectorToolset.cpp:749"), TEXT("slate_input_and_window_runtime")},
	};
	static_assert(UE_ARRAY_COUNT(Matrix) == 46, "UI Epic delegation matrix must remain exact 46/46.");
	return MakeArrayView(Matrix);
}
