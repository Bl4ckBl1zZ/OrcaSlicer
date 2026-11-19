#include "LoadGuard.hpp"

#include <utility>

#include "libslic3r/CustomGCode.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Tab.hpp"

#include "../settings/PresetOps.hpp"

namespace Slic3r { namespace GUI { namespace Mcp { namespace LoadGuard {

namespace {

bool ends_with(const std::string& text, const char* suffix)
{
    const std::string s(suffix);
    return text.size() > s.size() && text.compare(text.size() - s.size(), s.size(), s) == 0;
}

const std::set<std::string>& names_of(const Library& library, Preset::Type type)
{
    return type == Preset::TYPE_PRINT ? library.print : type == Preset::TYPE_FILAMENT ? library.filament : library.printer;
}

std::set<std::string>& names_of(Library& library, Preset::Type type)
{
    return type == Preset::TYPE_PRINT ? library.print : type == Preset::TYPE_FILAMENT ? library.filament : library.printer;
}

// A preset of the user's library (before the load), not one a project file brought.
bool from_library(const Preset& preset, const std::set<std::string>& names)
{
    return !preset.is_project_embedded && !preset.is_external && names.count(preset.name) != 0;
}

// The config a preset's values must match: the preset itself when it is the user's, else its nearest
// ancestor that is; nullptr (= the defaults) when none is.
const DynamicPrintConfig* reference(const PresetCollection& collection, const Preset& preset, const std::set<std::string>& names)
{
    const Preset* current = &preset;
    for (int depth = 0; current != nullptr && depth < 32; ++depth) {
        if (from_library(*current, names))
            return &current->config;
        current = collection.get_preset_parent(*current);
    }
    return nullptr;
}

// `key` of `config` serialised; the default value when `config` is null or lacks the key.
std::string value_text(const DynamicPrintConfig* config, const std::string& key)
{
    const ConfigOption* option = config != nullptr ? config->option(key) : nullptr;
    if (option == nullptr) {
        const ConfigOptionDef* def = print_config_def.get(key);
        if (def != nullptr && def->default_value)
            option = def->default_value.get();
    }
    return option != nullptr ? option->serialize() : std::string();
}

// One preset in use: the saved preset (as in the collection) and the values in effect (the edited
// copy for the selected preset).
struct InUse
{
    Preset::Type type;
    int          slot; // filament slot, 1-based; 0 otherwise
    Preset*      saved;
    Preset*      effective;
};

std::vector<InUse> presets_in_use()
{
    std::vector<InUse> out;
    for (Preset::Type type : {Preset::TYPE_PRINT, Preset::TYPE_PRINTER}) {
        PresetCollection& collection = PresetOps::collection(type);
        out.push_back({type, 0, &collection.get_selected_preset(), &collection.get_edited_preset()});
    }
    PresetBundle*     bundle = wxGetApp().preset_bundle;
    PresetCollection& filaments = PresetOps::collection(Preset::TYPE_FILAMENT);
    for (size_t i = 0; bundle != nullptr && i < bundle->filament_presets.size(); ++i) {
        Preset* saved = filaments.find_preset(bundle->filament_presets[i], false, true);
        Preset* effective = filaments.find_preset(bundle->filament_presets[i], false, false);
        if (saved != nullptr && effective != nullptr)
            out.push_back({Preset::TYPE_FILAMENT, static_cast<int>(i) + 1, saved, effective});
    }
    return out;
}

} // namespace

Kind kind_of(const std::string& key)
{
    if (key == "post_process")
        return Kind::Script;
    if (key == "print_host" || key.rfind("printhost_", 0) == 0 || key == "print_host_webui" || key == "host_type" || key == "bbl_use_printhost" ||
        key == "flashforge_serial_number")
        return Kind::Network;
    const ConfigOptionDef* def = print_config_def.get(key);
    if (key == "plugins" || ends_with(key, "_plugin_config_overrides") || (def != nullptr && def->is_plugin_backed()))
        return Kind::Plugin;
    if ((def != nullptr && def->is_code) || ends_with(key, "_gcode"))
        return Kind::Code;
    return Kind::None;
}

const char* kind_name(Kind kind)
{
    switch (kind) {
    case Kind::Code: return "gcode";
    case Kind::Script: return "script";
    case Kind::Plugin: return "plugin";
    case Kind::Network: return "network";
    default: return "other";
    }
}

Library snapshot()
{
    Library library;
    for (Preset::Type type : {Preset::TYPE_PRINT, Preset::TYPE_FILAMENT, Preset::TYPE_PRINTER}) {
        std::set<std::string>& names = names_of(library, type);
        for (const Preset& preset : PresetOps::collection(type).get_presets())
            if (!preset.is_project_embedded && !preset.is_external)
                names.insert(preset.name);
    }
    if (const PresetBundle* bundle = wxGetApp().preset_bundle; bundle != nullptr)
        for (const std::string& key : bundle->project_config.keys())
            if (kind_of(key) == Kind::Code)
                library.project_code[key] = value_text(&bundle->project_config, key);
    return library;
}

json to_json(const std::vector<Finding>& findings)
{
    json out = json::array();
    for (const Finding& f : findings) {
        json item = json::object({{"scope", f.scope}, {"key", f.key}, {"kind", kind_name(f.kind)}});
        if (!f.owner.empty())
            item["name"] = f.owner;
        if (f.slot > 0)
            item[f.scope == "plate" ? "plate" : "slot"] = f.slot;
        out.push_back(std::move(item));
    }
    return out;
}

std::vector<Finding> foreign_code(const Library& library, const Model& model)
{
    std::vector<Finding> out;
    for (const InUse& use : presets_in_use()) {
        const DynamicPrintConfig* ref = reference(PresetOps::collection(use.type), *use.saved, names_of(library, use.type));
        for (const std::string& key : use.effective->config.keys())
            if (kind_of(key) == Kind::Code && value_text(&use.effective->config, key) != value_text(ref, key))
                out.push_back({PresetOps::type_name(use.type), use.saved->name, key, Kind::Code, use.slot});
    }
    if (const PresetBundle* bundle = wxGetApp().preset_bundle; bundle != nullptr)
        for (const std::string& key : bundle->project_config.keys())
            if (kind_of(key) == Kind::Code) {
                const auto        before = library.project_code.find(key);
                const std::string now = value_text(&bundle->project_config, key);
                if (before == library.project_code.end() ? now != value_text(nullptr, key) : now != before->second)
                    out.push_back({"project", std::string(), key, Kind::Code, 0});
            }
    // Custom G-code inserted at a layer (the preview's "Add custom G-code"): text of the file's own.
    for (const auto& [plate, info] : model.plates_custom_gcodes)
        for (const CustomGCode::Item& item : info.gcodes)
            if (item.type == CustomGCode::Custom && !item.extra.empty()) {
                out.push_back({"plate", std::string(), "custom_gcode", Kind::Code, plate + 1});
                break;
            }
    return out;
}

std::vector<Finding> reset_unsafe(const Library& library)
{
    std::vector<Finding> out;
    for (const InUse& use : presets_in_use()) {
        PresetCollection&            collection = PresetOps::collection(use.type);
        const std::set<std::string>& names = names_of(library, use.type);
        const DynamicPrintConfig*    ref = reference(collection, *use.saved, names);
        // A copy: the reference may be the saved preset itself, whose values are not changed then.
        const DynamicPrintConfig     ref_copy = ref != nullptr ? *ref : DynamicPrintConfig();
        const DynamicPrintConfig*    want_from = ref != nullptr ? &ref_copy : nullptr;
        std::set<std::string>        reported;
        auto report = [&](const std::string& key, Kind kind) {
            if (reported.insert(key).second)
                out.push_back({PresetOps::type_name(use.type), use.saved->name, key, kind, use.slot});
        };
        // 1. The preset that came with the file: its saved values, so discarding edits keeps them out.
        if (!from_library(*use.saved, names))
            for (const std::string& key : use.saved->config.keys()) {
                const Kind kind = kind_of(key);
                if (kind == Kind::Script || kind == Kind::Plugin || kind == Kind::Network) {
                    const std::string want = value_text(want_from, key);
                    if (value_text(&use.saved->config, key) != want) {
                        use.saved->config.set_deserialize_strict(key, want);
                        report(key, kind);
                    }
                }
            }
        // 2. The values in effect (unsaved edits the load made).
        std::vector<std::pair<std::string, std::string>> edits;
        for (const std::string& key : use.effective->config.keys()) {
            const Kind kind = kind_of(key);
            if (kind == Kind::Script || kind == Kind::Plugin || kind == Kind::Network) {
                const std::string want = value_text(want_from, key);
                if (value_text(&use.effective->config, key) != want) {
                    edits.emplace_back(key, want);
                    report(key, kind);
                }
            }
        }
        if (edits.empty())
            continue;
        if (use.effective == use.saved) {
            // Not the edited copy (a filament slot the tab is not bound to): step 1 changed it.
            continue;
        }
        if (use.type == Preset::TYPE_FILAMENT) {
            // The filament tab edits one slot's preset; write the values directly and refresh it.
            for (const auto& [key, value] : edits)
                use.effective->config.set_deserialize_strict(key, value);
            Tab& tab = PresetOps::tab(use.type);
            tab.reload_config();
            tab.update_dirty();
        } else
            PresetOps::tab(use.type).apply_preset_config_values(edits);
    }
    return out;
}

std::vector<Finding> strip_model(Model& model, size_t first_object, bool keep_code)
{
    std::vector<Finding> out;
    auto strip = [&](ModelConfig& config, const char* scope, const std::string& owner) {
        for (const std::string& key : config.keys()) {
            const Kind kind = kind_of(key);
            if (kind == Kind::None || (keep_code && kind == Kind::Code))
                continue;
            config.erase(key);
            out.push_back({scope, owner, key, kind, 0});
        }
    };
    for (size_t o = first_object; o < model.objects.size(); ++o) {
        ModelObject* object = model.objects[o];
        strip(object->config, "object", object->name);
        for (ModelVolume* volume : object->volumes)
            strip(volume->config, "part", volume->name);
        for (auto& range : object->layer_config_ranges)
            strip(range.second, "layer range", object->name);
    }
    return out;
}

json untrusted_scripts()
{
    json              out = json::array();
    PresetCollection& prints = PresetOps::collection(Preset::TYPE_PRINT);
    const Preset&     saved = prints.get_selected_preset();
    const Preset&     edited = prints.get_edited_preset();
    for (const char* key : {"post_process", "slicing_pipeline_plugin"}) {
        const ConfigOptionStrings* values = edited.config.option<ConfigOptionStrings>(key);
        bool                       set = false;
        for (const std::string& v : values != nullptr ? values->values : std::vector<std::string>())
            set = set || !v.empty();
        if (!set)
            continue;
        std::string reason;
        if (saved.is_project_embedded || saved.is_external)
            reason = "the process preset came with a project file";
        else if (value_text(&edited.config, key) != value_text(&saved.config, key))
            reason = "it is an unsaved edit of the process preset";
        if (!reason.empty())
            out.push_back(json::object({{"key", key}, {"preset", saved.name}, {"reason", reason}}));
    }
    return out;
}

}}}} // namespace Slic3r::GUI::Mcp::LoadGuard
