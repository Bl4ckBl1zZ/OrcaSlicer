#include "McpSchema.hpp"

#include <algorithm>
#include <cmath>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

namespace Schema {

json parse(const char* text, const char* what)
{
    try {
        return json::parse(text);
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("MCP: invalid JSON in ") + what + ": " + e.what());
    }
}

const json& shared_defs()
{
    static const json defs = parse(R"JSON({
  "Id": {"type": "string", "pattern": "^(o|i|v|pl)[0-9]+$",
         "description": "an id from scene_get: o<N> object, i<N> instance, v<N> volume (part or modifier), pl<N> plate"},
  "PlateRef": {"description": "Plate: 1-based number as shown in the app, a pl<N> id, or \"current\".",
               "oneOf": [{"type": "integer", "minimum": 1}, {"type": "string", "pattern": "^(pl[0-9]+|current)$"}]},
  "Vec3": {"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 3},
  "Vec2": {"type": "array", "items": {"type": "number"}, "minItems": 2, "maxItems": 2},
  "Frame": {"enum": ["plate", "world", "object"]},
  "Color": {"type": "string", "pattern": "^#[0-9A-Fa-f]{6}$", "description": "a colour as #RRGGBB"},
  "Scope": {"description": "Where a setting lives: \"process\", \"printer\", \"project\", {\"filament\": slot}, {\"plate\": n}, {\"object\": id}, {\"volume\": id} or {\"layer_range\": {object, z_min, z_max}}.",
    "oneOf": [
      {"enum": ["process", "printer", "project"]},
      {"type": "object", "properties": {"filament": {"type": "integer", "minimum": 1}}, "required": ["filament"], "additionalProperties": false},
      {"type": "object", "properties": {"plate": {"$ref": "#/$defs/PlateRef"}}, "required": ["plate"], "additionalProperties": false},
      {"type": "object", "properties": {"object": {"$ref": "#/$defs/Id"}}, "required": ["object"], "additionalProperties": false},
      {"type": "object", "properties": {"volume": {"$ref": "#/$defs/Id"}}, "required": ["volume"], "additionalProperties": false},
      {"type": "object", "properties": {"layer_range": {"type": "object",
          "properties": {"object": {"$ref": "#/$defs/Id"}, "z_min": {"type": "number"}, "z_max": {"type": "number"}},
          "required": ["object", "z_min", "z_max"], "additionalProperties": false}}, "required": ["layer_range"], "additionalProperties": false}]},
  "Camera": {"type": "object", "properties": {
      "preset": {"enum": ["iso", "iso_left", "iso_back", "iso_back_left", "top", "bottom", "front", "back", "left", "right"]},
      "azimuth_deg": {"type": "number", "description": "0 looks from the front (-Y towards +Y); increases counter-clockwise seen from above."},
      "elevation_deg": {"type": "number", "minimum": -90, "maximum": 90, "description": "0 is horizontal, 90 looks straight down."},
      "target": {"description": "What to look at: \"plate\", \"objects\", \"toolpaths\", a point [x,y,z] in world mm, or a list of ids.",
                 "oneOf": [{"enum": ["plate", "objects", "toolpaths"]}, {"$ref": "#/$defs/Vec3"}, {"type": "array", "items": {"$ref": "#/$defs/Id"}, "minItems": 1}]},
      "distance": {"type": "number", "exclusiveMinimum": 0, "description": "Camera distance from the target in mm (perspective). Overrides zoom."},
      "zoom": {"type": "number", "exclusiveMinimum": 0, "description": "1 = fit target; 2 = twice as close."},
      "projection": {"enum": ["perspective", "orthographic"]}}, "additionalProperties": false}
})JSON", "shared $defs");
    return defs;
}

namespace {

const char* const REF_PREFIX = "#/$defs/";

void collect_refs(const json& node, std::set<std::string>& out)
{
    if (node.is_object()) {
        for (auto it = node.begin(); it != node.end(); ++it) {
            if (it.key() == "$ref" && it.value().is_string()) {
                const std::string ref = it.value().get<std::string>();
                if (ref.rfind(REF_PREFIX, 0) == 0)
                    out.insert(ref.substr(std::string(REF_PREFIX).size()));
            } else
                collect_refs(it.value(), out);
        }
    } else if (node.is_array()) {
        for (const json& item : node)
            collect_refs(item, out);
    }
}

const json* resolve_ref(const json& root, const std::string& ref)
{
    if (ref.rfind(REF_PREFIX, 0) != 0 || !root.is_object())
        return nullptr;
    auto defs = root.find("$defs");
    if (defs == root.end() || !defs->is_object())
        return nullptr;
    auto it = defs->find(ref.substr(std::string(REF_PREFIX).size()));
    return it == defs->end() ? nullptr : &*it;
}

std::string show(const json& value, size_t max_len = 80)
{
    std::string s = value.dump();
    if (s.size() > max_len)
        s = s.substr(0, max_len - 3) + "...";
    return s;
}

std::string join(const std::vector<std::string>& items, const char* sep = ", ")
{
    std::string out;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i > 0)
            out += sep;
        out += items[i];
    }
    return out;
}

const char* type_name(const json& value)
{
    switch (value.type()) {
    case json::value_t::null: return "null";
    case json::value_t::boolean: return "boolean";
    case json::value_t::string: return "string";
    case json::value_t::array: return "array";
    case json::value_t::object: return "object";
    case json::value_t::number_integer:
    case json::value_t::number_unsigned: return "integer";
    case json::value_t::number_float: return "number";
    default: return "value";
    }
}

bool is_integral(const json& value)
{
    if (value.is_number_integer())
        return true;
    if (value.is_number_float()) {
        const double d = value.get<double>();
        return std::isfinite(d) && std::floor(d) == d && std::fabs(d) < 9.0e15;
    }
    return false;
}

bool type_matches(const std::string& type, const json& value)
{
    if (type == "string") return value.is_string();
    if (type == "number") return value.is_number();
    if (type == "integer") return is_integral(value);
    if (type == "boolean") return value.is_boolean();
    if (type == "array") return value.is_array();
    if (type == "object") return value.is_object();
    if (type == "null") return value.is_null();
    return false;
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

std::string path_or_root(const std::string& path)
{
    return path.empty() ? std::string("The arguments") : "`" + path + "`";
}

std::string child_path(const std::string& path, const std::string& key)
{
    return path.empty() ? key : path + "." + key;
}

// One-line description of what a schema accepts, for "must be ..." messages.
std::string describe(const json& root, const json& schema, int depth = 0)
{
    if (!schema.is_object())
        return "any value";
    if (auto ref = schema.find("$ref"); ref != schema.end() && ref->is_string()) {
        if (const json* target = resolve_ref(root, ref->get<std::string>()); target != nullptr && depth < 4)
            return describe(root, *target, depth + 1);
        return "a value";
    }
    if (auto it = schema.find("const"); it != schema.end())
        return show(*it);
    if (auto it = schema.find("enum"); it != schema.end() && it->is_array()) {
        std::vector<std::string> values;
        for (const json& v : *it)
            values.push_back(show(v, 40));
        return "one of " + join(values, " | ");
    }
    for (const char* combiner : {"oneOf", "anyOf"}) {
        if (auto it = schema.find(combiner); it != schema.end() && it->is_array()) {
            std::vector<std::string> alternatives;
            for (const json& alt : *it)
                alternatives.push_back(describe(root, alt, depth + 1));
            return join(alternatives, ", or ");
        }
    }
    std::string type;
    if (auto it = schema.find("type"); it != schema.end()) {
        if (it->is_string())
            type = it->get<std::string>();
        else if (it->is_array()) {
            std::vector<std::string> types;
            for (const json& t : *it)
                if (t.is_string())
                    types.push_back(t.get<std::string>());
            type = join(types, " or ");
        }
    }
    std::ostringstream out;
    if (type == "object") {
        out << "an object";
        if (auto req = schema.find("required"); req != schema.end() && req->is_array() && !req->empty()) {
            std::vector<std::string> keys;
            for (const json& k : *req)
                if (k.is_string())
                    keys.push_back(k.get<std::string>());
            out << " with " << join(keys);
        } else if (auto props = schema.find("properties"); props != schema.end() && props->is_object() && !props->empty()) {
            std::vector<std::string> keys;
            for (auto p = props->begin(); p != props->end(); ++p)
                keys.push_back(p.key());
            out << " {" << join(keys) << "}";
        }
        return out.str();
    }
    if (type == "array") {
        out << "an array";
        const auto min_items = schema.find("minItems");
        const auto max_items = schema.find("maxItems");
        if (min_items != schema.end() && max_items != schema.end() && *min_items == *max_items)
            out << " of " << min_items->dump();
        if (auto items = schema.find("items"); items != schema.end() && items->is_object()) {
            if (auto item_type = items->find("type"); item_type != items->end() && item_type->is_string())
                out << " " << item_type->get<std::string>() << "s";
            else
                out << " items, each " << describe(root, *items, depth + 1);
        }
        return out.str();
    }
    if (type.empty() && schema.contains("pattern"))
        type = "string";
    out << (type.empty() ? "a value" : (type == "integer" ? "an integer" : "a " + type));
    if (auto it = schema.find("minimum"); it != schema.end()) out << " >= " << it->dump();
    if (auto it = schema.find("exclusiveMinimum"); it != schema.end()) out << " > " << it->dump();
    if (auto it = schema.find("maximum"); it != schema.end()) out << " <= " << it->dump();
    if (auto it = schema.find("exclusiveMaximum"); it != schema.end()) out << " < " << it->dump();
    if (auto it = schema.find("pattern"); it != schema.end() && it->is_string()) out << " matching " << it->get<std::string>();
    return out.str();
}

class Validator
{
public:
    explicit Validator(const json& root) : m_root(root) {}

    // Returns an empty string when `value` is valid.
    std::string run(const json& schema, const json& value, const std::string& path, int depth = 0)
    {
        if (depth > 64)
            return path_or_root(path) + " is nested too deeply.";
        if (schema.is_boolean())
            return schema.get<bool>() ? std::string() : path_or_root(path) + " is not allowed here.";
        if (!schema.is_object())
            return {};

        if (auto ref = schema.find("$ref"); ref != schema.end() && ref->is_string()) {
            const json* target = resolve_ref(m_root, ref->get<std::string>());
            if (target == nullptr)
                return "Internal schema error: unresolved " + ref->get<std::string>() + ".";
            if (std::string err = run(*target, value, path, depth + 1); !err.empty())
                return err;
        }

        if (auto it = schema.find("type"); it != schema.end()) {
            bool ok = false;
            if (it->is_string())
                ok = type_matches(it->get<std::string>(), value);
            else if (it->is_array())
                for (const json& t : *it)
                    ok = ok || (t.is_string() && type_matches(t.get<std::string>(), value));
            if (!ok)
                return path_or_root(path) + " must be " + describe(m_root, schema) + " (got " + type_name(value) + " " + show(value, 60) + ").";
        }

        if (auto it = schema.find("const"); it != schema.end() && *it != value)
            return path_or_root(path) + " must be " + show(*it) + " (got " + show(value, 60) + ").";

        if (auto it = schema.find("enum"); it != schema.end() && it->is_array()) {
            if (std::find(it->begin(), it->end(), value) == it->end())
                return path_or_root(path) + " must be " + describe(m_root, schema) + " (got " + show(value, 60) + ").";
        }

        if (value.is_number()) {
            const double d = value.get<double>();
            if (auto it = schema.find("minimum"); it != schema.end() && it->is_number() && d < it->get<double>())
                return path_or_root(path) + " must be >= " + it->dump() + " (got " + value.dump() + ").";
            if (auto it = schema.find("maximum"); it != schema.end() && it->is_number() && d > it->get<double>())
                return path_or_root(path) + " must be <= " + it->dump() + " (got " + value.dump() + ").";
            if (auto it = schema.find("exclusiveMinimum"); it != schema.end() && it->is_number() && d <= it->get<double>())
                return path_or_root(path) + " must be > " + it->dump() + " (got " + value.dump() + ").";
            if (auto it = schema.find("exclusiveMaximum"); it != schema.end() && it->is_number() && d >= it->get<double>())
                return path_or_root(path) + " must be < " + it->dump() + " (got " + value.dump() + ").";
        }

        if (value.is_string()) {
            const std::string s = value.get<std::string>();
            if (auto it = schema.find("minLength"); it != schema.end() && it->is_number() && s.size() < it->get<size_t>())
                return path_or_root(path) + " must be at least " + it->dump() + " characters long.";
            if (auto it = schema.find("maxLength"); it != schema.end() && it->is_number() && s.size() > it->get<size_t>())
                return path_or_root(path) + " must be at most " + it->dump() + " characters long.";
            if (auto it = schema.find("pattern"); it != schema.end() && it->is_string()) {
                bool matched = true;
                try {
                    matched = std::regex_search(s, std::regex(it->get<std::string>(), std::regex::ECMAScript));
                } catch (const std::regex_error&) {
                    return "Internal schema error: bad pattern " + it->get<std::string>() + ".";
                }
                if (!matched) {
                    const std::string description = schema.value("description", std::string());
                    if (description.empty())
                        return path_or_root(path) + " must match " + it->get<std::string>() + " (got " + show(value, 60) + ").";
                    return path_or_root(path) + " must be " + description + " (got " + show(value, 60) + ").";
                }
            }
        }

        if (value.is_array()) {
            if (auto it = schema.find("minItems"); it != schema.end() && it->is_number() && value.size() < it->get<size_t>())
                return path_or_root(path) + " must have at least " + it->dump() + " item(s) (got " + std::to_string(value.size()) + ").";
            if (auto it = schema.find("maxItems"); it != schema.end() && it->is_number() && value.size() > it->get<size_t>())
                return path_or_root(path) + " must have at most " + it->dump() + " item(s) (got " + std::to_string(value.size()) + ").";
            if (auto it = schema.find("items"); it != schema.end()) {
                for (size_t i = 0; i < value.size(); ++i)
                    if (std::string err = run(*it, value[i], path + "[" + std::to_string(i) + "]", depth + 1); !err.empty())
                        return err;
            }
        }

        if (value.is_object()) {
            if (auto it = schema.find("required"); it != schema.end() && it->is_array()) {
                for (const json& key : *it)
                    if (key.is_string() && !value.contains(key.get<std::string>())) {
                        std::string msg = "Missing required argument `" + child_path(path, key.get<std::string>()) + "`";
                        if (auto props = schema.find("properties"); props != schema.end() && props->is_object())
                            if (auto p = props->find(key.get<std::string>()); p != props->end())
                                msg += " (" + describe(m_root, *p) + ")";
                        return msg + ".";
                    }
            }
            const auto props = schema.find("properties");
            const auto additional = schema.find("additionalProperties");
            for (auto member = value.begin(); member != value.end(); ++member) {
                const json* member_schema = nullptr;
                if (props != schema.end() && props->is_object())
                    if (auto p = props->find(member.key()); p != props->end())
                        member_schema = &*p;
                if (member_schema != nullptr) {
                    if (std::string err = run(*member_schema, member.value(), child_path(path, member.key()), depth + 1); !err.empty())
                        return err;
                } else if (additional != schema.end()) {
                    if (additional->is_boolean() && !additional->get<bool>())
                        return unknown_property(schema, path, member.key());
                    if (additional->is_object())
                        if (std::string err = run(*additional, member.value(), child_path(path, member.key()), depth + 1); !err.empty())
                            return err;
                }
            }
        }

        if (auto it = schema.find("allOf"); it != schema.end() && it->is_array()) {
            for (const json& sub : *it)
                if (std::string err = run(sub, value, path, depth + 1); !err.empty())
                    return err;
        }

        if (auto it = schema.find("anyOf"); it != schema.end() && it->is_array()) {
            if (std::string err = alternatives(*it, value, path, depth, false); !err.empty())
                return err;
        }

        if (auto it = schema.find("oneOf"); it != schema.end() && it->is_array()) {
            if (std::string err = alternatives(*it, value, path, depth, true); !err.empty())
                return err;
        }

        if (auto it = schema.find("not"); it != schema.end()) {
            if (run(*it, value, path, depth + 1).empty())
                return path_or_root(path) + " must not be " + describe(m_root, *it) + ".";
        }

        return {};
    }

private:
    std::string unknown_property(const json& schema, const std::string& path, const std::string& key) const
    {
        std::vector<std::string> known;
        if (auto props = schema.find("properties"); props != schema.end() && props->is_object())
            for (auto p = props->begin(); p != props->end(); ++p)
                known.push_back(p.key());
        std::string msg = "Unknown argument `" + child_path(path, key) + "`.";
        std::string best;
        size_t best_distance = 3;
        for (const std::string& candidate : known) {
            const size_t d = edit_distance(key, candidate);
            if (d < best_distance) {
                best_distance = d;
                best = candidate;
            }
        }
        if (!best.empty())
            msg += " Did you mean `" + best + "`?";
        if (!known.empty())
            msg += " Accepted: " + join(known) + ".";
        return msg;
    }

    // The schema a $ref chain ends at.
    const json& deref(const json& schema) const
    {
        const json* cur = &schema;
        for (int i = 0; i < 8 && cur->is_object(); ++i) {
            auto ref = cur->find("$ref");
            if (ref == cur->end() || !ref->is_string())
                break;
            const json* target = resolve_ref(m_root, ref->get<std::string>());
            if (target == nullptr)
                break;
            cur = target;
        }
        return *cur;
    }

    // oneOf / anyOf. When nothing matches, reports the error of the alternative the value most
    // plainly aimed at (same JSON type; for objects, the one whose required key is present),
    // otherwise lists what is accepted.
    std::string alternatives(const json& options, const json& value, const std::string& path, int depth, bool exactly_one)
    {
        size_t matches = 0;
        std::vector<std::string> errors;
        for (const json& option : options) {
            std::string err = run(option, value, path, depth + 1);
            if (err.empty())
                ++matches;
            errors.push_back(std::move(err));
        }
        if (matches == 1 || (matches > 1 && !exactly_one))
            return {};
        if (matches > 1)
            return path_or_root(path) + " is ambiguous: it matches more than one accepted form.";

        std::vector<size_t> candidates;
        for (size_t i = 0; i < options.size(); ++i) {
            const json& option = deref(options[i]);
            if (!option.is_object())
                continue;
            if (value.is_object()) {
                auto req = option.find("required");
                if (req == option.end() || !req->is_array())
                    continue;
                bool any = false;
                for (const json& key : *req)
                    any = any || (key.is_string() && value.contains(key.get<std::string>()));
                if (any)
                    candidates.push_back(i);
            } else if (auto type = option.find("type"); type != option.end() && type->is_string() && type_matches(type->get<std::string>(), value)) {
                candidates.push_back(i);
            }
        }
        if (candidates.size() == 1)
            return errors[candidates.front()];

        std::vector<std::string> forms;
        for (const json& option : options)
            forms.push_back(describe(m_root, option));
        return path_or_root(path) + " must be " + join(forms, ", or ") + " (got " + show(value, 60) + ").";
    }

    const json& m_root;
};

const std::set<std::string>& supported_keywords()
{
    static const std::set<std::string> keys = {
        "type", "enum", "const", "properties", "required", "additionalProperties", "items", "minItems", "maxItems",
        "minimum", "maximum", "exclusiveMinimum", "exclusiveMaximum", "minLength", "maxLength", "pattern",
        "oneOf", "anyOf", "allOf", "not", "$ref", "$defs", "$schema",
        "description", "default", "title", "examples"};
    return keys;
}

std::string check_node(const json& root, const json& node, const std::string& where, int depth)
{
    if (depth > 64)
        return where + ": nested too deeply";
    if (node.is_boolean())
        return {};
    if (!node.is_object())
        return where + ": a schema must be an object or a boolean";
    for (auto it = node.begin(); it != node.end(); ++it) {
        const std::string& key = it.key();
        if (supported_keywords().count(key) == 0)
            return where + ": unsupported keyword \"" + key + "\"";
        if (key == "$ref") {
            if (!it->is_string() || resolve_ref(root, it->get<std::string>()) == nullptr)
                return where + ": unresolved $ref " + it->dump();
        } else if (key == "properties" || key == "$defs") {
            if (!it->is_object())
                return where + ": \"" + key + "\" must be an object";
            for (auto p = it->begin(); p != it->end(); ++p)
                if (std::string err = check_node(root, p.value(), where + "/" + key + "/" + p.key(), depth + 1); !err.empty())
                    return err;
        } else if (key == "items" || key == "not" || (key == "additionalProperties" && it->is_object())) {
            if (std::string err = check_node(root, *it, where + "/" + key, depth + 1); !err.empty())
                return err;
        } else if (key == "oneOf" || key == "anyOf" || key == "allOf") {
            if (!it->is_array() || it->empty())
                return where + ": \"" + key + "\" must be a non-empty array";
            for (size_t i = 0; i < it->size(); ++i)
                if (std::string err = check_node(root, (*it)[i], where + "/" + key + "/" + std::to_string(i), depth + 1); !err.empty())
                    return err;
        } else if (key == "pattern") {
            if (!it->is_string())
                return where + ": \"pattern\" must be a string";
            try {
                std::regex re(it->get<std::string>(), std::regex::ECMAScript);
            } catch (const std::regex_error&) {
                return where + ": bad pattern " + it->dump();
            }
        } else if (key == "required" || key == "enum") {
            if (!it->is_array())
                return where + ": \"" + key + "\" must be an array";
        }
    }
    return {};
}

} // namespace

json with_shared_defs(json schema)
{
    if (!schema.is_object())
        return schema;
    const json& shared = shared_defs();
    json defs = json::object();
    if (auto own = schema.find("$defs"); own != schema.end() && own->is_object())
        defs = *own;

    // Pull in referenced fragments until nothing new is referenced.
    std::set<std::string> done;
    for (;;) {
        std::set<std::string> refs;
        collect_refs(schema, refs);
        collect_refs(defs, refs);
        bool added = false;
        for (const std::string& name : refs) {
            if (!done.insert(name).second || defs.contains(name))
                continue;
            if (auto it = shared.find(name); it != shared.end()) {
                defs[name] = *it;
                added = true;
            }
        }
        if (!added)
            break;
    }
    if (!defs.empty())
        schema["$defs"] = std::move(defs);
    return schema;
}

std::string validate(const json& schema, const json& value)
{
    Validator validator(schema);
    return validator.run(schema, value, std::string());
}

std::string check_schema(const json& schema)
{
    return check_node(schema, schema, "#", 0);
}

} // namespace Schema

}}} // namespace Slic3r::GUI::Mcp
