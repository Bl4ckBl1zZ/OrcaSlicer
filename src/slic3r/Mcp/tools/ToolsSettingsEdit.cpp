// settings_set: set or reset settings at every scope.
//
//   process / printer / {filament: n}   the edited preset, through Tab::apply_preset_config_values (the
//                                        path a hand edit takes: dependent options, dirty marker, plater
//                                        update). The preset becomes modified, not saved. Journal.
//   project                              PresetBundle::project_config, as sync_colour_config writes it.
//                                        Journal.
//   {plate: n}                           the plate's own settings through the PartPlate setters the plate
//                                        tab uses. Undo stack.
//   {object: id} / {volume: id} /        the item's ModelConfig, as the per-object settings tab writes it.
//   {layer_range: {object, z_min, z_max}} Undo stack.
//
// Validation (key, scope, type, enum, range, forbidden keys) happens for the whole batch before
// anything is written.

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/ObjectDataViewModel.hpp"
#include "slic3r/GUI/PresetComboBoxes.hpp"
#include "slic3r/GUI/ParamsPanel.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Tab.hpp"

#include "../McpFiles.hpp"
#include "../McpIds.hpp"
#include "../McpRefresh.hpp"
#include "../McpScene.hpp"
#include "../McpSchema.hpp"
#include "../McpSettings.hpp"
#include "../McpTool.hpp"
#include "settings/PresetOps.hpp"
#include "settings/SettingsCommon.hpp"

namespace Slic3r { namespace GUI { namespace Mcp {

namespace {

using namespace SettingsEdit;

PresetBundle& bundle() { return *wxGetApp().preset_bundle; }
Plater&       plater() { return *wxGetApp().plater(); }

std::vector<std::string> reset_keys(const json& reset)
{
    std::vector<std::string> out;
    if (reset.is_array())
        for (const json& key : reset)
            out.push_back(key.get<std::string>());
    return out;
}

void check_overlap(const json& values, const std::vector<std::string>& resets)
{
    std::set<std::string> seen;
    for (const std::string& key : resets) {
        if (!seen.insert(key).second)
            fail("invalid_argument", "\"" + key + "\" appears twice in `reset`.");
        if (values.is_object() && values.contains(key))
            fail("invalid_argument", "\"" + key + "\" is both in `values` and in `reset`.", "Give each key once.");
    }
}

// --- preset scopes --------------------------------------------------------------------------------

json diff_configs(const DynamicPrintConfig& before, const DynamicPrintConfig& after, const std::set<std::string>& skip)
{
    json out = json::array();
    for (const std::string& key : after.keys()) {
        if (skip.count(key) != 0)
            continue;
        const ConfigOption* a = before.option(key);
        const ConfigOption* b = after.option(key);
        if (a != nullptr && b != nullptr && *a == *b)
            continue;
        json item = json::object();
        item["key"] = key;
        // Redacts secrets (print host, credentials); null when the key is absent.
        item["old"] = Settings::value_json_for(before, key);
        item["new"] = Settings::value_json_for(after, key);
        out.push_back(std::move(item));
    }
    return out;
}

json set_preset_scope(ToolContext& ctx, Preset::Type type, int slot, const json& values, const std::vector<std::string>& resets)
{
    const Where where = type == Preset::TYPE_PRINT ? Where::Process : type == Preset::TYPE_PRINTER ? Where::Printer : Where::Filament;
    // Validate the batch first.
    const DynamicPrintConfig  reference = full_config();
    const std::vector<Parsed> parsed = parse_values(values, where, &reference);
    for (const std::string& key : resets)
        check_key(key, where);

    std::string preset_name;
    if (type == Preset::TYPE_FILAMENT)
        preset_name = PresetOps::bind_filament_tab(slot);
    PresetCollection& c = PresetOps::collection(type);
    Tab&              t = PresetOps::tab(type);
    preset_name = c.get_edited_preset().name;

    // reset = back to the saved preset's value.
    std::vector<std::pair<std::string, std::string>> batch;
    for (const Parsed& item : parsed)
        batch.emplace_back(item.key, item.text);
    const DynamicPrintConfig& saved = c.get_selected_preset().config;
    for (const std::string& key : resets)
        if (saved.has(key))
            batch.emplace_back(key, saved.opt_serialize(key));

    const DynamicPrintConfig before = c.get_edited_preset().config;
    const bool               auto_adjust = ctx.arg<bool>("auto_adjust", true);
    // The dependency checks of ConfigManipulation ask "... adjust automatically?" (Yes/No) or warn (OK).
    ctx.on_dialog({"", "?", auto_adjust ? DialogAnswer::Yes : DialogAnswer::No, true});
    ctx.on_dialog({"", "", DialogAnswer::Default, true});
    if (!batch.empty()) {
        try {
            t.apply_preset_config_values(batch);
        } catch (const ToolError&) {
            throw;
        } catch (const std::exception& e) {
            fail("invalid_value", std::string("The application rejected the change: ") + e.what());
        }
    }
    const DynamicPrintConfig& after = c.get_edited_preset().config;

    std::set<std::string> requested;
    json                  changed = json::array();
    for (const auto& [key, text] : batch) {
        requested.insert(key);
        json item = json::object();
        item["key"] = key;
        item["old"] = value_json(before, key);
        item["new"] = value_json(after, key);
        changed.push_back(std::move(item));
    }
    const json side_effects = diff_configs(before, after, requested);

    // Journal: revert puts back the old values of everything that changed.
    std::vector<std::pair<std::string, std::string>> undo;
    for (const json& item : changed)
        if (item["old"] != item["new"] && before.has(item["key"].get<std::string>()))
            undo.emplace_back(item["key"].get<std::string>(), before.opt_serialize(item["key"].get<std::string>()));
    for (const json& item : side_effects)
        if (before.has(item["key"].get<std::string>()))
            undo.emplace_back(item["key"].get<std::string>(), before.opt_serialize(item["key"].get<std::string>()));
    json journal_before = json::object(), journal_after = json::object();
    for (const json& item : changed) {
        journal_before[item["key"].get<std::string>()] = item["old"];
        journal_after[item["key"].get<std::string>()] = item["new"];
    }
    json scope = type == Preset::TYPE_FILAMENT ? json{{"filament", slot}} : json(PresetOps::type_name(type));
    if (!undo.empty()) {
        ctx.journal(scope, "settings", journal_before, journal_after, [type, slot, preset_name, undo] {
            if (type == Preset::TYPE_FILAMENT)
                PresetOps::bind_filament_tab(slot);
            if (PresetOps::collection(type).get_edited_preset().name != preset_name)
                fail("conflict", "The " + std::string(PresetOps::type_name(type)) + " preset is no longer \"" + preset_name + "\".",
                     "Select it again (preset_select) before reverting.");
            PresetOps::tab(type).apply_preset_config_values(undo);
        });
        ctx.set_slice_invalidated();
    }

    json out = json::object();
    out["ok"] = true;
    out["scope"] = scope;
    out["preset"] = preset_name;
    out["modified"] = c.current_is_dirty();
    out["changed"] = changed;
    out["side_effects"] = side_effects;
    if (type == Preset::TYPE_FILAMENT) {
        json shared = json::array();
        for (size_t i = 0; i < bundle().filament_presets.size(); ++i)
            if (const Preset* p = bundle().filaments.find_preset(bundle().filament_presets[i], false); p != nullptr && p->name == preset_name)
                shared.push_back(i + 1);
        out["slots_sharing_preset"] = shared;
        if (shared.size() > 1)
            ctx.note("Slots " + shared.dump() + " use the same preset; the change applies to all of them.");
    }
    if (c.current_is_dirty())
        ctx.note("The preset is modified, not saved: preset_save keeps the edits as a user preset; projects store them either way.");
    return out;
}

// --- project scope --------------------------------------------------------------------------------

json set_project_scope(ToolContext& ctx, const json& values, const std::vector<std::string>& resets)
{
    if (!resets.empty())
        fail("invalid_argument", "Project settings have no saved value to go back to; set a value instead of `reset`.");
    const DynamicPrintConfig  reference = full_config();
    const std::vector<Parsed> parsed = parse_values(values, Where::Project, &reference);
    DynamicPrintConfig& project = bundle().project_config;
    const DynamicPrintConfig before = project;
    DynamicPrintConfig changed_config = project;
    for (const Parsed& item : parsed)
        changed_config.set_deserialize_strict(item.key, item.text);
    project.apply(changed_config);
    plater().update_project_dirty_from_presets();
    bundle().export_selections(*wxGetApp().app_config);
    plater().on_config_change(changed_config);

    json changed = json::array(), journal_before = json::object(), journal_after = json::object();
    std::vector<std::pair<std::string, std::string>> undo;
    for (const Parsed& item : parsed) {
        json entry = {{"key", item.key}, {"old", value_json(before, item.key)}, {"new", value_json(project, item.key)}};
        journal_before[item.key] = entry["old"];
        journal_after[item.key] = entry["new"];
        if (before.has(item.key))
            undo.emplace_back(item.key, before.opt_serialize(item.key));
        changed.push_back(std::move(entry));
    }
    ctx.journal("project", "settings", journal_before, journal_after, [undo] {
        PresetBundle&      pb = *wxGetApp().preset_bundle;
        DynamicPrintConfig config = pb.project_config;
        for (const auto& [key, text] : undo)
            config.set_deserialize_strict(key, text);
        pb.project_config.apply(config);
        wxGetApp().plater()->update_project_dirty_from_presets();
        wxGetApp().preset_bundle->export_selections(*wxGetApp().app_config);
        wxGetApp().plater()->on_config_change(config);
    });
    ctx.set_slice_invalidated();
    json out = json::object();
    out["ok"] = true;
    out["scope"] = "project";
    out["changed"] = changed;
    return out;
}

// --- plate scope ----------------------------------------------------------------------------------

json set_plate_scope(ToolContext& ctx, int plate_idx, const json& values, const std::vector<std::string>& resets)
{
    PartPlate* plate = plater().get_partplate_list().get_plate(plate_idx);
    const DynamicPrintConfig  reference = full_config();
    const std::vector<Parsed> parsed = parse_values(values, Where::Plate, &reference);
    for (const std::string& key : resets)
        check_key(key, Where::Plate);
    DynamicPrintConfig parsed_config;
    for (const Parsed& item : parsed) {
        if (item.key == "first_layer_print_sequence" || item.key == "other_layers_print_sequence")
            fail("invalid_argument", "\"" + item.key + "\" is edited in the plate's settings dialog; it is not writable through MCP yet.",
                 "Set first_layer_sequence_choice / other_layers_sequence_choice to \"auto\", or leave the order to the user.");
        parsed_config.set_deserialize_strict(item.key, item.text);
        if ((item.key == "first_layer_sequence_choice" || item.key == "other_layers_sequence_choice") &&
            parsed_config.opt_enum<LayerSeq>(item.key) != LayerSeq::flsAuto)
            fail("invalid_argument", "\"" + item.key + "\" = customize opens the plate's sequence dialog; only \"auto\" is writable through MCP.");
    }

    ctx.snapshot("plate " + std::to_string(plate_idx + 1) + " settings");
    DynamicPrintConfig& config = *plate->config();
    const DynamicPrintConfig before = config;
    // The setters the plate tab uses (TabPrintPlate::on_value_change).
    for (const Parsed& item : parsed) {
        const std::string& k = item.key;
        if (k == "curr_bed_type")
            plate->set_bed_type(parsed_config.opt_enum<BedType>(k));
        else if (k == "skirt_start_angle")
            config.set_key_value(k, new ConfigOptionFloat(parsed_config.opt_float(k)));
        else if (k == "print_sequence")
            plate->set_print_seq(parsed_config.opt_enum<PrintSequence>(k));
        else if (k == "first_layer_sequence_choice") {
            config.set_key_value(k, parsed_config.option(k)->clone());
            plate->set_first_layer_print_sequence({});
        } else if (k == "other_layers_sequence_choice") {
            config.set_key_value(k, parsed_config.option(k)->clone());
            plate->set_other_layers_print_sequence({});
        } else if (k == "spiral_mode")
            plate->set_spiral_vase_mode(parsed_config.opt_bool(k), false);
    }
    for (const std::string& k : resets) {
        if (k == "curr_bed_type")
            plate->reset_bed_type();
        else if (k == "skirt_start_angle")
            config.erase(k);
        else if (k == "print_sequence")
            plate->set_print_seq(PrintSequence::ByDefault);
        else if (k == "first_layer_sequence_choice" || k == "first_layer_print_sequence") {
            config.erase("first_layer_sequence_choice");
            plate->set_first_layer_print_sequence({});
        } else if (k == "other_layers_sequence_choice" || k == "other_layers_print_sequence") {
            config.erase("other_layers_sequence_choice");
            plate->set_other_layers_print_sequence({});
        } else if (k == "spiral_mode")
            plate->set_spiral_vase_mode(false, true);
    }
    if (Tab* plate_tab = wxGetApp().get_plate_tab(); plate_tab != nullptr)
        if (auto* model_tab = dynamic_cast<TabPrintModel*>(plate_tab); model_tab != nullptr)
            model_tab->update_model_config();
    if (ParamsPanel* panel = wxGetApp().params_panel(); panel != nullptr)
        panel->notify_object_config_changed();
    Refresh::full();
    ctx.set_slice_invalidated();

    json changed = json::array();
    std::set<std::string> keys;
    for (const Parsed& item : parsed)
        keys.insert(item.key);
    for (const std::string& key : resets)
        keys.insert(key);
    for (const std::string& key : keys)
        changed.push_back({{"key", key}, {"old", value_json(before, key)}, {"new", value_json(config, key)}});
    json out = json::object();
    out["ok"] = true;
    out["scope"] = {{"plate", plate_idx + 1}};
    out["changed"] = changed;
    out["plate"] = Scene::plate_json(plate_idx);
    return out;
}

// --- model scopes ---------------------------------------------------------------------------------

json set_model_scope(ToolContext& ctx, const json& scope, const json& values, const json& reset)
{
    json out = json::object();
    if (scope.contains("object")) {
        const ObjectRef ref = Ids::object(scope["object"].get<std::string>(), "scope.object");
        const DynamicPrintConfig reference = full_config();
        parse_values(values, Where::Object, &reference);
        ctx.snapshot("settings of " + Ids::of(*ref.object));
        out["changed"] = apply_to_model_config(ref.object->config, Where::Object, values, reset);
        refresh_after_model_config(ref.obj_idx, ref.object, nullptr);
        out["scope"] = {{"object", Ids::of(*ref.object)}};
        out["object"] = Scene::object_json(ref.obj_idx, Scene::Detail::Normal);
    } else if (scope.contains("volume")) {
        const VolumeRef ref = Ids::volume(scope["volume"].get<std::string>(), "scope.volume");
        Where where;
        if (ref.volume->is_model_part())
            where = Where::Part;
        else if (ref.volume->is_modifier())
            where = Where::Modifier;
        else
            fail("invalid_argument", Ids::of(*ref.volume) + " is a " + Scene::volume_type_name(*ref.volume) + "; only parts and modifiers carry settings.",
                 "Change its type with modifier_update, or add a modifier with modifier_add.");
        const DynamicPrintConfig reference = full_config();
        parse_values(values, where, &reference);
        for (const std::string& key : reset_keys(reset))
            if (key == "extruder")
                fail("invalid_argument", "Reset the filament of a body with modifier_update filament 0.");
        ctx.snapshot("settings of " + Ids::of(*ref.volume));
        out["changed"] = apply_to_model_config(ref.volume->config, where, values, reset);
        refresh_after_model_config(ref.obj_idx, ref.object, ref.volume);
        out["scope"] = {{"volume", Ids::of(*ref.volume)}};
        out["object"] = Scene::object_json(ref.obj_idx, Scene::Detail::Normal);
    } else {
        const json&     range = scope["layer_range"];
        const ObjectRef ref = Ids::object(range["object"].get<std::string>(), "scope.layer_range.object");
        const double    z_min = range["z_min"].get<double>(), z_max = range["z_max"].get<double>();
        auto            it = ref.object->layer_config_ranges.begin();
        for (; it != ref.object->layer_config_ranges.end(); ++it)
            if (std::abs(it->first.first - z_min) < 1e-4 && std::abs(it->first.second - z_max) < 1e-4)
                break;
        if (it == ref.object->layer_config_ranges.end())
            fail("invalid_argument", "Object " + Ids::of(*ref.object) + " has no height range " + json(z_min).dump() + "-" + json(z_max).dump() + ".",
                 "layer_range action list shows the ranges.");
        for (const std::string& key : reset_keys(reset))
            if (key == "layer_height" || key == "extruder")
                fail("invalid_argument", "A height range always keeps its \"" + key + "\"; set another value instead.");
        const DynamicPrintConfig reference = full_config();
        parse_values(values, Where::LayerRange, &reference);
        ctx.snapshot("settings of a height range of " + Ids::of(*ref.object));
        out["changed"] = apply_to_model_config(it->second, Where::LayerRange, values, reset);
        refresh_after_layer_ranges(ref.obj_idx);
        Refresh::object(ref.obj_idx);
        out["scope"] = {{"layer_range", {{"object", Ids::of(*ref.object)}, {"z_min", z_min}, {"z_max", z_max}}}};
        out["object"] = Scene::object_json(ref.obj_idx, Scene::Detail::Normal);
    }
    ctx.set_slice_invalidated();
    out["ok"] = true;
    return out;
}

json settings_set(ToolContext& ctx)
{
    const json&                    scope = ctx.args()["scope"];
    const json                     values = ctx.has("values") ? ctx.args()["values"] : json(nullptr);
    const json                     reset = ctx.has("reset") ? ctx.args()["reset"] : json(nullptr);
    const std::vector<std::string> resets = reset_keys(reset);
    if ((values.is_null() || values.empty()) && resets.empty())
        fail("invalid_argument", "Nothing to do: give `values` and/or `reset`.");
    check_overlap(values, resets);

    if (scope.is_string()) {
        const std::string name = scope.get<std::string>();
        if (name == "project")
            return set_project_scope(ctx, values, resets);
        return set_preset_scope(ctx, PresetOps::type_from(name), 0, values, resets);
    }
    if (scope.contains("filament"))
        return set_preset_scope(ctx, Preset::TYPE_FILAMENT, scope["filament"].get<int>(), values, resets);
    if (scope.contains("plate"))
        return set_plate_scope(ctx, Ids::plate(scope["plate"], "scope.plate"), values, resets);
    return set_model_scope(ctx, scope, values, reset);
}

// Test only: what the GUI shows, to check that a change reached the window. The object list rows
// of every object (type, name, children), the preset selectors' texts (with the "(modified)"
// marker), whether a row shows the "has overrides" icon, and the filament colours the plater renders with.
json list_item_json(ObjectDataViewModel& model, const wxDataViewItem& item, int depth)
{
    static const std::vector<std::pair<int, const char*>> types = {{itPlate, "plate"},       {itObject, "object"},   {itVolume, "volume"},
                                                                   {itInstanceRoot, "instances"}, {itInstance, "instance"}, {itSettings, "settings"},
                                                                   {itLayerRoot, "layers"},   {itLayer, "layer"},     {itInfo, "info"}};
    json out = json::object();
    const int type = static_cast<int>(model.GetItemType(item));
    for (const auto& [bit, name] : types)
        if (type & bit)
            out["type"] = name;
    out["name"] = std::string(model.GetName(item).utf8_str().data());
    if (const auto* node = static_cast<const ObjectDataViewModelNode*>(item.GetID()); node != nullptr) {
        // Orca marks items with setting overrides by an icon in the row (NEW_OBJECT_SETTING), not a child row.
        out["overrides_icon"] = node->IsActionEnabled();
        const wxString extruder = const_cast<ObjectDataViewModelNode*>(node)->GetExtruder();
        if (!extruder.empty())
            out["filament_column"] = std::string(extruder.utf8_str().data());
    }
    if (depth < 3) {
        wxDataViewItemArray children;
        model.GetChildren(item, children);
        if (!children.IsEmpty()) {
            json list = json::array();
            for (const wxDataViewItem& child : children)
                list.push_back(list_item_json(model, child, depth + 1));
            out["children"] = std::move(list);
        }
    }
    return out;
}

json debug_settings_ui(ToolContext& ctx)
{
    (void) ctx;
    json out = json::object();
    ObjectList* list = wxGetApp().obj_list();
    json objects = json::array();
    if (list != nullptr && list->GetModel() != nullptr) {
        for (int i = 0; i < static_cast<int>(plater().model().objects.size()); ++i) {
            const wxDataViewItem item = list->GetModel()->GetItemById(i);
            if (item.IsOk())
                objects.push_back(list_item_json(*list->GetModel(), item, 0));
        }
    }
    out["object_list"] = std::move(objects);
    json selectors = json::object();
    for (Preset::Type type : {Preset::TYPE_PRINT, Preset::TYPE_PRINTER, Preset::TYPE_FILAMENT})
        if (Tab* tab = wxGetApp().get_tab(type); tab != nullptr && tab->get_combo_box() != nullptr)
        {
            TabPresetComboBox* combo = tab->get_combo_box();
            const int          item = combo->GetSelection();
            const wxString     text = item >= 0 ? combo->GetString(static_cast<unsigned>(item)) : combo->GetValue();
            selectors[PresetOps::type_name(type)] = std::string(text.utf8_str().data());
        }
    out["tab_selectors"] = std::move(selectors);
    json slots = json::array();
    for (PlaterPresetComboBox* combo : plater().sidebar().combos_filament())
        slots.push_back(combo != nullptr && combo->GetSelection() >= 0 ? std::string(combo->GetString(static_cast<unsigned>(combo->GetSelection())).utf8_str().data())
                                                                       : std::string());
    out["sidebar_filaments"] = std::move(slots);
    out["render_colors"] = plater().get_extruder_colors_from_plater_config();
    out["project_dirty"] = plater().is_project_dirty();
    return out;
}

} // namespace

MCP_TOOL_GROUP(SettingsEdit)
{
    {
        ToolDef tool;
        tool.name = "debug_settings_ui";
        tool.title = "Test only: what the GUI shows";
        tool.description = "Test only. Object list rows, preset selector texts, sidebar filament entries and render colours.";
        tool.input_schema = Schema::parse(R"JSON({"type": "object", "properties": {}, "additionalProperties": false})JSON", "debug_settings_ui");
        tool.debug_only = true;
        tool.read_only = true;
        tool.handler = debug_settings_ui;
        registry.add(std::move(tool));
    }
    ToolDef tool;
    tool.name = "settings_set";
    tool.title = "Set or reset settings at any scope";
    tool.description =
        "Set or reset print settings at one scope, several keys at once (validated together; nothing is written when one is wrong). "
        "scope: \"process\" | \"printer\" | {\"filament\": slot} edit the selected preset as the settings tabs do (it becomes modified, not "
        "saved; dependent options adjust, reported in side_effects; auto_adjust answers the app's \"adjust automatically?\" questions); "
        "\"project\" (project-level values such as flush_volumes_matrix); {\"plate\": n} (curr_bed_type, print_sequence, spiral_mode, "
        "skirt_start_angle, *_sequence_choice auto); {\"object\": id} | {\"volume\": id} (part or modifier) | {\"layer_range\": {object, "
        "z_min, z_max}} override the global value for that item, as the object list's settings do. Narrower scopes win: object < part/"
        "modifier, layer range applies to parts. `values` {key: value}: numbers, booleans, strings, enums as their value string, percentages "
        "\"15%\", arrays (or one value for every entry) for per-filament/extruder options. `reset`: keys back to the saved preset value "
        "(preset scopes) or remove the override (other scopes). Errors: unknown_key (nearest keys), key_not_in_scope (lists the scopes that "
        "take the key), invalid_value (lists accepted values), out_of_range (min/max), forbidden_key (post-processing, print host, plugin, and "
        "G-code text unless the user allowed it). settings_search finds keys. Object/part/range/plate changes are one undo step; preset and "
        "project changes go to the journal (history revert). Returns changed [{key, old, new}] and side_effects.";
    tool.input_schema = Schema::parse(R"JSON({
        "type": "object",
        "properties": {
            "scope": {"$ref": "#/$defs/Scope"},
            "values": {"type": "object", "description": "key -> value. Numbers, booleans, strings; percentages as \"15%\"; arrays for per-filament or per-extruder options (a single value is applied to every entry)."},
            "reset": {"type": "array", "items": {"type": "string"}, "description": "Keys to reset: at preset scopes back to the saved preset value, at object/part/layer/plate scopes remove the override."},
            "auto_adjust": {"type": "boolean", "default": true, "description": "Accept the dependent changes the app proposes (reported in side_effects)."}
        },
        "required": ["scope"],
        "additionalProperties": false})JSON", "settings_set");
    tool.idempotent = true;
    tool.handler = settings_set;
    registry.add(std::move(tool));
}

}}} // namespace Slic3r::GUI::Mcp
