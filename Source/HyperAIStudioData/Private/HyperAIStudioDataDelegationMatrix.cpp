// Games by Hyper 2026.

#include "HyperAIStudioDataDelegationMatrix.h"

namespace
{
	#define EPIC_ROW(Group, Name, Life, Access, Decl, Impl, Domain) \
		{TEXT(Group), TEXT(Name), TEXT(Life), TEXT(Access), TEXT(Decl), TEXT(Impl), TEXT(Domain)}

	static const FHyperAIStudioDataEpicDelegation EpicRows[] = {
		EPIC_ROW("ConfigSettingsToolset.ConfigSettingsToolset", "GetSectionPropertyValues", "inspect", "read", "ConfigSettingsToolset.h:74", "ConfigSettingsToolset.cpp:216", "config_section_state"),
		EPIC_ROW("ConfigSettingsToolset.ConfigSettingsToolset", "GetSectionSchema", "inspect", "read", "ConfigSettingsToolset.h:58", "ConfigSettingsToolset.cpp:193", "config_section_state"),
		EPIC_ROW("ConfigSettingsToolset.ConfigSettingsToolset", "ListCategories", "discover", "read", "ConfigSettingsToolset.h:33", "ConfigSettingsToolset.cpp:121", "config_settings_catalog"),
		EPIC_ROW("ConfigSettingsToolset.ConfigSettingsToolset", "ListContainers", "discover", "read", "ConfigSettingsToolset.h:24", "ConfigSettingsToolset.cpp:104", "config_settings_catalog"),
		EPIC_ROW("ConfigSettingsToolset.ConfigSettingsToolset", "ListSections", "discover", "read", "ConfigSettingsToolset.h:43", "ConfigSettingsToolset.cpp:151", "config_settings_catalog"),
		EPIC_ROW("ConfigSettingsToolset.ConfigSettingsToolset", "ResetSectionToDefaults", "delete", "destructive", "ConfigSettingsToolset.h:124", "ConfigSettingsToolset.cpp:362", "editor_config_overrides"),
		EPIC_ROW("ConfigSettingsToolset.ConfigSettingsToolset", "SaveSection", "save", "external", "ConfigSettingsToolset.h:110", "ConfigSettingsToolset.cpp:341", "editor_config_file"),
		EPIC_ROW("ConfigSettingsToolset.ConfigSettingsToolset", "SetSectionProperties", "save", "external", "ConfigSettingsToolset.h:95", "ConfigSettingsToolset.cpp:249", "editor_config_file"),

		EPIC_ROW("ConversationToolset.ConversationTools", "get_all_nodes", "inspect", "read", "conversation.py:38", "conversation.py:38", "data_observation"),
		EPIC_ROW("ConversationToolset.ConversationTools", "get_node_by_guid", "inspect", "read", "conversation.py:68", "conversation.py:68", "data_observation"),
		EPIC_ROW("ConversationToolset.ConversationTools", "get_node_connections", "inspect", "read", "conversation.py:85", "conversation.py:85", "data_observation"),
		EPIC_ROW("ConversationToolset.ConversationTools", "get_node_guids", "inspect", "read", "conversation.py:54", "conversation.py:54", "data_observation"),
		EPIC_ROW("ConversationToolset.ConversationTools", "get_sub_nodes", "inspect", "read", "conversation.py:97", "conversation.py:97", "data_observation"),
		EPIC_ROW("ConversationToolset.ConversationTools", "list_entry_points", "discover", "read", "conversation.py:14", "conversation.py:14", "data_observation"),
		EPIC_ROW("ConversationToolset.ConversationTools", "list_speakers", "discover", "read", "conversation.py:26", "conversation.py:26", "data_observation"),

		EPIC_ROW("DataRegistryToolset.DataRegistryTools", "GetItems", "inspect", "read", "DataRegistryTools.h:151", "DataRegistryTools.cpp:214", "data_registry_state"),
		EPIC_ROW("DataRegistryToolset.DataRegistryTools", "GetRegistryInfo", "inspect", "read", "DataRegistryTools.h:104", "DataRegistryTools.cpp:134", "data_registry_state"),
		EPIC_ROW("DataRegistryToolset.DataRegistryTools", "GetSchema", "inspect", "read", "DataRegistryTools.h:112", "DataRegistryTools.cpp:153", "data_registry_state"),
		EPIC_ROW("DataRegistryToolset.DataRegistryTools", "ListDataSources", "discover", "read", "DataRegistryTools.h:130", "DataRegistryTools.cpp:192", "data_registry_catalog"),
		EPIC_ROW("DataRegistryToolset.DataRegistryTools", "ListItems", "discover", "read", "DataRegistryTools.h:120", "DataRegistryTools.cpp:172", "data_registry_catalog"),
		EPIC_ROW("DataRegistryToolset.DataRegistryTools", "ListRegistries", "discover", "read", "DataRegistryTools.h:96", "DataRegistryTools.cpp:106", "data_registry_catalog"),
		EPIC_ROW("DataRegistryToolset.DataRegistryTools", "ListRuntimeSources", "discover", "read", "DataRegistryTools.h:140", "DataRegistryTools.cpp:203", "data_registry_catalog"),

		EPIC_ROW("EditorToolset.CurveTableTools", "add_key", "edit", "edit", "curve_table.py:127", "curve_table.py:127", "data_authored_state"),
		EPIC_ROW("EditorToolset.CurveTableTools", "add_row", "edit", "edit", "curve_table.py:80", "curve_table.py:80", "data_authored_state"),
		EPIC_ROW("EditorToolset.CurveTableTools", "create", "create", "edit", "curve_table.py:49", "curve_table.py:49", "data_authored_state"),
		EPIC_ROW("EditorToolset.CurveTableTools", "get_keys", "inspect", "read", "curve_table.py:162", "curve_table.py:162", "data_observation"),
		EPIC_ROW("EditorToolset.CurveTableTools", "import_file", "save", "external", "curve_table.py:16", "curve_table.py:16", "data_file_io"),
		EPIC_ROW("EditorToolset.CurveTableTools", "list_rows", "discover", "read", "curve_table.py:67", "curve_table.py:67", "data_observation"),
		EPIC_ROW("EditorToolset.CurveTableTools", "remove_row", "delete", "destructive", "curve_table.py:97", "curve_table.py:97", "data_authored_removal"),
		EPIC_ROW("EditorToolset.CurveTableTools", "rename_row", "edit", "edit", "curve_table.py:110", "curve_table.py:110", "data_authored_state"),
		EPIC_ROW("EditorToolset.CurveTableTools", "set_keys", "edit", "destructive", "curve_table.py:143", "curve_table.py:143", "data_authored_removal"),

		EPIC_ROW("EditorToolset.DataAssetTools", "create", "create", "edit", "data_asset.py:16", "data_asset.py:16", "assets_authored_state"),

		EPIC_ROW("EditorToolset.DataTableTools", "add_rows", "edit", "edit", "data_table.py:111", "data_table.py:111", "data_authored_state"),
		EPIC_ROW("EditorToolset.DataTableTools", "create", "create", "edit", "data_table.py:59", "data_table.py:59", "data_authored_state"),
		EPIC_ROW("EditorToolset.DataTableTools", "get_rows", "inspect", "read", "data_table.py:170", "data_table.py:170", "data_observation"),
		EPIC_ROW("EditorToolset.DataTableTools", "get_schema", "inspect", "read", "data_table.py:83", "data_table.py:83", "data_observation"),
		EPIC_ROW("EditorToolset.DataTableTools", "import_file", "save", "external", "data_table.py:34", "data_table.py:34", "data_file_io"),
		EPIC_ROW("EditorToolset.DataTableTools", "list_rows", "discover", "read", "data_table.py:98", "data_table.py:98", "data_observation"),
		EPIC_ROW("EditorToolset.DataTableTools", "remove_rows", "delete", "destructive", "data_table.py:128", "data_table.py:128", "data_authored_removal"),
		EPIC_ROW("EditorToolset.DataTableTools", "rename_rows", "edit", "edit", "data_table.py:144", "data_table.py:144", "data_authored_state"),
		EPIC_ROW("EditorToolset.DataTableTools", "search_row_structs", "discover", "read", "data_table.py:20", "data_table.py:20", "data_observation"),
		EPIC_ROW("EditorToolset.DataTableTools", "set_rows", "edit", "edit", "data_table.py:200", "data_table.py:200", "data_authored_state"),

		EPIC_ROW("EditorToolset.StringTableTools", "create", "create", "edit", "string_table.py:63", "string_table.py:63", "data_authored_state"),
		EPIC_ROW("EditorToolset.StringTableTools", "get_entry", "inspect", "read", "string_table.py:126", "string_table.py:126", "data_observation"),
		EPIC_ROW("EditorToolset.StringTableTools", "get_namespace", "inspect", "read", "string_table.py:98", "string_table.py:98", "data_observation"),
		EPIC_ROW("EditorToolset.StringTableTools", "get_table_id", "inspect", "read", "string_table.py:82", "string_table.py:82", "data_observation"),
		EPIC_ROW("EditorToolset.StringTableTools", "import_file", "save", "external", "string_table.py:20", "string_table.py:20", "data_file_io"),
		EPIC_ROW("EditorToolset.StringTableTools", "list_keys", "discover", "read", "string_table.py:112", "string_table.py:112", "data_observation"),
		EPIC_ROW("EditorToolset.StringTableTools", "remove_entry", "delete", "destructive", "string_table.py:159", "string_table.py:159", "data_authored_removal"),
		EPIC_ROW("EditorToolset.StringTableTools", "set_entry", "edit", "edit", "string_table.py:142", "string_table.py:142", "data_authored_state"),

		EPIC_ROW("GameplayTagsToolset.GameplayTagsToolset", "AddTag", "save", "external", "GameplayTagsToolset.h:63", "GameplayTagsToolset.cpp:93", "gameplay_tag_ini"),
		EPIC_ROW("GameplayTagsToolset.GameplayTagsToolset", "FindReferencersByTag", "discover", "read", "GameplayTagsToolset.h:88", "GameplayTagsToolset.cpp:149", "gameplay_tag_catalog"),
		EPIC_ROW("GameplayTagsToolset.GameplayTagsToolset", "GetTagInfo", "inspect", "read", "GameplayTagsToolset.h:53", "GameplayTagsToolset.cpp:65", "gameplay_tag_state"),
		EPIC_ROW("GameplayTagsToolset.GameplayTagsToolset", "ListTags", "discover", "read", "GameplayTagsToolset.h:44", "GameplayTagsToolset.cpp:38", "gameplay_tag_catalog"),
		EPIC_ROW("GameplayTagsToolset.GameplayTagsToolset", "RemoveTag", "delete", "destructive", "GameplayTagsToolset.h:71", "GameplayTagsToolset.cpp:110", "gameplay_tag_ini"),
		EPIC_ROW("GameplayTagsToolset.GameplayTagsToolset", "RenameTag", "save", "external", "GameplayTagsToolset.h:80", "GameplayTagsToolset.cpp:127", "gameplay_tag_ini")
	};

	#undef EPIC_ROW

	#define REQUIREMENT_ROW(Source, Id, Life, Access, Disposition, Coverage) \
		{TEXT(Source), TEXT(Id), TEXT(Life), TEXT(Access), TEXT(Disposition), TEXT(Coverage)}

	static const FHyperAIStudioDataRequirementDisposition RequirementRows[] = {
		REQUIREMENT_ROW("hyperai_requirement", "capability.data.localization.culture_edit", "edit", "edit", "hyperai_add", "excluded_localization_file_effect"),
		REQUIREMENT_ROW("hyperai_requirement", "capability.data.asset.create", "create", "edit", "hyperai_add", "epic_delegate"),
		REQUIREMENT_ROW("hyperai_requirement", "capability.data.table.create", "create", "edit", "hyperai_add", "epic_delegate"),
		REQUIREMENT_ROW("hyperai_requirement", "capability.data.string_table.create", "create", "edit", "hyperai_add", "epic_delegate"),
		REQUIREMENT_ROW("hyperai_requirement", "capability.data.table.edit", "edit", "edit", "hyperai_add", "epic_delegate"),
		REQUIREMENT_ROW("hyperai_requirement", "capability.data.localization.coverage_export", "inspect", "read", "hyperai_add", "excluded_raw_export"),
		REQUIREMENT_ROW("hyperai_requirement", "capability.data.localization.po_inventory", "inspect", "read", "hyperai_add", "excluded_raw_export"),
		REQUIREMENT_ROW("hyperai_requirement", "capability.data.localization.targets_list", "discover", "read", "hyperai_add", "excluded_filesystem_discovery"),
		REQUIREMENT_ROW("hyperai_requirement", "capability.data.gameplay_tags.manage", "inspect", "read", "hyperai_add", "epic_delegate"),
		REQUIREMENT_ROW("hyperai_requirement", "capability.data.table.read", "inspect", "read", "hyperai_add", "epic_delegate"),
		REQUIREMENT_ROW("hyperai_requirement", "capability.data.chooser.adapter", "discover", "unavailable", "capability_gated", "capability_gated_optional_adapter"),
		REQUIREMENT_ROW("hyperai_requirement", "capability.data.registry.adapter", "discover", "unavailable", "capability_gated", "epic_delegate"),
		REQUIREMENT_ROW("hyperai_requirement", "capability.data.table.row_add", "edit", "edit", "hyperai_add", "epic_delegate"),
		REQUIREMENT_ROW("hyperai_requirement", "capability.data.table.schema_create", "create", "edit", "hyperai_add", "epic_delegate"),
		REQUIREMENT_ROW("hyperai_requirement", "capability.data.enum.create", "create", "edit", "hyperai_add", "hyperai_shadow_gap"),
		REQUIREMENT_ROW("hyperai_requirement", "capability.data.struct.create", "create", "edit", "hyperai_add", "hyperai_shadow_gap"),
		REQUIREMENT_ROW("hyperai_requirement", "capability.data.table.json_import", "edit", "edit", "hyperai_add", "excluded_raw_import"),
		REQUIREMENT_ROW("hyperai_requirement", "capability.data.table.inspect", "inspect", "read", "hyperai_add", "epic_delegate")
	};

	#undef REQUIREMENT_ROW
}

TConstArrayView<FHyperAIStudioDataEpicDelegation> FHyperAIStudioDataDelegationMatrix::GetEpic()
{
	return MakeArrayView(EpicRows);
}

TConstArrayView<FHyperAIStudioDataRequirementDisposition> FHyperAIStudioDataDelegationMatrix::GetRequirements()
{
	return MakeArrayView(RequirementRows);
}
