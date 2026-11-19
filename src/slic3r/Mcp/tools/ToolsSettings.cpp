// Read-only settings tools: settings_search, settings_get, preset_list, filament_list.
// Writing settings (settings_set, preset_select ...) lives in the settings-edit group; both use
// McpSettings.hpp.

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "libslic3r/Config.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"

#include "../McpIds.hpp"
#include "../McpScene.hpp"
#include "../McpSchema.hpp"
#include "../McpSettings.hpp"
#include "../McpTool.hpp"
#include "../McpUtil.hpp"

namespace Slic3r { namespace GUI { namespace Mcp {

namespace {

const size_t SEARCH_BUDGET_CHARS = 18000;

std::vector<std::string> words_of(const std::string& text)
{
    std::vector<std::string> words;
    std::string              current;
    for (char ch : to_lower_ascii(text)) {
        if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '%')
            current += ch;
        else if (!current.empty()) {
            words.push_back(current);
            current.clear();
        }
    }
    if (!current.empty())
        words.push_back(current);
    return words;
}

int mode_rank(const std::string& mode)
{
    if (mode == "simple") return 0;
    if (mode == "advanced") return 1;
    if (mode == "expert") return 2;
    return 3;
}

// How well a setting matches the query words: 0 = no match. Every word must appear somewhere;
// matches in the key and label count for more than matches in the tooltip.
int match_score(const ConfigOptionDef& def, const std::string& key, const std::vector<std::string>& words, const std::string& whole_query)
{
    const std::string label = to_lower_ascii(def.full_label.empty() ? def.label : def.full_label);
    const std::string category = to_lower_ascii(def.category);
    const std::string tooltip = to_lower_ascii(def.tooltip);
    // "infill density" should find sparse_infill_density: compare keys with spaces as underscores.
    std::string key_like = whole_query;
    std::replace(key_like.begin(), key_like.end(), ' ', '_');

    int score = 0;
    if (key == key_like)
        score += 1000;
    else if (key.find(key_like) != std::string::npos)
        score += 300;
    if (label == whole_query)
        score += 500;
    else if (label.find(whole_query) != std::string::npos)
        score += 200;
    for (const std::string& word : words) {
        int word_score = 0;
        if (key.find(word) != std::string::npos) word_score = std::max(word_score, 40);
        if (label.find(word) != std::string::npos) word_score = std::max(word_score, 40);
        if (category.find(word) != std::string::npos) word_score = std::max(word_score, 15);
        if (tooltip.find(word) != std::string::npos) word_score = std::max(word_score, 5);
        if (word_score == 0)
            return 0;
        score += word_score;
    }
    // Among equal matches, the settings a user meets first come first.
    score += def.mode == comSimple ? 30 : (def.mode == comAdvanced ? 15 : 0);
    return score;
}

json settings_search(ToolContext& ctx)
{
    const std::string query = to_lower_ascii(ctx.arg("query", ""));
    const std::string category = to_lower_ascii(ctx.arg("category", ""));
    const std::string owner = ctx.arg("owner", "");
    const std::string mode = ctx.arg("mode", "");
    const size_t      limit = ctx.arg<size_t>("limit", 20);
    const size_t      offset = ctx.arg<size_t>("offset", 0);
    const bool        has_scope = ctx.has("scope");
    Settings::Scope   scope;
    if (has_scope)
        scope = Settings::parse_scope(ctx.args()["scope"], true);

    // No filter at all: the table of contents.
    if (query.empty() && category.empty() && owner.empty() && mode.empty() && !has_scope) {
        std::map<std::string, std::map<std::string, size_t>> counts; // owner -> category -> count
        for (const std::string& key : Settings::all_keys()) {
            const ConfigOptionDef* def = Settings::find_def(key);
            counts[Settings::owner_name(Settings::owner_of(key))][def->category.empty() ? "(none)" : def->category]++;
        }
        json out = json::object();
        json owners = json::object();
        for (const auto& [owner_key, categories] : counts) {
            json list = json::object();
            for (const auto& [name, count] : categories)
                list[name] = count;
            owners[owner_key] = std::move(list);
        }
        out["categories"] = std::move(owners);
        out["total"] = Settings::all_keys().size();
        ctx.note("Pass a query (words from the key, label or description), a category or an owner to list settings.");
        return out;
    }

    const std::vector<std::string> words = words_of(query);
    std::vector<std::pair<int, std::string>> hits;
    for (const std::string& key : Settings::all_keys()) {
        const ConfigOptionDef* def = Settings::find_def(key);
        if (!owner.empty() && owner != Settings::owner_name(Settings::owner_of(key)))
            continue;
        if (!category.empty() && to_lower_ascii(def->category).find(category) == std::string::npos)
            continue;
        if (!mode.empty()) {
            const char* names[] = { "simple", "advanced", "expert", "develop" };
            if (mode_rank(names[std::min<int>(3, static_cast<int>(def->mode))]) > mode_rank(mode))
                continue;
        }
        if (has_scope && !Settings::key_allowed(scope, key))
            continue;
        const int score = words.empty() ? 1 : match_score(*def, key, words, query);
        if (score > 0)
            hits.emplace_back(-score, key);
    }
    std::sort(hits.begin(), hits.end()); // best score first, then alphabetical

    json   list = json::array();
    size_t used = 0;
    size_t next = 0;
    bool   truncated = false;
    for (size_t i = offset; i < hits.size(); ++i) {
        if (list.size() >= limit) {
            truncated = true;
            next = i;
            break;
        }
        json         entry = Settings::describe(hits[i].second, true);
        const size_t size = dump_json(entry).size();
        if (!list.empty() && used + size > SEARCH_BUDGET_CHARS) {
            truncated = true;
            next = i;
            break;
        }
        used += size;
        list.push_back(std::move(entry));
    }
    json out = json::object();
    out["settings"] = std::move(list);
    out["matches"] = hits.size();
    if (truncated) {
        out["truncated"] = true;
        out["next"] = next;
    }
    if (hits.empty())
        ctx.note("Nothing matched. Try fewer or different words; settings_search without arguments lists the categories.");
    return out;
}

[[noreturn]] void unknown_key(const std::string& key)
{
    const std::vector<std::string> nearest = Settings::nearest_keys(key);
    std::string hint = "settings_search finds keys by word.";
    if (!nearest.empty()) {
        hint = "Did you mean ";
        for (size_t i = 0; i < nearest.size(); ++i)
            hint += (i > 0 ? ", " : "") + nearest[i];
        hint += "? settings_search finds keys by word.";
    }
    fail("unknown_key", "There is no setting `" + key + "`.", hint, json::object({{"key", key}, {"nearest", nearest}}));
}

json settings_get(ToolContext& ctx)
{
    const Settings::Scope scope = Settings::parse_scope(ctx.args()["scope"]);
    const bool            effective = ctx.arg<bool>("effective", false);
    const bool            modified_only = ctx.arg<bool>("modified_only", false);
    const ConfigBase&     stored = Settings::stored_config(scope);

    std::vector<std::string> keys;
    const bool               explicit_keys = ctx.has("keys");
    if (explicit_keys) {
        for (const json& key : ctx.args()["keys"])
            keys.push_back(key.get<std::string>());
    } else {
        // Without keys: what was changed at this scope.
        keys = Settings::modified_keys(scope);
    }

    json out = json::object();
    out["scope"] = scope.name();
    if (const Preset* preset = Settings::preset_of(scope); preset != nullptr) {
        out["preset"] = preset->name;
        out["modified"] = Settings::modified_keys(scope);
    }

    const std::set<std::string> modified = [&]() {
        const std::vector<std::string> list = Settings::modified_keys(scope);
        return std::set<std::string>(list.begin(), list.end());
    }();
    DynamicPrintConfig full;
    if (effective)
        full = wxGetApp().preset_bundle->full_config();

    json values = json::object();
    for (const std::string& key : keys) {
        if (Settings::find_def(key) == nullptr) {
            if (explicit_keys)
                unknown_key(key);
            continue; // internal keys of a stored config (not settings) are skipped
        }
        if (explicit_keys && !effective) {
            std::string              why;
            std::vector<std::string> where;
            if (!Settings::key_allowed(scope, key, &why, &where)) {
                std::string hint = "It can be read at: ";
                for (size_t i = 0; i < where.size(); ++i)
                    hint += (i > 0 ? ", " : "") + where[i];
                hint += ". To see the value in force for this item, pass effective: true.";
                fail("key_not_in_scope", why, hint, json::object({{"key", key}, {"scopes", where}}));
            }
        }
        if (modified_only && modified.count(key) == 0)
            continue;
        if (effective)
            values[key] = Settings::effective_value(scope, key, full);
        else
            values[key] = Settings::value_json_for(stored, key); // null: not set at this scope
    }
    out["values"] = std::move(values);
    if (!explicit_keys && !scope.is_preset())
        ctx.note("Without `keys` this lists what is set at the scope itself. Pass keys with effective: true for the values in force.");
    return out;
}

json preset_list(ToolContext& ctx)
{
    PresetBundle&     bundle = *wxGetApp().preset_bundle;
    const std::string type = ctx.arg("type", "process");
    const std::string query = to_lower_ascii(ctx.arg("query", ""));
    const bool        compatible_only = ctx.arg<bool>("compatible_only", true);
    const size_t      limit = ctx.arg<size_t>("limit", 50);
    const size_t      offset = ctx.arg<size_t>("offset", 0);

    const PresetCollection* collection = &bundle.prints;
    if (type == "printer")
        collection = &bundle.printers;
    else if (type == "filament")
        collection = &bundle.filaments;
    const std::string selected = collection->get_selected_preset_name();

    // Which filament slots use each filament preset.
    std::map<std::string, std::vector<size_t>> slots;
    if (type == "filament")
        for (size_t i = 0; i < bundle.filament_presets.size(); ++i)
            slots[bundle.filament_presets[i]].push_back(i + 1);

    json   list = json::array();
    size_t index = 0, total = 0;
    bool   truncated = false;
    for (const Preset& preset : collection->get_presets()) {
        const bool in_use = preset.name == selected || slots.count(preset.name) != 0;
        if (preset.is_default || (!preset.is_visible && !in_use))
            continue;
        if (compatible_only && !preset.is_compatible && !in_use)
            continue;
        if (!query.empty() && to_lower_ascii(preset.name).find(query) == std::string::npos)
            continue;
        ++total;
        if (index++ < offset)
            continue;
        if (list.size() >= limit) {
            truncated = true;
            continue;
        }
        json item = json::object();
        item["name"] = preset.name;
        item["system"] = preset.is_system;
        item["compatible"] = preset.is_compatible;
        if (type == "filament") {
            if (auto it = slots.find(preset.name); it != slots.end())
                item["slots"] = it->second;
            if (const auto* filament_type = preset.config.option<ConfigOptionStrings>("filament_type"); filament_type != nullptr && !filament_type->values.empty())
                item["filament_type"] = filament_type->values.front();
        }
        if (preset.name == selected) {
            item["selected"] = true;
            const std::vector<std::string> dirty = collection->current_dirty_options();
            if (!dirty.empty())
                item["modified_keys"] = dirty;
        }
        if (const std::string& parent = preset.inherits(); !parent.empty())
            item["inherits"] = parent;
        list.push_back(std::move(item));
    }
    json out = json::object();
    out["type"] = type;
    out["selected"] = selected;
    out["presets"] = std::move(list);
    out["total"] = total;
    if (truncated) {
        out["truncated"] = true;
        out["next"] = offset + limit;
    }
    return out;
}

json filament_list(ToolContext&)
{
    const Model& model = wxGetApp().plater()->model();
    json         list = Scene::filaments_json();

    // Where each slot is used: as the filament of an object, of a part, or in colour painting.
    const size_t count = list.size();
    std::vector<json> objects(count, json::array()), parts(count, json::array()), paint(count, json::array());
    for (const ModelObject* object : model.objects) {
        const int object_slot = object->config.has("extruder") ? object->config.extruder() : 1;
        bool      object_uses_own = false;
        for (const ModelVolume* volume : object->volumes) {
            if (!volume->is_model_part() && !volume->is_modifier())
                continue;
            const int volume_slot = volume->config.has("extruder") ? volume->config.extruder() : 0;
            if (volume_slot >= 1 && volume_slot <= static_cast<int>(count))
                parts[static_cast<size_t>(volume_slot - 1)].push_back(Ids::of(*volume));
            else if (volume->is_model_part())
                object_uses_own = true;
            // Slots that appear only through painting on this volume.
            if (!volume->mmu_segmentation_facets.empty())
                for (int used : volume->get_extruders())
                    if (used >= 1 && used <= static_cast<int>(count) && used != volume_slot && !(volume_slot == 0 && used == object_slot)) {
                        json& target = paint[static_cast<size_t>(used - 1)];
                        if (std::find(target.begin(), target.end(), json(Ids::of(*volume))) == target.end())
                            target.push_back(Ids::of(*volume));
                    }
        }
        if (object_uses_own && object_slot >= 1 && object_slot <= static_cast<int>(count))
            objects[static_cast<size_t>(object_slot - 1)].push_back(Ids::of(*object));
    }
    for (size_t i = 0; i < count; ++i) {
        json used = json::object();
        used["objects"] = std::move(objects[i]);
        used["parts"] = std::move(parts[i]);
        used["paint"] = std::move(paint[i]);
        list[i]["used_by"] = std::move(used);
    }
    json out = json::object();
    out["filaments"] = std::move(list);
    return out;
}

} // namespace

MCP_TOOL_GROUP(Settings)
{
    {
        ToolDef tool;
        tool.name = "settings_search";
        tool.title = "Find settings";
        tool.description =
            "Find print, filament and printer settings by keyword, category or owner. Each hit gives the key to use with settings_get / "
            "settings_set, its label, type, unit, range, allowed values, default, which preset type owns it (process, filament, printer, "
            "project) and the scopes where it can be overridden (plate, object, part, modifier, layer_range). Query words are matched against "
            "the key, label, category and description; all words must match. Without arguments it returns the categories with counts. "
            "Labels and descriptions are in English.";
        tool.input_schema = Schema::parse(R"JSON({
            "type": "object",
            "properties": {
                "query": {"type": "string", "description": "Words to look for, e.g. \"infill density\", \"seam\", \"wall_loops\"."},
                "category": {"type": "string", "description": "Only settings whose category contains this, e.g. \"Strength\", \"Support\", \"Speed\"."},
                "owner": {"enum": ["process", "filament", "printer", "project"]},
                "scope": {"$ref": "#/$defs/Scope", "description": "Only settings that can be set at this scope."},
                "mode": {"enum": ["simple", "advanced", "expert"], "description": "Only settings shown in this user mode or a simpler one."},
                "limit": {"type": "integer", "minimum": 1, "maximum": 100, "default": 20},
                "offset": {"type": "integer", "minimum": 0, "default": 0}
            },
            "additionalProperties": false})JSON", "settings_search");
        tool.read_only = true;
        tool.idempotent = true;
        tool.handler = settings_search;
        registry.add(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "settings_get";
        tool.title = "Read settings";
        tool.description =
            "Read setting values at a scope: \"process\", \"printer\", \"project\", {\"filament\": slot}, {\"plate\": n}, {\"object\": id}, "
            "{\"volume\": id} or {\"layer_range\": {object, z_min, z_max}}. With `keys`, returns those values as stored at the scope (null when "
            "the scope does not set the key). With effective: true, returns for each key the value in force for that item and where it comes "
            "from (process, plate:1, object:o57, volume:v77 ...): narrower scopes override wider ones. Without `keys`, returns what is "
            "modified at the scope: for a preset the keys that differ from the saved preset, for an object, part or range its overrides. "
            "Values: numbers, booleans, strings; percentages as \"15%\"; lists for per-filament or per-extruder options.";
        tool.input_schema = Schema::parse(R"JSON({
            "type": "object",
            "properties": {
                "scope": {"$ref": "#/$defs/Scope"},
                "keys": {"type": "array", "items": {"type": "string"}, "maxItems": 200, "description": "Setting keys, as settings_search returns them."},
                "effective": {"type": "boolean", "default": false, "description": "Return {value, source}: the value in force and the scope it comes from."},
                "modified_only": {"type": "boolean", "default": false, "description": "Of the given keys, return only those modified at the scope."}
            },
            "required": ["scope"],
            "additionalProperties": false})JSON", "settings_get");
        tool.read_only = true;
        tool.idempotent = true;
        tool.handler = settings_get;
        registry.add(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "preset_list";
        tool.title = "List presets";
        tool.description =
            "List printer, process or filament presets: name, whether it is a system preset, compatible with the selected printer, selected "
            "(with the keys modified since it was saved), what it inherits from, and for filament presets the slots that use it. By default "
            "only presets compatible with the current printer are listed. Paged with limit / offset; `query` filters by name.";
        tool.input_schema = Schema::parse(R"JSON({
            "type": "object",
            "properties": {
                "type": {"enum": ["printer", "process", "filament"]},
                "query": {"type": "string", "description": "Only presets whose name contains this."},
                "compatible_only": {"type": "boolean", "default": true},
                "limit": {"type": "integer", "minimum": 1, "maximum": 200, "default": 50},
                "offset": {"type": "integer", "minimum": 0, "default": 0}
            },
            "required": ["type"],
            "additionalProperties": false})JSON", "preset_list");
        tool.read_only = true;
        tool.idempotent = true;
        tool.handler = preset_list;
        registry.add(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "filament_list";
        tool.title = "List filaments";
        tool.description =
            "The project's filament slots: slot number (1-based, the number objects, parts and paint refer to), preset, material type, colour, "
            "and where each slot is used: as the filament of objects, of individual parts or modifiers, or in colour painting.";
        tool.read_only = true;
        tool.idempotent = true;
        tool.handler = filament_list;
        registry.add(std::move(tool));
    }
}

}}} // namespace Slic3r::GUI::Mcp
