#include "McpSettings.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <map>
#include <set>
#include <sstream>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"

#include "McpError.hpp"
#include "McpScene.hpp"
#include "McpUtil.hpp"

namespace Slic3r { namespace GUI { namespace Mcp {

namespace Settings {

namespace {

const char* const REDACTED = "<redacted>";

std::set<std::string> to_set(const std::vector<std::string>& keys)
{
    return std::set<std::string>(keys.begin(), keys.end());
}

// The key sets, built once. Membership in the preset option lists is the test the tabs apply.
struct Catalogue
{
    std::set<std::string>        process, filament, printer, object, region, plate;
    std::vector<std::string>     keys; // every key with an owner, alphabetical
    std::map<std::string, Owner> owners;

    Catalogue()
    {
        process = to_set(Preset::print_options());
        filament = to_set(Preset::filament_options());
        printer = to_set(Preset::printer_options());
        object = to_set(PrintObjectConfig().keys());
        region = to_set(PrintRegionConfig().keys());
        // The keys of the plate settings dialog (Tab.cpp, plate_keys).
        plate = { "curr_bed_type", "skirt_start_angle", "first_layer_print_sequence", "first_layer_sequence_choice",
                  "other_layers_print_sequence", "other_layers_sequence_choice", "print_sequence", "spiral_mode" };
        const std::set<std::string> project = to_set(wxGetApp().preset_bundle->project_config.keys());
        for (const auto& [key, def] : print_config_def.options) {
            Owner owner = Owner::None;
            if (process.count(key) != 0) owner = Owner::Process;
            else if (filament.count(key) != 0) owner = Owner::Filament;
            else if (printer.count(key) != 0) owner = Owner::Printer;
            else if (project.count(key) != 0) owner = Owner::Project;
            if (owner == Owner::None)
                continue;
            owners[key] = owner;
            keys.push_back(key);
        }
    }
};

const Catalogue& catalogue()
{
    static const Catalogue instance;
    return instance;
}

const char* scalar_type_name(ConfigOptionType type)
{
    switch (static_cast<ConfigOptionType>(static_cast<int>(type) & ~static_cast<int>(coVectorType))) {
    case coFloat: return "float";
    case coInt: return "int";
    case coString: return "string";
    case coPercent: return "percent";
    case coFloatOrPercent: return "float_or_percent";
    case coPoint: return "point";
    case coPoint3: return "point3";
    case coBool: return "bool";
    case coEnum: return "enum";
    default: return "other";
    }
}

const char* mode_name(ConfigOptionMode mode)
{
    switch (mode) {
    case comSimple: return "simple";
    case comAdvanced: return "advanced";
    case comExpert: return "expert";
    default: return "develop";
    }
}

size_t edit_distance(const std::string& a, const std::string& b)
{
    std::vector<size_t> prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j)
        prev[j] = j;
    for (size_t i = 1; i <= a.size(); ++i) {
        cur[0] = i;
        for (size_t j = 1; j <= b.size(); ++j)
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] == b[j - 1] ? size_t(0) : size_t(1))});
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

std::string number_text(const json& value)
{
    // Integers without a fraction, floats in shortest form: what the profile files contain.
    if (value.is_number_integer())
        return std::to_string(value.get<long long>());
    std::ostringstream out;
    out.precision(15);
    out << value.get<double>();
    return out.str();
}

[[noreturn]] void invalid_value(const std::string& key, const json& value, const std::string& expected, json details = nullptr)
{
    std::string shown = dump_json(value);
    if (shown.size() > 80)
        shown = shown.substr(0, 77) + "...";
    if (details.is_null())
        details = json::object();
    details["key"] = key;
    fail("invalid_value", "`" + key + "`: " + shown + " is not a valid value. Expected " + expected + ".",
         "settings_search with the key shows its type, range and allowed values.", std::move(details));
}

void check_range(const ConfigOptionDef& def, const std::string& key, double number, const json& value)
{
    const bool has_min = def.min > -FLT_MAX;
    const bool has_max = def.max < FLT_MAX;
    if ((has_min && number < static_cast<double>(def.min)) || (has_max && number > static_cast<double>(def.max))) {
        json details = json::object();
        details["key"] = key;
        if (has_min) details["min"] = def.min;
        if (has_max) details["max"] = def.max;
        std::string range = has_min && has_max ? "between " + number_text(json(def.min)) + " and " + number_text(json(def.max))
                          : has_min ? "at least " + number_text(json(def.min)) : "at most " + number_text(json(def.max));
        fail("out_of_range", "`" + key + "`: " + dump_json(value) + " is out of range; it must be " + range + ".", "", std::move(details));
    }
}

// One element of the option in its serialised form.
std::string element_text(const ConfigOptionDef& def, const std::string& key, ConfigOptionType scalar, const json& value)
{
    if (value.is_null()) {
        if (!def.nullable)
            invalid_value(key, value, "a value (this option has no \"unset\" entry)");
        return "nil";
    }
    switch (scalar) {
    case coBool:
        if (value.is_boolean()) return value.get<bool>() ? "1" : "0";
        if (value.is_number_integer() && (value.get<long long>() == 0 || value.get<long long>() == 1)) return value.get<long long>() == 1 ? "1" : "0";
        invalid_value(key, value, "true or false");
    case coInt:
        if (!value.is_number() || std::floor(value.get<double>()) != value.get<double>())
            invalid_value(key, value, "an integer");
        check_range(def, key, value.get<double>(), value);
        return std::to_string(static_cast<long long>(value.get<double>()));
    case coFloat:
        if (!value.is_number())
            invalid_value(key, value, "a number");
        check_range(def, key, value.get<double>(), value);
        return number_text(value);
    case coPercent:
        if (value.is_number()) {
            check_range(def, key, value.get<double>(), value);
            return number_text(value) + "%";
        }
        if (value.is_string()) {
            std::string text = value.get<std::string>();
            if (!text.empty() && text.back() == '%')
                text.pop_back();
            char*        end = nullptr;
            const double number = std::strtod(text.c_str(), &end);
            if (!text.empty() && end != nullptr && *end == '\0') {
                check_range(def, key, number, value);
                return text + "%";
            }
        }
        invalid_value(key, value, "a percentage such as \"15%\" (or the number 15)");
    case coFloatOrPercent:
        if (value.is_number()) {
            return number_text(value);
        }
        if (value.is_string()) {
            std::string text = value.get<std::string>();
            const bool  percent = !text.empty() && text.back() == '%';
            if (percent)
                text.pop_back();
            char* end = nullptr;
            std::strtod(text.c_str(), &end);
            if (!text.empty() && end != nullptr && *end == '\0')
                return text + (percent ? "%" : "");
        }
        invalid_value(key, value, "a number (absolute value) or a percentage string such as \"50%\"");
    case coEnum: {
        if (!value.is_string())
            invalid_value(key, value, "one of the allowed values (a string)", json::object({{"values", def.enum_values}}));
        const std::string text = value.get<std::string>();
        const bool known = std::find(def.enum_values.begin(), def.enum_values.end(), text) != def.enum_values.end() ||
                           (def.enum_keys_map != nullptr && def.enum_keys_map->count(text) != 0);
        if (!known) {
            std::string list;
            for (const std::string& v : def.enum_values)
                list += (list.empty() ? "" : " | ") + v;
            invalid_value(key, value, "one of " + list, json::object({{"values", def.enum_values}}));
        }
        return text;
    }
    case coPoint:
        if (value.is_array() && value.size() == 2 && value[0].is_number() && value[1].is_number())
            return number_text(value[0]) + "x" + number_text(value[1]);
        if (value.is_string())
            return value.get<std::string>();
        invalid_value(key, value, "a point [x, y]");
    case coPoint3:
        if (value.is_array() && value.size() == 3 && value[0].is_number() && value[1].is_number() && value[2].is_number())
            return number_text(value[0]) + "," + number_text(value[1]) + "," + number_text(value[2]);
        if (value.is_string())
            return value.get<std::string>();
        invalid_value(key, value, "a point [x, y, z]");
    default:
        if (value.is_string())
            return value.get<std::string>();
        invalid_value(key, value, "a string");
    }
}

// "XxY" -> the option's own point syntax is used for vectors of points; a single point is "x,y".
std::string single_point_text(const json& value)
{
    return number_text(value[0]) + "," + number_text(value[1]);
}

PartPlate* plate_at(int plate_idx)
{
    return wxGetApp().plater()->get_partplate_list().get_plate(plate_idx);
}

const ModelConfig* find_range(const ModelObject& object, double z_min, double z_max)
{
    for (const auto& [range, config] : object.layer_config_ranges)
        if (std::abs(range.first - z_min) < 1e-4 && std::abs(range.second - z_max) < 1e-4)
            return &config;
    return nullptr;
}

std::string trim_number(double value)
{
    std::ostringstream out;
    out << Scene::round4(value);
    return out.str();
}

} // namespace

const char* owner_name(Owner owner)
{
    switch (owner) {
    case Owner::Process: return "process";
    case Owner::Filament: return "filament";
    case Owner::Printer: return "printer";
    case Owner::Project: return "project";
    case Owner::None: break;
    }
    return "";
}

const ConfigOptionDef* find_def(const std::string& key)
{
    if (catalogue().owners.count(key) == 0)
        return nullptr;
    return print_config_def.get(key);
}

Owner owner_of(const std::string& key)
{
    auto it = catalogue().owners.find(key);
    return it == catalogue().owners.end() ? Owner::None : it->second;
}

const std::vector<std::string>& all_keys() { return catalogue().keys; }

bool allowed_on_object(const std::string& key)
{
    const Catalogue& c = catalogue();
    return c.process.count(key) != 0 && (c.object.count(key) != 0 || c.region.count(key) != 0);
}

bool allowed_on_volume(const std::string& key)
{
    const Catalogue& c = catalogue();
    return c.process.count(key) != 0 && c.region.count(key) != 0;
}

bool allowed_on_layer_range(const std::string& key) { return key == "layer_height" || allowed_on_volume(key); }

bool allowed_on_plate(const std::string& key) { return catalogue().plate.count(key) != 0; }

std::vector<std::string> scopes_of(const std::string& key)
{
    std::vector<std::string> out;
    const Owner owner = owner_of(key);
    if (owner != Owner::None)
        out.emplace_back(owner_name(owner));
    if (allowed_on_plate(key)) out.emplace_back("plate");
    if (allowed_on_object(key)) out.emplace_back("object");
    if (allowed_on_volume(key)) {
        out.emplace_back("part");
        out.emplace_back("modifier");
    }
    if (allowed_on_layer_range(key)) out.emplace_back("layer_range");
    return out;
}

std::string vector_kind(const std::string& key)
{
    const ConfigOptionDef* def = find_def(key);
    if (def == nullptr || def->is_scalar())
        return {};
    switch (owner_of(key)) {
    case Owner::Filament: return "filament";
    case Owner::Printer: {
        const auto& extruder_keys = print_config_def.extruder_option_keys();
        return std::find(extruder_keys.begin(), extruder_keys.end(), key) != extruder_keys.end() ? "extruder" : "list";
    }
    default: return "list";
    }
}

std::vector<std::string> nearest_keys(const std::string& key, size_t count)
{
    std::vector<std::pair<size_t, std::string>> scored;
    const std::string wanted = to_lower_ascii(key);
    for (const std::string& candidate : all_keys()) {
        size_t score = edit_distance(wanted, candidate);
        if (candidate.find(wanted) != std::string::npos || (wanted.size() > 3 && wanted.find(candidate) != std::string::npos))
            score = std::min<size_t>(score, 2);
        if (score <= std::max<size_t>(3, wanted.size() / 3))
            scored.emplace_back(score, candidate);
    }
    std::sort(scored.begin(), scored.end());
    std::vector<std::string> out;
    for (size_t i = 0; i < scored.size() && i < count; ++i)
        out.push_back(scored[i].second);
    return out;
}

bool is_secret(const std::string& key)
{
    // Credentials, and where the printer is and how to reach it: an agent never reads them.
    return key == "printhost_apikey" || key == "printhost_password" || key == "printhost_user" || key == "printhost_port" ||
           key == "printhost_cafile" || key == "print_host" || key == "print_host_webui" || key == "flashforge_serial_number";
}

std::string write_refusal(const std::string& key)
{
    const ConfigOptionDef* def = find_def(key);
    if (key == "post_process")
        return "post-processing scripts are programs that run on export";
    if (key == "print_host" || key.rfind("printhost_", 0) == 0 || key == "bbl_use_printhost" || key == "printer_agent" || key == "print_host_webui" ||
        key == "host_type" || key == "flashforge_serial_number")
        return "it holds the printer's address or credentials";
    if (def != nullptr && def->is_plugin_backed())
        return "choosing a plug-in can install software";
    if (def != nullptr && (def->is_code || (key.size() > 6 && key.compare(key.size() - 6, 6, "_gcode") == 0))) {
        const AppConfig* config = wxGetApp().app_config;
        if (config == nullptr || !config->get_bool("mcp_allow_gcode_edit"))
            return "it is G-code text, and editing G-code is switched off in Preferences > MCP server";
    }
    return {};
}

json value_json(const ConfigOption& option) { return config_value_json(option); }

json value_json_for(const ConfigBase& config, const std::string& key)
{
    const ConfigOption* option = config.option(key);
    if (option == nullptr)
        return nullptr;
    if (is_secret(key)) {
        const std::string text = option->serialize();
        return text.empty() ? json("") : json(REDACTED);
    }
    return config_value_json(*option);
}

json describe(const std::string& key, bool with_tooltip)
{
    const ConfigOptionDef* def = find_def(key);
    json out = json::object();
    out["key"] = key;
    if (def == nullptr)
        return out;
    out["label"] = def->full_label.empty() ? def->label : def->full_label;
    const Owner owner = owner_of(key);
    if (!def->category.empty())
        out["category"] = def->category;
    out["type"] = scalar_type_name(def->type);
    if (const std::string kind = vector_kind(key); !kind.empty())
        out["vector"] = kind;
    if (def->nullable)
        out["nullable"] = true;
    if (!def->sidetext.empty())
        out["unit"] = def->sidetext;
    if (def->min > -FLT_MAX)
        out["min"] = def->min;
    if (def->max < FLT_MAX)
        out["max"] = def->max;
    if (!def->enum_values.empty()) {
        out["values"] = def->enum_values;
        if (!def->enum_labels.empty())
            out["labels"] = def->enum_labels;
    }
    if (def->default_value)
        out["default"] = is_secret(key) ? json("") : config_value_json(*def->default_value);
    if (!def->ratio_over.empty())
        out["percent_of"] = def->ratio_over;
    out["mode"] = mode_name(def->mode);
    out["owner"] = owner_name(owner);
    out["scopes"] = scopes_of(key);
    if (def->readonly)
        out["read_only"] = true;
    if (const std::string refusal = write_refusal(key); !refusal.empty())
        out["write"] = "refused: " + refusal;
    if (with_tooltip && !def->tooltip.empty()) {
        std::string tooltip = def->tooltip;
        if (tooltip.size() > 600)
            tooltip = tooltip.substr(0, 597) + "...";
        out["tooltip"] = tooltip;
    }
    return out;
}

std::unique_ptr<ConfigOption> option_from_json(const std::string& key, const json& value, size_t vector_size)
{
    const ConfigOptionDef* def = find_def(key);
    if (def == nullptr) {
        const std::vector<std::string> nearest = nearest_keys(key);
        std::string hint = "settings_search finds keys by word.";
        if (!nearest.empty()) {
            hint = "Did you mean ";
            for (size_t i = 0; i < nearest.size(); ++i)
                hint += (i > 0 ? ", " : "") + nearest[i];
            hint += "? settings_search finds keys by word.";
        }
        fail("unknown_key", "There is no setting `" + key + "`.", hint, json::object({{"key", key}, {"nearest", nearest}}));
    }

    std::unique_ptr<ConfigOption> option(def->create_default_option());
    if (!option)
        fail("internal", "Cannot create a value for `" + key + "`.");
    const auto scalar = static_cast<ConfigOptionType>(static_cast<int>(def->type) & ~static_cast<int>(coVectorType));

    std::string text;
    if (def->is_scalar()) {
        if (value.is_array() && scalar != coPoint && scalar != coPoint3)
            invalid_value(key, value, "a single value, not a list");
        if (scalar == coPoint && value.is_array() && value.size() == 2 && value[0].is_number() && value[1].is_number())
            text = single_point_text(value);
        else if (scalar == coString && value.is_string())
            text = escape_string_cstyle(value.get<std::string>()); // deserialize() un-escapes
        else
            text = element_text(*def, key, scalar, value);
    } else if (def->type == coPointsGroups || def->type == coIntsGroups) {
        if (!value.is_string())
            invalid_value(key, value, "the serialised string form of this option");
        text = value.get<std::string>();
    } else {
        std::vector<std::string> elements;
        if (value.is_array() && !(scalar == coPoint && value.size() == 2 && value[0].is_number())) {
            for (const json& element : value)
                elements.push_back(element_text(*def, key, scalar, element));
            if (vector_size > 0 && elements.size() != vector_size && scalar != coPoint)
                invalid_value(key, value, "a list of " + std::to_string(vector_size) + " values (one per " +
                              (vector_kind(key) == "filament" ? "filament" : vector_kind(key) == "extruder" ? "extruder" : "entry") +
                              "), or a single value for all of them",
                              json::object({{"expected_length", vector_size}}));
        } else {
            // One value for every entry.
            elements.assign(std::max<size_t>(1, vector_size), element_text(*def, key, scalar, value));
        }
        if (scalar == coString) {
            text = escape_strings_cstyle(elements);
        } else {
            for (size_t i = 0; i < elements.size(); ++i)
                text += (i > 0 ? "," : "") + elements[i];
        }
    }

    bool ok = false;
    try {
        ok = option->deserialize(text);
    } catch (const std::exception&) {
        ok = false;
    }
    if (!ok)
        invalid_value(key, value, std::string("a value of type ") + scalar_type_name(def->type) + (def->is_scalar() ? "" : " (a list)"));
    return option;
}

std::string Scope::name() const
{
    switch (kind) {
    case Kind::Process: return "process";
    case Kind::Printer: return "printer";
    case Kind::Project: return "project";
    case Kind::Filament: return "filament:" + std::to_string(filament_slot + 1);
    case Kind::Plate: return "plate:" + std::to_string(plate_idx + 1);
    case Kind::Object: return "object:" + Ids::of(*object.object);
    case Kind::Volume: return "volume:" + Ids::of(*volume.volume);
    case Kind::LayerRange: return "layer_range:" + trim_number(z_min) + "-" + trim_number(z_max);
    }
    return {};
}

Scope parse_scope(const json& value, bool allow_missing_range)
{
    Scope scope;
    if (value.is_string()) {
        const std::string text = value.get<std::string>();
        if (text == "process") scope.kind = Scope::Kind::Process;
        else if (text == "printer") scope.kind = Scope::Kind::Printer;
        else if (text == "project") scope.kind = Scope::Kind::Project;
        else fail("invalid_argument", "`scope`: \"" + text + "\" is not a scope.",
                  "Use \"process\", \"printer\", \"project\", {\"filament\": slot}, {\"plate\": n}, {\"object\": id}, {\"volume\": id} or {\"layer_range\": {object, z_min, z_max}}.");
        return scope;
    }
    if (!value.is_object() || value.size() != 1)
        fail("invalid_argument", "`scope` must be a scope name or an object with exactly one member.");
    if (value.contains("filament")) {
        scope.kind = Scope::Kind::Filament;
        scope.filament_slot = Ids::filament_slot(value["filament"], "scope.filament");
    } else if (value.contains("plate")) {
        scope.kind = Scope::Kind::Plate;
        scope.plate_idx = Ids::plate(value["plate"], "scope.plate");
    } else if (value.contains("object")) {
        scope.kind = Scope::Kind::Object;
        scope.object = Ids::object(value["object"].get<std::string>(), "scope.object");
    } else if (value.contains("volume")) {
        scope.kind = Scope::Kind::Volume;
        scope.volume = Ids::volume(value["volume"].get<std::string>(), "scope.volume");
        scope.object = ObjectRef { scope.volume.object, scope.volume.obj_idx };
    } else if (value.contains("layer_range")) {
        const json& range = value["layer_range"];
        scope.kind = Scope::Kind::LayerRange;
        scope.object = Ids::object(range.value("object", std::string()), "scope.layer_range.object");
        scope.z_min = range.value("z_min", 0.);
        scope.z_max = range.value("z_max", 0.);
        if (!allow_missing_range && find_range(*scope.object.object, scope.z_min, scope.z_max) == nullptr) {
            json ranges = json::array();
            for (const auto& [existing, config] : scope.object.object->layer_config_ranges)
                ranges.push_back(json::array({Scene::round4(existing.first), Scene::round4(existing.second)}));
            fail("stale_id", "`scope.layer_range`: " + Ids::of(*scope.object.object) + " has no height range " + trim_number(scope.z_min) + " - " + trim_number(scope.z_max) + ".",
                 "scene_get lists the object's layer_ranges.", json::object({{"ranges", ranges}}));
        }
    } else
        fail("invalid_argument", "`scope`: unknown member.");
    return scope;
}

bool key_allowed(const Scope& scope, const std::string& key, std::string* why, std::vector<std::string>* where)
{
    bool        ok = false;
    std::string what;
    const Owner owner = owner_of(key);
    switch (scope.kind) {
    case Scope::Kind::Process: ok = owner == Owner::Process; what = "a process setting"; break;
    case Scope::Kind::Printer: ok = owner == Owner::Printer; what = "a printer setting"; break;
    case Scope::Kind::Filament: ok = owner == Owner::Filament; what = "a filament setting"; break;
    case Scope::Kind::Project: ok = owner == Owner::Project; what = "a project setting"; break;
    case Scope::Kind::Plate: ok = allowed_on_plate(key); what = "a per-plate setting"; break;
    case Scope::Kind::Object: ok = allowed_on_object(key); what = "settable on an object"; break;
    case Scope::Kind::Volume: ok = allowed_on_volume(key); what = "settable on a part or modifier"; break;
    case Scope::Kind::LayerRange: ok = allowed_on_layer_range(key); what = "settable on a height range"; break;
    }
    if (!ok) {
        if (why != nullptr)
            *why = "`" + key + "` is not " + what + ".";
        if (where != nullptr)
            *where = scopes_of(key);
    }
    return ok;
}

const Preset* preset_of(const Scope& scope)
{
    PresetBundle& bundle = *wxGetApp().preset_bundle;
    switch (scope.kind) {
    case Scope::Kind::Process: return &bundle.prints.get_edited_preset();
    case Scope::Kind::Printer: return &bundle.printers.get_edited_preset();
    case Scope::Kind::Filament: {
        const std::string& name = bundle.filament_presets[static_cast<size_t>(scope.filament_slot)];
        // The edited copy when this slot's preset is the one open in the filament tab.
        if (bundle.filaments.get_edited_preset().name == name)
            return &bundle.filaments.get_edited_preset();
        return bundle.filaments.find_preset(name);
    }
    default: return nullptr;
    }
}

const ConfigBase& stored_config(const Scope& scope)
{
    static const DynamicPrintConfig empty;
    PresetBundle& bundle = *wxGetApp().preset_bundle;
    switch (scope.kind) {
    case Scope::Kind::Process:
    case Scope::Kind::Printer:
    case Scope::Kind::Filament: {
        const Preset* preset = preset_of(scope);
        if (preset != nullptr)
            return preset->config;
        return empty;
    }
    case Scope::Kind::Project: return bundle.project_config;
    case Scope::Kind::Plate: {
        const DynamicPrintConfig* config = plate_at(scope.plate_idx)->config();
        if (config != nullptr)
            return *config;
        return empty;
    }
    case Scope::Kind::Object: return scope.object.object->config.get();
    case Scope::Kind::Volume: return scope.volume.volume->config.get();
    case Scope::Kind::LayerRange: {
        const ModelConfig* config = find_range(*scope.object.object, scope.z_min, scope.z_max);
        if (config != nullptr)
            return config->get();
        return empty;
    }
    }
    return empty;
}

std::vector<std::string> modified_keys(const Scope& scope)
{
    PresetBundle& bundle = *wxGetApp().preset_bundle;
    switch (scope.kind) {
    case Scope::Kind::Process: return bundle.prints.current_dirty_options();
    case Scope::Kind::Printer: return bundle.printers.current_dirty_options();
    case Scope::Kind::Filament: {
        const std::string& name = bundle.filament_presets[static_cast<size_t>(scope.filament_slot)];
        if (bundle.filaments.get_edited_preset().name == name)
            return bundle.filaments.current_dirty_options();
        return {};
    }
    default: return stored_config(scope).keys();
    }
}

json effective_value(const Scope& scope, const std::string& key, const DynamicPrintConfig& full_config)
{
    json        value = nullptr;
    std::string source;
    auto        take = [&](const ConfigBase& config, const std::string& from) {
        if (config.option(key) != nullptr) {
            value = value_json_for(config, key);
            source = from;
        }
    };

    // The global configuration: process, filaments, printer and project merged, as slicing sees it.
    take(full_config, owner_name(owner_of(key)));
    if (scope.is_preset() || scope.kind == Scope::Kind::Project) {
        take(stored_config(scope), scope.name());
    } else {
        // The plate the item is on overrides the per-plate keys.
        int plate_idx = scope.plate_idx;
        if (scope.is_model() && !scope.object.object->instances.empty())
            plate_idx = Ids::plate_of_instance(scope.object.obj_idx, 0);
        if (plate_idx >= 0 && allowed_on_plate(key))
            if (const DynamicPrintConfig* config = plate_at(plate_idx)->config(); config != nullptr)
                take(*config, "plate:" + std::to_string(plate_idx + 1));
        if (scope.is_model()) {
            take(scope.object.object->config.get(), "object:" + Ids::of(*scope.object.object));
            if (scope.kind != Scope::Kind::Object)
                take(stored_config(scope), scope.name());
        }
    }

    json out = json::object();
    out["value"] = std::move(value);
    out["source"] = source;
    return out;
}

const ConfigOption* effective_option(const ModelObject& object, const ModelVolume* volume, const std::string& key)
{
    if (volume != nullptr && volume->config.has(key))
        return volume->config.get().option(key);
    if (object.config.has(key))
        return object.config.get().option(key);
    const ModelObjectPtrs& objects = wxGetApp().plater()->model().objects;
    const auto             found = std::find(objects.begin(), objects.end(), &object);
    const int              obj_idx = found == objects.end() ? -1 : static_cast<int>(found - objects.begin());
    if (obj_idx >= 0 && !object.instances.empty() && allowed_on_plate(key)) {
        const int plate_idx = Ids::plate_of_instance(obj_idx, 0);
        if (plate_idx >= 0)
            if (const DynamicPrintConfig* config = plate_at(plate_idx)->config(); config != nullptr && config->option(key) != nullptr)
                return config->option(key);
    }
    PresetBundle& bundle = *wxGetApp().preset_bundle;
    for (const ConfigBase* config : {static_cast<const ConfigBase*>(&bundle.prints.get_edited_preset().config),
                                     static_cast<const ConfigBase*>(&bundle.printers.get_edited_preset().config),
                                     static_cast<const ConfigBase*>(&bundle.filaments.get_edited_preset().config),
                                     static_cast<const ConfigBase*>(&bundle.project_config)})
        if (const ConfigOption* option = config->option(key); option != nullptr)
            return option;
    return nullptr;
}

} // namespace Settings

}}} // namespace Slic3r::GUI::Mcp
