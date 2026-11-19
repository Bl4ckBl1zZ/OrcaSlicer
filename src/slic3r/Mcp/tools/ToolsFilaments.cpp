// Filaments: filament_edit (add / remove / set_color / auto_flush). Listing is filament_list (core).
//
// The project's filament slots live in PresetBundle::filament_presets and the colours in
// project_config; neither is on the undo stack, so every change is recorded in the journal. The
// changes go through the sidebar's own functions (Sidebar::add_custom_filament, delete_filament,
// PlaterPresetComboBox::sync_colour_config, auto_calc_flushing_volumes), which renumber object,
// part and paint assignments and refresh the sidebar, plates and 3D view as the GUI does.
// The preset of a slot is chosen with preset_select (type filament, slot); per-object and per-part
// filament assignment is object_update / modifier_update. Mixed (blended) slots of this fork are
// reported but read-only.

#include <algorithm>
#include <string>
#include <vector>

#include <wx/colour.h>

#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/libslic3r.h"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/PresetComboBoxes.hpp"

#include "../McpIds.hpp"
#include "../McpScene.hpp"
#include "../McpSchema.hpp"
#include "../McpTool.hpp"
#include "settings/PresetOps.hpp"

namespace Slic3r { namespace GUI { namespace Mcp {

namespace {

PresetBundle& bundle() { return *wxGetApp().preset_bundle; }
Plater&       plater() { return *wxGetApp().plater(); }

std::string upper(std::string text)
{
    for (char& ch : text)
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return text;
}

std::vector<std::string> strings_of(const char* key)
{
    if (const auto* option = bundle().project_config.option<ConfigOptionStrings>(key); option != nullptr)
        return option->values;
    return {};
}

// Where slot `slot` (1-based) is used: objects, parts/modifiers, height ranges and colour paint.
json used_by(int slot)
{
    json objects = json::array(), parts = json::array(), ranges = json::array(), paint = json::array();
    for (const ModelObject* object : plater().model().objects) {
        const int object_filament = object->config.has("extruder") ? object->config.extruder() : 1;
        if (object_filament == slot)
            objects.push_back(Ids::of(*object));
        for (const ModelVolume* volume : object->volumes) {
            if (volume->config.has("extruder") && volume->config.extruder() == slot)
                parts.push_back(Ids::of(*volume));
            const std::vector<size_t> painted = volume->get_extruders_from_multi_material_painting();
            if (std::find(painted.begin(), painted.end(), static_cast<size_t>(slot - 1)) != painted.end())
                paint.push_back(Ids::of(*volume));
        }
        for (const auto& [range, config] : object->layer_config_ranges)
            if (config.has("extruder") && config.get().opt_int("extruder") == slot)
                ranges.push_back({{"object", Ids::of(*object)}, {"z_min", Scene::round4(range.first)}, {"z_max", Scene::round4(range.second)}});
    }
    json out = json::object();
    out["objects"] = std::move(objects);
    out["parts"] = std::move(parts);
    out["layer_ranges"] = std::move(ranges);
    out["paint"] = std::move(paint);
    return out;
}

bool is_used(const json& usage)
{
    for (const char* key : {"objects", "parts", "layer_ranges", "paint"})
        if (!usage[key].empty())
            return true;
    return false;
}

json slot_json(size_t index)
{
    PresetBundle& b = bundle();
    json          item = json::object();
    item["slot"] = index + 1;
    item["preset"] = b.filament_presets[index];
    if (const Preset* preset = b.filaments.find_preset(b.filament_presets[index], false); preset != nullptr) {
        const DynamicPrintConfig& config = preset->config;
        if (const auto* type = config.option<ConfigOptionStrings>("filament_type"); type != nullptr && !type->values.empty())
            item["type"] = type->values.front();
        if (const auto* vendor = config.option<ConfigOptionStrings>("filament_vendor"); vendor != nullptr && !vendor->values.empty())
            item["vendor"] = vendor->values.front();
        if (const auto* temp = config.option<ConfigOptionInts>("nozzle_temperature"); temp != nullptr && !temp->values.empty())
            item["nozzle_temperature"] = temp->values.front();
        item["system"] = preset->is_system;
    }
    const std::vector<std::string> colours = strings_of("filament_colour");
    if (index < colours.size())
        item["color"] = colours[index];
    const std::vector<std::string> multi = strings_of("filament_multi_colour");
    if (index < multi.size() && multi[index].find(' ') != std::string::npos)
        item["colors"] = multi[index];
    if (b.is_mixed_filament(index))
        item["mixed"] = true;
    if (const auto* map = b.project_config.option<ConfigOptionInts>("filament_map"); map != nullptr && index < map->values.size())
        item["extruder"] = map->values[index];
    item["used_by"] = used_by(static_cast<int>(index) + 1);
    return item;
}

json filaments_json()
{
    json out = json::array();
    for (size_t i = 0; i < bundle().filament_presets.size(); ++i)
        out.push_back(slot_json(i));
    return out;
}

int slot_index(ToolContext& ctx, const char* key)
{
    if (!ctx.has(key))
        fail("invalid_argument", std::string("This action needs `") + key + "` (1-based filament slot).");
    const int    slot = ctx.arg<int>(key, 0);
    const size_t count = bundle().filament_presets.size();
    if (slot < 1 || static_cast<size_t>(slot) > count)
        fail("invalid_argument", std::string("`") + key + "`: there is no filament slot " + std::to_string(slot) + ".",
             "The project has " + std::to_string(count) + " filament slot(s).");
    return slot - 1;
}

void refuse_mixed(size_t index)
{
    if (bundle().is_mixed_filament(index))
        fail("invalid_argument", "Filament slot " + std::to_string(index + 1) + " is a mixed-colour slot; mixed filaments are read-only through MCP.",
             "Edit mixed filaments in the application.");
}

// The body of PlaterPresetComboBox::sync_colour_config for one slot, through the slot's own combo.
void set_colour(size_t index, const std::vector<std::string>& colours, bool gradient)
{
    std::vector<PlaterPresetComboBox*>& combos = plater().sidebar().combos_filament();
    if (index >= combos.size() || combos[index] == nullptr)
        fail("invalid_argument", "Filament slot " + std::to_string(index + 1) + " has no colour selector.");
    combos[index]->sync_colour_config(colours, gradient);
}

json filament_edit(ToolContext& ctx)
{
    const std::string action = ctx.arg("action", "");
    PresetBundle&     b = bundle();
    Sidebar&          sidebar = plater().sidebar();
    json              out = json::object();
    auto              unexpected = [&](std::initializer_list<const char*> keys) {
        for (const char* key : keys)
            if (ctx.has(key))
                fail("invalid_argument", std::string("`") + key + "` is not used by action \"" + action + "\".");
    };

    if (action == "add") {
        unexpected({"slot", "replace_with"});
        std::string preset_name;
        if (ctx.has("preset")) {
            // Resolve against slot 1's selector: same printer, same compatibility filter.
            preset_name = PresetOps::resolve(Preset::TYPE_FILAMENT, ctx.arg("preset", ""), 1);
        }
        const size_t count = b.filament_presets.size();
        if (b.num_physical_filaments() >= MAXIMUM_EXTRUDER_NUMBER || count >= MAXIMUM_EXTRUDER_NUMBER)
            fail("limit", "The project already has the maximum of " + std::to_string(MAXIMUM_EXTRUDER_NUMBER) + " filaments.");
        const std::string colour = ctx.has("color") ? upper(ctx.arg("color", "")) : upper(Plater::get_next_color_for_filament().GetAsString(wxC2S_HTML_SYNTAX).ToStdString());
        const size_t insert_at = b.num_physical_filaments();
        sidebar.add_custom_filament(wxColour(colour), preset_name);
        if (b.filament_presets.size() != count + 1)
            fail("internal", "The application did not add a filament slot.", "Check the application window for a message.");
        if (!preset_name.empty() && b.filament_presets[insert_at] != preset_name)
            ctx.note("The new slot uses \"" + b.filament_presets[insert_at] + "\"; the requested preset was not applied.");
        const int slot = static_cast<int>(insert_at) + 1;
        if (insert_at + 1 < b.filament_presets.size())
            ctx.note("The new slot was inserted before the mixed-colour slots, which moved up by one.");
        ctx.journal("filaments", "add", static_cast<int>(count), {{"slot", slot}, {"color", colour}, {"preset", b.filament_presets[insert_at]}},
                    [slot] {
                        if (is_used(used_by(slot)))
                            fail("conflict", "Filament slot " + std::to_string(slot) + " is in use now; remove it with filament_edit and replace_with.");
                        wxGetApp().plater()->sidebar().delete_filament(static_cast<size_t>(slot - 1), -1);
                    });
        out["added"] = slot_json(insert_at);
    } else if (action == "remove") {
        unexpected({"color", "preset"});
        const int index = slot_index(ctx, "slot");
        refuse_mixed(static_cast<size_t>(index));
        if (b.num_physical_filaments() <= 1)
            fail("last_filament", "The project needs at least one filament.");
        int replace = -1;
        if (ctx.has("replace_with")) {
            replace = slot_index(ctx, "replace_with");
            if (replace == index)
                fail("invalid_argument", "`replace_with` must be another slot than the one removed.");
        }
        const json usage = used_by(index + 1);
        if (is_used(usage) && replace < 0) {
            json details = json::object();
            details["used_by"] = usage;
            fail("in_use", "Filament slot " + std::to_string(index + 1) + " is in use.",
                 "Give replace_with: the slot that takes over its objects, parts, ranges and paint.", details);
        }
        if (b.filaments.current_is_dirty() && (index == 0 || b.is_the_only_edited_filament(static_cast<unsigned>(index))))
            fail("unsaved_changes", "The filament tab has unsaved edits of \"" + b.filaments.get_edited_preset().name +
                                        "\", and removing this slot switches the tab.",
                 "Save or discard them first (preset_save type filament).");
        json before = slot_json(static_cast<size_t>(index));
        const size_t count = b.filament_presets.size();
        sidebar.delete_filament(static_cast<size_t>(index), replace);
        if (b.filament_presets.size() != count - 1)
            fail("internal", "The application did not remove the slot.", "Check the application window for a message.");
        if (static_cast<size_t>(index) + 1 < count)
            ctx.note("Slots above " + std::to_string(index + 1) + " moved down by one; their numbers changed.");
        ctx.journal("filaments", "remove", before, nullptr, nullptr);
        out["removed"] = before;
        if (replace >= 0)
            out["replaced_by"] = replace + 1 - (replace > index ? 1 : 0);
        ctx.set_slice_invalidated();
    } else if (action == "set_color") {
        unexpected({"replace_with", "preset"});
        const int index = slot_index(ctx, "slot");
        refuse_mixed(static_cast<size_t>(index));
        if (!ctx.has("color"))
            fail("invalid_argument", "action \"set_color\" needs `color` (#RRGGBB).");
        const std::string              colour = upper(ctx.arg("color", ""));
        const std::vector<std::string> colours = strings_of("filament_colour");
        const std::vector<std::string> multi = strings_of("filament_multi_colour");
        const std::vector<std::string> kinds = strings_of("filament_colour_type");
        const std::string old_colour = static_cast<size_t>(index) < colours.size() ? colours[static_cast<size_t>(index)] : std::string();
        const std::string old_multi = static_cast<size_t>(index) < multi.size() ? multi[static_cast<size_t>(index)] : old_colour;
        const bool        old_gradient = static_cast<size_t>(index) < kinds.size() && kinds[static_cast<size_t>(index)] == "0";
        set_colour(static_cast<size_t>(index), {colour}, false);
        const size_t slot_count = b.filament_presets.size();
        ctx.journal(json{{"filament", index + 1}}, "color", old_colour, colour, [index, old_multi, old_gradient, slot_count] {
            if (wxGetApp().preset_bundle->filament_presets.size() != slot_count)
                fail("conflict", "The filament slots changed since; set the colour again with filament_edit.");
            std::vector<std::string> parts;
            std::string              token;
            for (char ch : old_multi + " ") {
                if (ch == ' ') {
                    if (!token.empty())
                        parts.push_back(token);
                    token.clear();
                } else
                    token += ch;
            }
            if (parts.empty())
                fail("not_revertible", "The old colour is not known.");
            set_colour(static_cast<size_t>(index), parts, old_gradient);
        });
        out["changed"] = slot_json(static_cast<size_t>(index));
    } else if (action == "auto_flush") {
        unexpected({"slot", "replace_with", "color", "preset"});
        const json before = b.project_config.has("flush_volumes_matrix")
                                ? config_value_json(*b.project_config.option("flush_volumes_matrix"))
                                : json(nullptr);
        const std::string before_text = b.project_config.has("flush_volumes_matrix") ? b.project_config.opt_serialize("flush_volumes_matrix") : std::string();
        sidebar.auto_calc_flushing_volumes(-1);
        const json after = b.project_config.has("flush_volumes_matrix") ? config_value_json(*b.project_config.option("flush_volumes_matrix")) : json(nullptr);
        ctx.journal("project", "flush_volumes_matrix", before, after, [before_text] {
            if (before_text.empty())
                fail("not_revertible", "There was no flushing matrix before.");
            PresetBundle&      pb = *wxGetApp().preset_bundle;
            DynamicPrintConfig changed = pb.project_config;
            changed.set_deserialize_strict("flush_volumes_matrix", before_text);
            pb.project_config.apply(changed);
            wxGetApp().plater()->update_project_dirty_from_presets();
            wxGetApp().plater()->on_config_change(changed);
        });
        out["flush_volumes_matrix"] = after;
        ctx.set_slice_invalidated();
    } else {
        fail("invalid_argument", "`action` must be add, remove, set_color or auto_flush.");
    }
    out["ok"] = true;
    out["filaments"] = filaments_json();
    return out;
}

} // namespace

MCP_TOOL_GROUP(Filaments)
{
    {
        ToolDef tool;
        tool.name = "filament_edit";
        tool.title = "Add, remove or recolour filament slots";
        tool.description =
            "Change the project's filament slots, as the sidebar's filament list does. action add {color?: \"#RRGGBB\" (default: the next "
            "colour the app picks), preset?: filament preset for the new slot}: limit at 64 slots. action remove {slot, replace_with?}: a slot "
            "in use by objects, parts, height ranges or colour paint needs replace_with (the slot that takes them over), else in_use; the last "
            "filament cannot be removed (last_filament); slots above move down by one (see notes). action set_color {slot, color}: the slot's "
            "colour, as the colour picker sets it (objects using it are redrawn). action auto_flush: recompute the flushing volumes matrix. "
            "Mixed-colour slots are read-only. The preset of a slot is chosen with preset_select type filament; objects and parts are assigned "
            "to a slot with object_update / modifier_update `filament`. Outside the undo stack: recorded in the journal (history revert: add, "
            "set_color and auto_flush are revertible, remove is not). Returns every slot with its preset, type, colour and where it is used.";
        tool.input_schema = Schema::parse(R"JSON({
            "type": "object",
            "properties": {
                "action": {"enum": ["add", "remove", "set_color", "auto_flush"]},
                "slot": {"type": "integer", "minimum": 1, "description": "1-based slot (remove, set_color)."},
                "color": {"$ref": "#/$defs/Color"},
                "preset": {"type": "string", "minLength": 1, "description": "Filament preset of the new slot (add)."},
                "replace_with": {"type": "integer", "minimum": 1, "description": "Slot that takes over what used the removed slot (remove)."}
            },
            "required": ["action"],
            "additionalProperties": false})JSON", "filament_edit");
        tool.destructive = true;
        tool.handler = filament_edit;
        registry.add(std::move(tool));
    }
}

}}} // namespace Slic3r::GUI::Mcp
