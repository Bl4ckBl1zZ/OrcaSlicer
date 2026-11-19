// Presets: preset_select, preset_save (save / discard), preset_delete, preset_rename, preset_diff.
//
// Selection goes through the GUI's own selector path (PresetOps::select): the process tab's
// select_preset, or the sidebar's printer / filament-slot dropdown handler, so everything the GUI does
// on a selection happens. The "unsaved changes" dialog never opens: the tool decides from `on_unsaved`
// before switching. Preset changes are outside the undo stack; each is recorded in the journal.
// System presets are never modified.

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Tab.hpp"

#include "../McpScene.hpp"
#include "../McpSchema.hpp"
#include "../McpTool.hpp"
#include "settings/PresetOps.hpp"

namespace Slic3r { namespace GUI { namespace Mcp {

namespace {

using namespace PresetOps;

PresetBundle& bundle() { return *wxGetApp().preset_bundle; }

json scope_json(Preset::Type type, int slot)
{
    if (type == Preset::TYPE_FILAMENT && slot > 0)
        return json{{"filament", slot}};
    return type_name(type);
}

// The serialised edits of the edited preset, for re-applying them later.
std::vector<std::pair<std::string, std::string>> dirty_serialized(Preset::Type type)
{
    std::vector<std::pair<std::string, std::string>> out;
    PresetCollection&                                c = collection(type);
    const DynamicPrintConfig&                        edited = c.get_edited_preset().config;
    for (const std::string& key : c.current_dirty_options())
        if (edited.has(key))
            out.emplace_back(key, edited.opt_serialize(key));
    return out;
}

void reapply(Preset::Type type, const std::vector<std::pair<std::string, std::string>>& values)
{
    if (!values.empty())
        tab(type).apply_preset_config_values(values);
}

json state_json(Preset::Type type)
{
    PresetCollection& c = collection(type);
    json out = json::object();
    out["type"] = type_name(type);
    out["selected"] = c.get_selected_preset().name;
    out["modified"] = c.current_is_dirty();
    if (c.current_is_dirty())
        out["modified_keys"] = c.current_dirty_options();
    return out;
}

// --- preset_select --------------------------------------------------------------------------------

json do_select(ToolContext& ctx, const SelectRequest& request)
{
    const SelectOutcome outcome = select(request);
    json out = json::object();
    out["ok"] = true;
    out["type"] = type_name(request.type);
    if (request.type == Preset::TYPE_FILAMENT)
        out["slot"] = request.slot;
    out["before"] = outcome.before;
    out["selected"] = outcome.after;
    out["changed"] = outcome.changed;
    if (!outcome.discarded.empty())
        out["discarded"] = outcome.discarded;
    if (!outcome.carried_over.empty())
        out["carried_over"] = outcome.carried_over;
    if (!outcome.rejected.empty())
        out["rejected_keys"] = outcome.rejected;
    if (!outcome.saved_as.is_null())
        out["saved_as"] = outcome.saved_as;
    for (const json& note : outcome.notes)
        ctx.note(note.get<std::string>());
    if (!outcome.changed) {
        ctx.note("That preset was already selected; nothing changed.");
    } else {
        SelectRequest back;
        back.type = request.type;
        back.slot = request.slot;
        back.name = outcome.before;
        back.on_unsaved = OnUnsaved::Fail;
        json before = json::object();
        before["preset"] = outcome.before;
        if (!outcome.discarded.empty())
            before["discarded_edits"] = outcome.discarded;
        ctx.journal(scope_json(request.type, request.slot), "preset", before, outcome.after, [back] { select(back); });
        ctx.set_slice_invalidated();
    }
    out["presets"] = {state_json(Preset::TYPE_PRINTER), state_json(Preset::TYPE_PRINT), state_json(Preset::TYPE_FILAMENT)};
    out["filaments"] = Scene::filaments_json();
    out["scene"] = Scene::summary_json();
    return out;
}

json preset_select(ToolContext& ctx)
{
    SelectRequest request;
    request.type = type_from(ctx.arg("type", ""));
    request.name = ctx.arg("name", "");
    request.on_unsaved = on_unsaved_from(ctx.arg("on_unsaved", "fail"));
    request.save_as = ctx.arg("save_as", "");
    if (request.type == Preset::TYPE_FILAMENT) {
        if (!ctx.has("slot"))
            fail("invalid_argument", "A filament preset is selected for a slot: give `slot` (1-based).", "filament_list shows the slots.");
        request.slot = ctx.arg<int>("slot", 0);
    } else if (ctx.has("slot")) {
        fail("invalid_argument", "`slot` applies to type filament only.");
    }
    if (request.on_unsaved == OnUnsaved::Save && ctx.has("save_as") && request.save_as.empty())
        fail("invalid_argument", "`save_as` must not be empty.");
    if (request.on_unsaved != OnUnsaved::Save && ctx.has("save_as"))
        fail("invalid_argument", "`save_as` is used with on_unsaved \"save\" only.");
    return do_select(ctx, request);
}

// --- preset_save ----------------------------------------------------------------------------------

void check_tab_slot(Preset::Type type, ToolContext& ctx)
{
    if (type != Preset::TYPE_FILAMENT || !ctx.has("slot"))
        return;
    const int     slot = ctx.arg<int>("slot", 0);
    PresetBundle& b = bundle();
    if (slot < 1 || static_cast<size_t>(slot) > b.filament_presets.size())
        fail("invalid_argument", "`slot`: there is no filament slot " + std::to_string(slot) + ".");
    const Preset* slot_preset = b.filaments.find_preset(b.filament_presets[static_cast<size_t>(slot) - 1], false);
    if (slot_preset == nullptr || slot_preset->name != b.filaments.get_edited_preset().name)
        fail("nothing_modified",
             "The filament tab edits \"" + b.filaments.get_edited_preset().name + "\", not the preset of slot " + std::to_string(slot) + ".",
             "Only the preset in the filament tab can hold unsaved edits; preset_diff type filament shows them.");
}

json preset_save(ToolContext& ctx)
{
    const Preset::Type type = type_from(ctx.arg("type", ""));
    const std::string  action = ctx.arg("action", "save");
    PresetCollection&  c = collection(type);
    check_tab_slot(type, ctx);
    Preset&            edited = c.get_edited_preset();
    const std::string  current = edited.name;
    json out = json::object();
    out["ok"] = true;
    out["type"] = type_name(type);

    if (action == "discard") {
        if (ctx.has("name") || ctx.has("overwrite"))
            fail("invalid_argument", "action \"discard\" takes no `name` or `overwrite`.");
        if (!c.current_is_dirty())
            fail("nothing_modified", "The " + std::string(type_name(type)) + " preset \"" + current + "\" has no unsaved edits.");
        const json values = dirty_values(type);
        const auto serialized = dirty_serialized(type);
        discard_edits(type);
        ctx.journal(scope_json(type, 0), "discard", values, nullptr, [type, serialized, current] {
            if (collection(type).get_edited_preset().name != current)
                fail("conflict", "The " + std::string(type_name(type)) + " preset is no longer \"" + current + "\"; select it first.");
            reapply(type, serialized);
        });
        ctx.set_slice_invalidated();
        out["discarded"] = values;
        out["preset"] = state_json(type);
        return out;
    }

    // save
    std::string name = ctx.arg("name", "");
    const bool  overwrite = ctx.arg<bool>("overwrite", false);
    if (name.empty()) {
        if (edited.is_system || edited.is_default)
            fail("system_preset", "\"" + current + "\" is a system preset and cannot be overwritten.",
                 "Give `name` to save the edits as a new user preset.");
        name = current;
    }
    if (name == current && !c.current_is_dirty())
        fail("nothing_modified", "\"" + current + "\" has no unsaved edits.", "Give a new `name` to save a copy.");
    if (const Preset* existing = c.find_preset(name, false); existing != nullptr) {
        if (existing->is_system || existing->is_default)
            fail("name_exists", "\"" + name + "\" is a system preset; it cannot be overwritten.", "Choose another name.");
        if (!overwrite && name != current)
            fail("name_exists", "A " + std::string(type_name(type)) + " preset \"" + name + "\" exists already.",
                 "Choose another name, or set overwrite: true to replace it.");
    }
    if (name.find_first_of("<>:\"/\\|?*") != std::string::npos || name.empty())
        fail("invalid_argument", "`name` must not contain any of < > : \" / \\ | ? *.");
    const json saved_keys = c.current_is_dirty() ? json(c.current_dirty_options()) : json::array();
    tab(type).save_preset(name);
    if (c.get_selected_preset().name != name || c.current_is_dirty())
        fail("internal", "Saving \"" + name + "\" did not complete.", "Check the application window for a message.");
    ctx.journal(scope_json(type, 0), "save", current, name, nullptr);
    out["name"] = name;
    out["saved_keys"] = saved_keys;
    out["replaced"] = name == current || overwrite;
    out["preset"] = state_json(type);
    return out;
}

// --- preset_delete / preset_rename ---------------------------------------------------------------

const Preset& user_preset(Preset::Type type, const std::string& name)
{
    PresetCollection& c = collection(type);
    const Preset*     preset = c.find_preset(name, false);
    if (preset == nullptr)
        preset = c.find_preset(c.get_preset_name_by_alias(name), false);
    if (preset == nullptr)
        fail("unknown_preset", std::string("There is no ") + type_name(type) + " preset \"" + name + "\".", "preset_list lists the presets.");
    if (preset->is_system || preset->is_default || !preset->is_user())
        fail("system_preset", "\"" + preset->name + "\" is not a user preset; system and bundled presets cannot be renamed or deleted.");
    std::vector<std::string> heirs;
    for (const Preset& other : c)
        if (other.inherits() == preset->name)
            heirs.push_back(other.name);
    if (!heirs.empty()) {
        json details = json::object();
        details["inherited_by"] = heirs;
        fail("invalid_argument", "\"" + preset->name + "\" is inherited by other presets.", "Delete or detach those first.", details);
    }
    return *preset;
}

// PresetCollection::delete_preset erases the entry without moving the selection index, so the
// selection would shift to a neighbour: delete, then select the kept preset again by name.
void delete_unselected(Preset::Type type, const std::string& name)
{
    PresetCollection& c = collection(type);
    const std::string keep = c.get_selected_preset().name;
    Preset            copy = *c.find_preset(name, false);
    wxGetApp().delete_preset_from_cloud(copy);
    if (!c.delete_preset(name))
        fail("internal", "The application refused to delete \"" + name + "\".");
    c.select_preset_by_name(keep, true);
    tab(type).load_current_preset();
}

void refresh_preset_lists(Preset::Type type)
{
    bundle().update_compatible(PresetSelectCompatibleType::Never);
    tab(type).update_tab_ui();
    wxGetApp().plater()->sidebar().update_all_preset_comboboxes();
}

json preset_delete(ToolContext& ctx)
{
    const Preset::Type type = type_from(ctx.arg("type", ""));
    PresetCollection&  c = collection(type);
    const std::string  name = user_preset(type, ctx.arg("name", "")).name;
    const bool         selected = c.get_selected_preset().name == name;
    if (type == Preset::TYPE_FILAMENT) {
        const auto& slots = bundle().filament_presets;
        for (size_t i = 0; i < slots.size(); ++i)
            if (slots[i] == name && !(selected && slots.size() == 1))
                fail("invalid_argument", "\"" + name + "\" is used by filament slot " + std::to_string(i + 1) + ".",
                     "Select another preset for that slot first (preset_select type filament).");
    }
    json out = json::object();
    if (selected) {
        if (c.current_is_dirty() && ctx.arg("on_unsaved", "fail") != "discard")
            fail("unsaved_changes", "\"" + name + "\" is selected and has unsaved edits, which deleting it drops.",
                 "Repeat with on_unsaved: \"discard\", or save them under another name first.");
        // Deleting switches to another preset, which may make the edited process / filament presets
        // incompatible: the app would ask about their edits. Decide here instead.
        std::vector<Preset::Type> dependents;
        if (type == Preset::TYPE_PRINTER)
            dependents = {Preset::TYPE_PRINT, Preset::TYPE_FILAMENT};
        else if (type == Preset::TYPE_PRINT)
            dependents = {Preset::TYPE_FILAMENT};
        json discarded = json::object();
        for (Preset::Type dependent : dependents) {
            if (!collection(dependent).current_is_dirty())
                continue;
            if (ctx.arg("on_unsaved", "fail") != "discard") {
                json details = json::object();
                details[type_name(dependent)] = {{"preset", collection(dependent).get_edited_preset().name}, {"modified", dirty_values(dependent)}};
                fail("unsaved_changes",
                     std::string("The ") + type_name(dependent) + " preset has unsaved edits, which the switch after deleting may drop.",
                     "Repeat with on_unsaved: \"discard\", or save them first (preset_save).", details);
            }
            discarded[type_name(dependent)] = dirty_values(dependent);
        }
        if (c.current_is_dirty())
            discarded[type_name(type)] = dirty_values(type);
        for (Preset::Type dependent : dependents)
            if (collection(dependent).current_is_dirty())
                discard_edits(dependent);
        if (!discarded.empty())
            out["discarded"] = discarded;
        // The tab's own delete: it asks for confirmation (answered here), deletes the file and selects
        // the preset's parent or the next visible one.
        ctx.on_dialog({"Delete Preset", "", DialogAnswer::Yes, true});
        ctx.on_dialog({"Delete", "", DialogAnswer::Ok, true});
        c.discard_current_changes();
        tab(type).delete_preset();
    } else {
        if (c.current_is_dirty())
            fail("unsaved_changes", "The selected " + std::string(type_name(type)) + " preset \"" + c.get_selected_preset().name +
                                        "\" has unsaved edits, which deleting another preset would drop.",
                 "Save or discard them first (preset_save).");
        delete_unselected(type, name);
        refresh_preset_lists(type);
    }
    if (c.find_preset(name, false) != nullptr)
        fail("internal", "\"" + name + "\" still exists after deleting it.");
    ctx.journal(scope_json(type, 0), "delete", name, nullptr, nullptr);
    ctx.set_slice_invalidated();
    out["ok"] = true;
    out["deleted"] = name;
    out["preset"] = state_json(type);
    if (selected)
        ctx.note("The deleted preset was selected; the application selected \"" + c.get_selected_preset().name + "\" instead.");
    return out;
}

void rename_preset(Preset::Type type, const std::string& from, const std::string& to)
{
    PresetCollection& c = collection(type);
    const std::string previous = c.get_selected_preset().name;
    const bool        was_selected = previous == from;
    if (!was_selected) {
        SelectRequest request;
        request.type = type;
        request.name = from;
        if (type == Preset::TYPE_FILAMENT) {
            // The tab, not a slot: point it at the preset directly.
            if (c.current_is_dirty())
                fail("unsaved_changes", "The filament tab holds unsaved edits of \"" + previous + "\".", "Save or discard them first.");
            if (!tab(type).select_preset(from))
                fail("dialog_blocked", "Selecting \"" + from + "\" in the filament tab was cancelled.");
        } else {
            select(request);
        }
    }
    // Save under the new name (save_preset re-points filament slots and the sidebar from the old name),
    // then remove the old file.
    tab(type).save_preset(to);
    if (c.get_selected_preset().name != to)
        fail("internal", "Saving \"" + to + "\" did not complete.");
    delete_unselected(type, from);
    refresh_preset_lists(type);
    if (!was_selected) {
        if (type == Preset::TYPE_FILAMENT)
            tab(type).select_preset(previous == from ? to : previous);
        else {
            SelectRequest back;
            back.type = type;
            back.name = previous == from ? to : previous;
            select(back);
        }
    }
}

json preset_rename(ToolContext& ctx)
{
    const Preset::Type type = type_from(ctx.arg("type", ""));
    PresetCollection&  c = collection(type);
    const std::string  from = user_preset(type, ctx.arg("name", "")).name;
    const std::string  to = ctx.arg("new_name", "");
    if (to.empty() || to.find_first_of("<>:\"/\\|?*") != std::string::npos)
        fail("invalid_argument", "`new_name` must not be empty or contain any of < > : \" / \\ | ? *.");
    if (to == from)
        fail("invalid_argument", "`new_name` is the current name.");
    if (c.find_preset(to, false) != nullptr)
        fail("name_exists", "A " + std::string(type_name(type)) + " preset \"" + to + "\" exists already.", "Choose another name.");
    if (c.get_selected_preset().name == from && c.current_is_dirty())
        fail("unsaved_changes", "\"" + from + "\" has unsaved edits; renaming would save them.",
             "Save (preset_save) or discard them first.");
    if (c.get_selected_preset().name != from && c.current_is_dirty())
        fail("unsaved_changes", "The selected " + std::string(type_name(type)) + " preset \"" + c.get_selected_preset().name + "\" has unsaved edits.",
             "Renaming another preset selects it for a moment; save or discard the edits first.");
    rename_preset(type, from, to);
    ctx.journal(scope_json(type, 0), "rename", from, to, [type, from, to] { rename_preset(type, to, from); });
    json out = json::object();
    out["ok"] = true;
    out["renamed"] = {{"from", from}, {"to", to}};
    out["preset"] = state_json(type);
    return out;
}

// --- preset_diff ----------------------------------------------------------------------------------

const DynamicPrintConfig& config_named(Preset::Type type, const std::string& name, std::string& label)
{
    PresetCollection& c = collection(type);
    if (name == "@edited") {
        label = c.get_edited_preset().name + " (with unsaved edits)";
        return c.get_edited_preset().config;
    }
    if (name == "@saved") {
        label = c.get_selected_preset().name;
        return c.get_selected_preset().config;
    }
    const Preset* preset = c.find_preset(name, false);
    if (preset == nullptr)
        preset = c.find_preset(c.get_preset_name_by_alias(name), false);
    if (preset == nullptr)
        fail("unknown_preset", std::string("There is no ") + type_name(type) + " preset \"" + name + "\".", "preset_list lists the presets.");
    label = preset->name;
    return preset->config;
}

json preset_diff(ToolContext& ctx)
{
    const Preset::Type        type = type_from(ctx.arg("type", ""));
    std::string               label_a, label_b;
    const DynamicPrintConfig& a = config_named(type, ctx.arg("a", "@edited"), label_a);
    const DynamicPrintConfig& b = config_named(type, ctx.arg("b", "@saved"), label_b);
    const size_t              limit = ctx.arg<size_t>("limit", 200);
    const std::vector<std::string>& keys = type == Preset::TYPE_PRINT ? Preset::print_options()
                                           : type == Preset::TYPE_FILAMENT ? Preset::filament_options()
                                                                           : Preset::printer_options();
    json differences = json::array();
    size_t total = 0;
    for (const std::string& key : keys) {
        const ConfigOption* va = a.option(key);
        const ConfigOption* vb = b.option(key);
        if (va == nullptr && vb == nullptr)
            continue;
        if (va != nullptr && vb != nullptr && *va == *vb)
            continue;
        ++total;
        if (differences.size() >= limit)
            continue;
        json item = json::object();
        item["key"] = key;
        item["a"] = va != nullptr ? config_value_json(*va) : json(nullptr);
        item["b"] = vb != nullptr ? config_value_json(*vb) : json(nullptr);
        differences.push_back(std::move(item));
    }
    json out = json::object();
    out["a"] = label_a;
    out["b"] = label_b;
    out["count"] = total;
    out["differences"] = std::move(differences);
    if (total > limit)
        out["truncated"] = true;
    return out;
}

} // namespace

MCP_TOOL_GROUP(Presets)
{
    {
        ToolDef tool;
        tool.name = "preset_select";
        tool.title = "Select a printer, process or filament preset";
        tool.description =
            "Select the printer or process preset, or the filament preset of a slot, exactly as picking it in the GUI's selector does "
            "(compatibility rules, object re-centring when the bed changes, flushing volumes; picking a filament also sets the slot's colour "
            "to the preset's default colour, as the GUI does). Names as preset_list or the selector show them. Unsaved edits that the switch "
            "would drop are never asked about: on_unsaved fail (default; returns unsaved_changes with the modified keys), discard, carry_over "
            "(re-apply the edits to the new preset one key at a time; refused keys are returned in rejected_keys; the app does not carry "
            "printer edits, nor filament edits to another filament type) or save (save the edits first, under save_as; required for a system "
            "preset). Edits of dependent presets (a dirty filament or process preset that is incompatible with the new printer/process) are "
            "discarded with a note under discard/carry_over. Outside the undo stack: recorded in the journal; history revert re-selects the "
            "previous preset (discarded edits are in the journal entry's `before`). Returns the selection, what was discarded or carried "
            "over, the filaments and a scene summary.";
        tool.input_schema = Schema::parse(R"JSON({
            "type": "object",
            "properties": {
                "type": {"enum": ["process", "printer", "filament"]},
                "name": {"type": "string", "minLength": 1},
                "slot": {"type": "integer", "minimum": 1, "description": "Filament slot (type filament)."},
                "on_unsaved": {"enum": ["fail", "discard", "carry_over", "save"], "default": "fail"},
                "save_as": {"type": "string", "minLength": 1, "description": "Name to save the edits under (on_unsaved save)."}
            },
            "required": ["type", "name"],
            "additionalProperties": false})JSON", "preset_select");
        tool.destructive = true;
        tool.idempotent = true;
        tool.handler = preset_select;
        registry.add(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "preset_save";
        tool.title = "Save or discard the edits of a preset";
        tool.description =
            "action save: save the edited printer, process or filament preset (the one in its settings tab, with the edits settings_set made) "
            "as a user preset. Without `name` it overwrites the current user preset; a system preset needs a new `name` (system_preset). An "
            "existing name fails with name_exists unless overwrite: true; saving selects the saved preset. action discard: drop the unsaved "
            "edits (nothing_modified when there are none); history revert re-applies them. For a filament, `slot` checks that the tab edits "
            "that slot's preset. Recorded in the journal. Returns the name and the keys saved.";
        tool.input_schema = Schema::parse(R"JSON({
            "type": "object",
            "properties": {
                "type": {"enum": ["process", "printer", "filament"]},
                "action": {"enum": ["save", "discard"], "default": "save"},
                "name": {"type": "string", "minLength": 1, "maxLength": 200},
                "overwrite": {"type": "boolean", "default": false},
                "slot": {"type": "integer", "minimum": 1}
            },
            "required": ["type"],
            "additionalProperties": false})JSON", "preset_save");
        tool.destructive = true;
        tool.handler = preset_save;
        registry.add(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "preset_delete";
        tool.title = "Delete a user preset";
        tool.description =
            "Delete a user preset (printer, process or filament). System and bundled presets cannot be deleted (system_preset); a preset other "
            "presets inherit from, or a filament preset a slot uses, is refused. Deleting the selected preset selects its parent as the GUI "
            "does; its unsaved edits need on_unsaved discard. Not revertible (the file is removed); recorded in the journal.";
        tool.input_schema = Schema::parse(R"JSON({
            "type": "object",
            "properties": {
                "type": {"enum": ["process", "printer", "filament"]},
                "name": {"type": "string", "minLength": 1},
                "on_unsaved": {"enum": ["fail", "discard"], "default": "fail"}
            },
            "required": ["type", "name"],
            "additionalProperties": false})JSON", "preset_delete");
        tool.destructive = true;
        tool.handler = preset_delete;
        registry.add(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "preset_rename";
        tool.title = "Rename a user preset";
        tool.description =
            "Rename a user preset: saves it under new_name (filament slots that used it follow) and removes the old file. Refused for system "
            "presets, existing names, presets inherited by others, and while the preset or the selected one has unsaved edits. The selection "
            "is restored afterwards. Recorded in the journal; history revert renames it back.";
        tool.input_schema = Schema::parse(R"JSON({
            "type": "object",
            "properties": {
                "type": {"enum": ["process", "printer", "filament"]},
                "name": {"type": "string", "minLength": 1},
                "new_name": {"type": "string", "minLength": 1, "maxLength": 200}
            },
            "required": ["type", "name", "new_name"],
            "additionalProperties": false})JSON", "preset_rename");
        tool.destructive = true;
        tool.handler = preset_rename;
        registry.add(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "preset_diff";
        tool.title = "Compare two presets";
        tool.description =
            "Settings that differ between two presets of one type: [{key, a, b}]. `a` and `b` are preset names, or \"@edited\" (the preset in "
            "the settings tab including unsaved edits) and \"@saved\" (the selected preset as saved). Default a=@edited, b=@saved: the unsaved "
            "edits. Read-only.";
        tool.input_schema = Schema::parse(R"JSON({
            "type": "object",
            "properties": {
                "type": {"enum": ["process", "printer", "filament"]},
                "a": {"type": "string", "minLength": 1, "default": "@edited"},
                "b": {"type": "string", "minLength": 1, "default": "@saved"},
                "limit": {"type": "integer", "minimum": 1, "maximum": 1000, "default": 200}
            },
            "required": ["type"],
            "additionalProperties": false})JSON", "preset_diff");
        tool.read_only = true;
        tool.idempotent = true;
        tool.handler = preset_diff;
        registry.add(std::move(tool));
    }
}

}}} // namespace Slic3r::GUI::Mcp
