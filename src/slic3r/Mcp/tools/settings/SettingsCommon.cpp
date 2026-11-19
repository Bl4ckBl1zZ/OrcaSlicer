#include "SettingsCommon.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <set>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/LocalesUtils.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/ParamsPanel.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Tab.hpp"

#include "../../McpError.hpp"
#include "../../McpRefresh.hpp"
#include "../../McpScene.hpp"
#include "../../McpSettings.hpp"

namespace Slic3r { namespace GUI { namespace Mcp { namespace SettingsEdit {

namespace {

// Per-slot arrays of the project config that filament_edit maintains; not written directly.
bool is_structural_project_key(const std::string& key)
{
    static const std::set<std::string> structural = {
        "filament_colour", "filament_multi_colour", "filament_colour_type", "filament_map", "filament_nozzle_map", "filament_volume_map",
        "filament_is_mixed", "filament_mixed_components", "filament_mixed_sublayer_ratios", "filament_mixed_gradient",
        "filament_mixed_gradient_range", "filament_mixed_gradient_curve", "filament_mixed_gradient_per_part"};
    return structural.count(key) != 0;
}

} // namespace

const char* where_name(Where where)
{
    switch (where) {
    case Where::Process: return "process";
    case Where::Printer: return "printer";
    case Where::Filament: return "filament";
    case Where::Project: return "project";
    case Where::Plate: return "plate";
    case Where::Object: return "object";
    case Where::Part: return "part";
    case Where::Modifier: return "modifier";
    case Where::LayerRange: return "layer_range";
    }
    return "?";
}

// The key sets come from the core catalogue (McpSettings.hpp).
bool key_allowed(const std::string& key, Where where)
{
    using Settings::Owner;
    switch (where) {
    case Where::Process: return Settings::owner_of(key) == Owner::Process;
    case Where::Printer: return Settings::owner_of(key) == Owner::Printer;
    case Where::Filament: return Settings::owner_of(key) == Owner::Filament;
    case Where::Project: return Settings::owner_of(key) == Owner::Project && !is_structural_project_key(key);
    case Where::Plate: return Settings::allowed_on_plate(key);
    case Where::Object: return Settings::allowed_on_object(key);
    case Where::Part:
    case Where::Modifier: return Settings::allowed_on_volume(key);
    case Where::LayerRange: return Settings::allowed_on_layer_range(key);
    }
    return false;
}

std::vector<std::string> scopes_of(const std::string& key)
{
    return Settings::scopes_of(key);
}

std::string forbidden_reason(const std::string& key)
{
    std::string why = Settings::write_refusal(key);
    if (why.empty())
        if (const ConfigOptionDef* def = Settings::find_def(key); def != nullptr && def->readonly)
            why = "the application marks it read-only";
    return why;
}

void check_key(const std::string& key, Where where)
{
    if (Settings::find_def(key) == nullptr) {
        const std::vector<std::string> nearest = Settings::nearest_keys(key);
        json details = json::object();
        details["key"] = key;
        details["nearest"] = nearest;
        std::string hint = "settings_search finds keys by words.";
        if (!nearest.empty())
            hint = "Did you mean " + nearest.front() + "? " + hint;
        fail("unknown_key", "There is no setting \"" + key + "\".", hint, details);
    }
    if (const std::string why = forbidden_reason(key); !why.empty()) {
        json details = json::object();
        details["key"] = key;
        fail("forbidden_key", "\"" + key + "\" cannot be changed through MCP: " + why + ".",
             "Ask the user to change it in the application if it is really needed.", details);
    }
    if (!key_allowed(key, where)) {
        const std::vector<std::string> scopes = scopes_of(key);
        json details = json::object();
        details["key"] = key;
        details["scope"] = where_name(where);
        details["scopes"] = scopes;
        std::string list;
        for (size_t i = 0; i < scopes.size(); ++i)
            list += (i ? ", " : "") + scopes[i];
        std::string hint = scopes.empty() ? "This setting cannot be written at any scope." : "Settable at: " + list + ".";
        if (where == Where::Project && is_structural_project_key(key))
            hint = "Filament colours and slot arrays are changed with filament_edit.";
        if (key == "layer_height" && (where == Where::Part || where == Where::Modifier))
            hint += " For a different layer height in part of an object use layer_range.";
        fail("key_not_in_scope", "\"" + key + "\" cannot be set at scope " + where_name(where) + ".", hint, details);
    }
}

std::vector<Parsed> parse_values(const json& values, Where where, const DynamicPrintConfig* reference)
{
    std::vector<Parsed> out;
    if (values.is_null())
        return out;
    if (!values.is_object())
        fail("invalid_argument", "`values` must be an object {key: value}.");
    for (auto it = values.begin(); it != values.end(); ++it) {
        const std::string& key = it.key();
        check_key(key, where);
        size_t size = 0;
        if (reference != nullptr)
            if (const auto* current = dynamic_cast<const ConfigOptionVectorBase*>(reference->option(key)); current != nullptr)
                size = current->size();
        // Type, enum, range and vector length: the core's conversion (McpSettings.hpp).
        const std::unique_ptr<ConfigOption> option = Settings::option_from_json(key, it.value(), size);
        out.push_back({key, option->serialize()});
    }
    return out;
}

json value_json(const DynamicPrintConfig& config, const std::string& key)
{
    return Settings::value_json_for(config, key);
}

json apply_to_model_config(ModelConfig& config, Where where, const json& values, const json& reset)
{
    const DynamicPrintConfig reference = full_config();
    const std::vector<Parsed> parsed = parse_values(values, where, &reference);
    std::vector<std::string>  resets;
    if (!reset.is_null()) {
        for (const json& item : reset) {
            const std::string key = item.get<std::string>();
            if (print_config_def.get(key) == nullptr)
                check_key(key, where); // unknown_key with suggestions
            if (values.is_object() && values.contains(key))
                fail("invalid_argument", "\"" + key + "\" is both in `values` and in `reset`.", "Give each key once.");
            resets.push_back(key);
        }
    }

    // Validation done; write.
    DynamicPrintConfig scratch;
    for (const Parsed& item : parsed)
        scratch.set_deserialize_strict(item.key, item.text);

    json changes = json::array();
    std::vector<std::string> keys;
    for (const Parsed& item : parsed)
        keys.push_back(item.key);
    std::vector<json> olds;
    for (const std::string& key : keys)
        olds.push_back(value_json(config.get(), key));
    if (!keys.empty())
        config.apply_only(scratch, keys);
    for (size_t i = 0; i < keys.size(); ++i) {
        json change = json::object();
        change["key"] = keys[i];
        change["old"] = olds[i];
        change["new"] = value_json(config.get(), keys[i]);
        changes.push_back(std::move(change));
    }
    for (const std::string& key : resets) {
        json change = json::object();
        change["key"] = key;
        change["old"] = value_json(config.get(), key);
        config.erase(key);
        change["new"] = nullptr;
        changes.push_back(std::move(change));
    }
    return changes;
}

DynamicPrintConfig full_config()
{
    return wxGetApp().preset_bundle->full_config();
}

namespace {

void reload_model_tabs()
{
    for (Tab* tab : {wxGetApp().get_model_tab(false), wxGetApp().get_model_tab(true), wxGetApp().get_layer_tab()})
        if (auto* model_tab = dynamic_cast<TabPrintModel*>(tab); model_tab != nullptr)
            model_tab->update_model_config();
    if (ParamsPanel* panel = wxGetApp().params_panel(); panel != nullptr)
        panel->notify_object_config_changed();
}

} // namespace

void refresh_after_model_config(int obj_idx, ModelObject* object, ModelVolume* volume)
{
    if (ObjectList* list = wxGetApp().obj_list(); list != nullptr)
        list->object_config_options_changed({object, volume});
    reload_model_tabs();
    Refresh::object(obj_idx);
}

void refresh_after_layer_ranges(int obj_idx)
{
    (void) obj_idx;
    // The layer rows are built from layer_config_ranges; the object list's own resync rebuilds them.
    Refresh::full();
    reload_model_tabs();
}

}}}} // namespace Slic3r::GUI::Mcp::SettingsEdit
