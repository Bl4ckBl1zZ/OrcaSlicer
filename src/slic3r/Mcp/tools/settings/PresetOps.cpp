#include "PresetOps.hpp"

#include <algorithm>

#include <wx/event.h>

#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/PresetComboBoxes.hpp"
#include "slic3r/GUI/Tab.hpp"

#include "../../McpError.hpp"
#include "../../McpScene.hpp"

namespace Slic3r { namespace GUI { namespace Mcp { namespace PresetOps {

namespace {

PresetBundle& bundle() { return *wxGetApp().preset_bundle; }

bool is_marker(PresetComboBox& combo, int item)
{
    const auto marker = reinterpret_cast<size_t>(combo.GetClientData(static_cast<unsigned>(item)));
    return marker >= PresetComboBox::LABEL_ITEM_DISABLED && marker < PresetComboBox::LABEL_ITEM_MAX;
}

std::string utf8(const wxString& text) { return std::string(text.utf8_str().data()); }

// The combo item whose alias (internal name) or label is `name`; -1 when none; -2 when ambiguous.
int combo_item(PresetComboBox& combo, const std::string& name, std::string* internal)
{
    const std::string want = Preset::remove_suffix_modified(name);
    int               found = -1;
    for (int item = 0; item < static_cast<int>(combo.GetCount()); ++item) {
        if (is_marker(combo, item))
            continue;
        const std::string alias = Preset::remove_suffix_modified(utf8(combo.GetItemAlias(item)));
        const std::string label = Preset::remove_suffix_modified(utf8(combo.GetString(static_cast<unsigned>(item))));
        if ((alias.empty() || alias != want) && label != want)
            continue;
        if (found >= 0 && found != item) {
            // Prefer an exact internal-name match over a label match.
            const std::string found_alias = Preset::remove_suffix_modified(utf8(combo.GetItemAlias(found)));
            if (found_alias == want)
                continue;
            if (alias == want) {
                found = item;
                if (internal != nullptr)
                    *internal = alias;
                continue;
            }
            return -2;
        }
        found = item;
        if (internal != nullptr)
            *internal = alias.empty() ? label : alias;
    }
    return found;
}

// The selector item for a preset: by its name, or for a printer, by the printer model entry the
// sidebar shows for system printers (its handler then picks the preset of that model).
int item_for_preset(PresetComboBox& combo, Preset::Type type, const Preset& preset)
{
    std::string internal;
    int         item = combo_item(combo, preset.name, &internal);
    if (item >= 0 || type != Preset::TYPE_PRINTER)
        return item;
    if (!preset.alias.empty() && (item = combo_item(combo, preset.alias, &internal)) >= 0)
        return item;
    if (const auto* model = preset.config.option<ConfigOptionString>("printer_model"); model != nullptr && !model->value.empty())
        item = combo_item(combo, model->value, &internal);
    return item;
}

PresetComboBox* selector(Preset::Type type, int slot)
{
    Sidebar& sidebar = wxGetApp().plater()->sidebar();
    switch (type) {
    case Preset::TYPE_PRINTER: return sidebar.printer_combox();
    case Preset::TYPE_FILAMENT: {
        std::vector<PlaterPresetComboBox*>& combos = sidebar.combos_filament();
        if (slot >= 1 && static_cast<size_t>(slot) <= combos.size())
            return combos[static_cast<size_t>(slot) - 1];
        return nullptr;
    }
    default: return tab(type).get_combo_box();
    }
}

json names_offered(PresetComboBox* combo)
{
    json out = json::array();
    if (combo == nullptr)
        return out;
    for (int item = 0; item < static_cast<int>(combo->GetCount()) && out.size() < 40; ++item)
        if (!is_marker(*combo, item))
            out.push_back(Preset::remove_suffix_modified(utf8(combo->GetString(static_cast<unsigned>(item)))));
    return out;
}

void check_slot(int slot)
{
    const size_t count = bundle().filament_presets.size();
    if (slot < 1 || static_cast<size_t>(slot) > count)
        fail("invalid_argument", "`slot`: there is no filament slot " + std::to_string(slot) + ".",
             "The project has " + std::to_string(count) + " filament slot(s); filament slots are 1-based.");
    if (bundle().is_mixed_filament(static_cast<size_t>(slot) - 1))
        fail("invalid_argument", "Filament slot " + std::to_string(slot) + " is a mixed-colour slot; it has no preset of its own.",
             "Mixed filaments are read-only through MCP; edit them in the application.");
}

// Re-runs the selector's own selection handlers, as if the user picked `item` from the dropdown:
// PlaterPresetComboBox::OnSelect, then the sidebar's handler (Plater::priv::on_select_preset), which
// does everything the GUI does for that selection (slot mapping, object re-centring for a printer
// with another bed, flushing volumes, colour of the slot ...).
void pick_in_selector(PresetComboBox& combo, int item)
{
    combo.SetSelection(item);
    wxCommandEvent event(wxEVT_COMBOBOX, combo.GetId());
    event.SetEventObject(&combo);
    event.SetInt(item);
    combo.GetEventHandler()->ProcessEvent(event);
}

// Whether `collection`'s edited preset is dirty and would be asked about when `type` switches to
// `target`.
struct Affected
{
    Preset::Type type;
    bool         dependent; // not the collection being switched
};

std::vector<Affected> affected_by(const SelectRequest& request, const std::string& target)
{
    std::vector<Affected> out;
    PresetBundle&         b = bundle();
    if (request.type == Preset::TYPE_FILAMENT) {
        // The sidebar only switches the filament tab when there is a single filament.
        if (!wxGetApp().plater()->sidebar().is_multifilament() && b.filaments.current_is_dirty() &&
            b.filaments.get_edited_preset().name != target)
            out.push_back({Preset::TYPE_FILAMENT, false});
        return out;
    }
    PresetCollection& own = collection(request.type);
    if (own.current_is_dirty())
        out.push_back({request.type, false});
    const Preset* new_preset = own.find_preset(target, false);
    if (new_preset == nullptr)
        return out;
    const PresetWithVendorProfile new_with_vendor = own.get_preset_with_vendor_profile(*new_preset);
    if (request.type == Preset::TYPE_PRINT) {
        if (b.filaments.current_is_dirty() &&
            !is_compatible_with_print(b.filaments.get_edited_preset_with_vendor_profile(), new_with_vendor,
                                      b.printers.get_edited_preset_with_vendor_profile()))
            out.push_back({Preset::TYPE_FILAMENT, true});
    } else if (request.type == Preset::TYPE_PRINTER) {
        if (b.prints.current_is_dirty() && !is_compatible_with_printer(b.prints.get_edited_preset_with_vendor_profile(), new_with_vendor))
            out.push_back({Preset::TYPE_PRINT, true});
        if (b.filaments.current_is_dirty() && !is_compatible_with_printer(b.filaments.get_edited_preset_with_vendor_profile(), new_with_vendor))
            out.push_back({Preset::TYPE_FILAMENT, true});
    }
    return out;
}

} // namespace

Preset::Type type_from(const std::string& name)
{
    if (name == "process" || name == "print")
        return Preset::TYPE_PRINT;
    if (name == "printer")
        return Preset::TYPE_PRINTER;
    if (name == "filament")
        return Preset::TYPE_FILAMENT;
    fail("invalid_argument", "`type` must be process, printer or filament (got \"" + name + "\").");
}

const char* type_name(Preset::Type type)
{
    switch (type) {
    case Preset::TYPE_PRINT: return "process";
    case Preset::TYPE_PRINTER: return "printer";
    case Preset::TYPE_FILAMENT: return "filament";
    default: return "preset";
    }
}

PresetCollection& collection(Preset::Type type)
{
    PresetBundle& b = bundle();
    switch (type) {
    case Preset::TYPE_PRINT: return b.prints;
    case Preset::TYPE_FILAMENT: return b.filaments;
    default: return b.printers;
    }
}

Tab& tab(Preset::Type type)
{
    Tab* t = wxGetApp().get_tab(type);
    if (t == nullptr)
        fail("internal", std::string("The ") + type_name(type) + " settings tab is not available.");
    return *t;
}

OnUnsaved on_unsaved_from(const std::string& name)
{
    if (name == "discard")
        return OnUnsaved::Discard;
    if (name == "carry_over")
        return OnUnsaved::CarryOver;
    if (name == "save")
        return OnUnsaved::Save;
    return OnUnsaved::Fail;
}

json dirty_values(Preset::Type type)
{
    PresetCollection& c = collection(type);
    json              out = json::object();
    if (!c.current_is_dirty())
        return out;
    const DynamicPrintConfig& config = c.get_edited_preset().config;
    for (const std::string& key : c.current_dirty_options())
        if (const ConfigOption* option = config.option(key); option != nullptr)
            out[key] = config_value_json(*option);
    return out;
}

std::string resolve(Preset::Type type, const std::string& name, int slot)
{
    if (type == Preset::TYPE_FILAMENT)
        check_slot(slot);
    PresetCollection& c = collection(type);
    PresetComboBox*   combo = selector(type, slot);
    std::string       internal;
    const int         item = combo != nullptr && combo->GetCount() > 0 ? combo_item(*combo, name, &internal) : -1;
    if (item == -2)
        fail("invalid_argument", "The name \"" + name + "\" matches several entries of the " + type_name(type) + " selector.",
             "Use the preset's internal name from preset_list.");
    if (item >= 0) {
        if (const Preset* preset = c.find_preset(internal, false); preset != nullptr)
            return preset->name;
        internal = c.get_preset_name_by_alias(internal);
        if (const Preset* preset = c.find_preset(internal, false); preset != nullptr)
            return preset->name;
    }
    const Preset* preset = c.find_preset(name, false);
    if (preset == nullptr)
        preset = c.find_preset(c.get_preset_name_by_alias(name), false);
    if (preset != nullptr && combo != nullptr && preset->is_visible && item_for_preset(*combo, type, *preset) >= 0)
        return preset->name;
    json details = json::object();
    details["offered"] = names_offered(combo);
    if (preset == nullptr)
        fail("unknown_preset", std::string("There is no ") + type_name(type) + " preset \"" + name + "\".",
             "preset_list lists the presets; details.offered lists what the selector offers now.", details);
    // Exists, but the selector does not offer it: incompatible with the printer (or print), or hidden.
    fail("incompatible",
         std::string("The ") + type_name(type) + " preset \"" + preset->name + "\" is not offered for the current " +
             (type == Preset::TYPE_PRINTER ? "configuration" : "printer") + (preset->is_visible ? "" : " (it is hidden)") + ".",
         "Choose one of details.offered, or select a printer it is made for first.", details);
}

std::string bind_filament_tab(int slot)
{
    check_slot(slot);
    PresetBundle&     b = bundle();
    const std::string target = b.filament_presets[static_cast<size_t>(slot) - 1];
    const Preset*     preset = b.filaments.find_preset(target, false);
    if (preset == nullptr)
        fail("unknown_preset", "Filament slot " + std::to_string(slot) + " names \"" + target + "\", which is not a known filament preset.");
    Tab& filament_tab = tab(Preset::TYPE_FILAMENT);
    if (b.filaments.get_edited_preset().name != preset->name) {
        if (b.filaments.current_is_dirty()) {
            json details = json::object();
            details["edited_preset"] = b.filaments.get_edited_preset().name;
            details["modified"] = dirty_values(Preset::TYPE_FILAMENT);
            fail("unsaved_changes",
                 "The filament settings tab holds unsaved edits of \"" + b.filaments.get_edited_preset().name +
                     "\"; editing slot " + std::to_string(slot) + " (\"" + preset->name + "\") would switch the tab away from them.",
                 "Save them with preset_save (type filament, action save, name), or drop them with action discard, then retry.", details);
        }
        if (!filament_tab.select_preset(preset->name))
            fail("dialog_blocked", "Switching the filament tab to \"" + preset->name + "\" was cancelled.");
    }
    if (TabPresetComboBox* combo = filament_tab.get_combo_box(); combo != nullptr)
        combo->set_filament_idx(slot - 1);
    return preset->name;
}

void discard_edits(Preset::Type type)
{
    PresetCollection& c = collection(type);
    if (!c.current_is_dirty())
        return;
    c.discard_current_changes();
    Tab& t = tab(type);
    t.load_current_preset();
    wxGetApp().plater()->on_config_change(bundle().full_config());
    wxGetApp().plater()->update_project_dirty_from_presets();
}

SelectOutcome select(const SelectRequest& request)
{
    SelectOutcome     out;
    PresetBundle&     b = bundle();
    PresetCollection& c = collection(request.type);
    const std::string target = resolve(request.type, request.name, request.slot);

    out.before = request.type == Preset::TYPE_FILAMENT ? b.filament_presets[static_cast<size_t>(request.slot) - 1] : c.get_selected_preset().name;
    if (request.type == Preset::TYPE_FILAMENT) {
        if (const Preset* p = c.find_preset(out.before, false); p != nullptr)
            out.before = p->name;
    }
    out.after = target;
    if (out.before == target && !(request.type != Preset::TYPE_FILAMENT && c.current_is_dirty() && request.on_unsaved == OnUnsaved::Discard)) {
        out.changed = false;
        return out;
    }

    // 1. Unsaved edits the switch would ask about.
    const std::vector<Affected> affected = affected_by(request, target);
    if (!affected.empty() && request.on_unsaved == OnUnsaved::Fail) {
        json details = json::object();
        for (const Affected& a : affected) {
            json entry = json::object();
            entry["preset"] = collection(a.type).get_edited_preset().name;
            entry["modified"] = dirty_values(a.type);
            entry["reason"] = a.dependent ? "incompatible with the new selection, so the app would drop or ask about these edits"
                                          : "unsaved edits of the preset being switched";
            details[type_name(a.type)] = std::move(entry);
        }
        fail("unsaved_changes", std::string("Switching the ") + type_name(request.type) + " preset would drop unsaved edits.",
             "Repeat with on_unsaved: \"discard\" (drop them), \"carry_over\" (re-apply them to the new preset), or \"save\" with save_as "
             "(keep them in a new preset first).",
             details);
    }
    if (request.on_unsaved == OnUnsaved::CarryOver) {
        for (const Affected& a : affected) {
            if (a.dependent)
                continue;
            if (a.type == Preset::TYPE_PRINTER)
                fail("invalid_argument", "The app does not carry printer edits over to another printer preset.",
                     "Use on_unsaved \"discard\" or \"save\".");
            if (a.type == Preset::TYPE_FILAMENT) {
                const Preset* from = &b.filaments.get_edited_preset();
                const Preset* to = b.filaments.find_preset(target, false);
                auto          type_of = [](const Preset* p) {
                    const auto* opt = p == nullptr ? nullptr : p->config.option<ConfigOptionStrings>("filament_type");
                    return opt != nullptr && !opt->values.empty() ? opt->values.front() : std::string();
                };
                if (type_of(from) != type_of(to))
                    fail("invalid_argument", "The app does not carry filament edits over to a filament of another type (" + type_of(from) +
                                                 " -> " + type_of(to) + ").",
                         "Use on_unsaved \"discard\" or \"save\".");
            }
        }
    }
    if (request.on_unsaved == OnUnsaved::Save) {
        for (const Affected& a : affected) {
            if (a.dependent) {
                fail("unsaved_changes",
                     std::string("The ") + type_name(a.type) + " preset \"" + collection(a.type).get_edited_preset().name +
                         "\" has unsaved edits and is not compatible with the new selection.",
                     std::string("Save or discard them first with preset_save type ") + type_name(a.type) + ".");
            }
        }
        bool own_dirty = false;
        for (const Affected& a : affected)
            own_dirty = own_dirty || !a.dependent;
        if (own_dirty) {
            const Preset& edited = collection(request.type).get_edited_preset();
            if (request.save_as.empty() && (edited.is_system || edited.is_default))
                fail("system_preset", "\"" + edited.name + "\" is a system preset; its edits can only be saved under a new name.",
                     "Give save_as.");
            const std::string name = request.save_as.empty() ? edited.name : request.save_as;
            if (const Preset* existing = collection(request.type).find_preset(name, false);
                existing != nullptr && name != edited.name && (existing->is_system || existing->is_default))
                fail("name_exists", "\"" + name + "\" is the name of a system preset.", "Choose another save_as.");
        }
    }

    // 2. Act on the policy.
    std::vector<std::pair<std::string, std::string>> carried_text;
    for (const Affected& a : affected) {
        const json values = dirty_values(a.type);
        if (request.on_unsaved == OnUnsaved::Save && !a.dependent) {
            Preset&           edited = collection(a.type).get_edited_preset();
            const std::string name = request.save_as.empty() ? edited.name : request.save_as;
            tab(a.type).save_preset(name);
            if (collection(a.type).current_is_dirty())
                fail("internal", "Saving \"" + name + "\" did not complete.");
            out.saved_as = name;
            continue;
        }
        if (request.on_unsaved == OnUnsaved::CarryOver && !a.dependent) {
            PresetCollection&         own = collection(a.type);
            const DynamicPrintConfig& edited = own.get_edited_preset().config;
            const std::vector<std::string>& options = a.type == Preset::TYPE_PRINT ? Preset::print_options() : Preset::filament_options();
            for (const std::string& key : own.current_dirty_options())
                if (edited.has(key) && std::find(options.begin(), options.end(), key) != options.end())
                    carried_text.emplace_back(key, edited.opt_serialize(key));
        }
        out.discarded[type_name(a.type)] = values;
        collection(a.type).discard_current_changes();
        if (a.dependent)
            out.notes.push_back(std::string("Discarded the unsaved edits of the ") + type_name(a.type) +
                                " preset: they do not fit the new selection.");
    }

    // 3. Switch, through the same path as the GUI.
    if (request.type == Preset::TYPE_PRINT) {
        Tab& t = tab(request.type);
        if (!t.select_preset(target))
            fail("dialog_blocked", "Selecting the process preset \"" + target + "\" was cancelled.");
        wxGetApp().plater()->on_config_change(b.full_config());
    } else {
        PresetComboBox* combo = selector(request.type, request.slot);
        const Preset*   target_preset = c.find_preset(target, false);
        const int       item = combo != nullptr && target_preset != nullptr ? item_for_preset(*combo, request.type, *target_preset) : -1;
        if (item < 0)
            fail("incompatible", std::string("The ") + type_name(request.type) + " selector no longer offers \"" + target + "\".");
        pick_in_selector(*combo, item);
    }

    // 4. Verify.
    std::string now = request.type == Preset::TYPE_FILAMENT ? b.filament_presets[static_cast<size_t>(request.slot) - 1] : c.get_selected_preset().name;
    if (const Preset* p = c.find_preset(now, false); p != nullptr)
        now = p->name;
    if (now != target)
        fail("internal", std::string("The ") + type_name(request.type) + " preset is \"" + now + "\" after selecting \"" + target + "\".",
             "Check the application's window; history lists what changed.");
    out.changed = true;

    // 5. Carry the edits over: one key at a time, so a key the new preset refuses is reported, not fatal.
    if (!carried_text.empty()) {
        Tab& t = tab(request.type);
        for (const auto& [key, text] : carried_text) {
            try {
                t.apply_preset_config_values({{key, text}});
                out.carried_over.push_back(key);
            } catch (const std::exception& e) {
                json rejected = json::object();
                rejected["key"] = key;
                rejected["reason"] = e.what();
                out.rejected.push_back(std::move(rejected));
            }
        }
    }
    return out;
}

}}}} // namespace Slic3r::GUI::Mcp::PresetOps
