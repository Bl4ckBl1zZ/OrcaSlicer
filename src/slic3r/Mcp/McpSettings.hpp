#ifndef slic3r_Mcp_Settings_hpp_
#define slic3r_Mcp_Settings_hpp_

// What the settings tools share: the catalogue of options (who owns a key, where it may be set,
// what it accepts), the scopes a setting can live at, the wire format of values in both
// directions, and the access rules (secrets, keys that must not be written).
//
// Wire format: booleans, numbers and strings as JSON natives; percentages as "15%"; float-or-
// percent options as a number (absolute) or a "15%" string; enums as their profile string; vector
// options (per filament, per extruder) as arrays, with null for a nil entry of a nullable option.
// It is the serialised form the profiles on disk use, typed.
//
// Main thread only (it reads the preset bundle and the model).

#include <memory>
#include <string>
#include <vector>

#include "McpIds.hpp"
#include "McpJson.hpp"

namespace Slic3r {

class ConfigBase;
class ConfigOption;
class ConfigOptionDef;
class DynamicPrintConfig;
class ModelConfig;
class Preset;

namespace GUI { namespace Mcp {

namespace Settings {

// --- catalogue ---------------------------------------------------------------------------------

// Which preset type (or the project) holds a key.
enum class Owner { None, Process, Filament, Printer, Project };

const char* owner_name(Owner owner); // "process", "filament", "printer", "project", ""

// Definition of a key the settings tools know (one with an owner), or nullptr.
const ConfigOptionDef* find_def(const std::string& key);
Owner                  owner_of(const std::string& key);
// Every known key, in definition order.
const std::vector<std::string>& all_keys();

// May the key be overridden at these model scopes? (The key sets the GUI's override tabs use.)
bool allowed_on_object(const std::string& key);      // print options in PrintObjectConfig or PrintRegionConfig
bool allowed_on_volume(const std::string& key);      // PrintRegionConfig: parts and modifiers
bool allowed_on_layer_range(const std::string& key); // layer_height + PrintRegionConfig
bool allowed_on_plate(const std::string& key);       // the per-plate keys
// Scope names where the key can be set: "process" | "filament" | "printer" | "project", then
// "plate", "object", "part", "modifier", "layer_range" as applicable.
std::vector<std::string> scopes_of(const std::string& key);

// "filament" (one value per filament slot), "extruder" (one per extruder / nozzle), "list"
// (another vector), or "" for a scalar option.
std::string vector_kind(const std::string& key);

// Up to `count` known keys closest to `key`, for "did you mean" hints.
std::vector<std::string> nearest_keys(const std::string& key, size_t count = 5);

// The catalogue entry returned by settings_search:
// {key, label, category, type, unit, min, max, values, labels, default, mode, owner, scopes,
//  vector, tooltip, read_only?, write}.
json describe(const std::string& key, bool with_tooltip = true);

// --- access rules ---------------------------------------------------------------------------------

// Values that are never returned (API keys, passwords): reads give "<redacted>" when set.
bool is_secret(const std::string& key);
// Why the agent may not write the key, or an empty string. Keys that run code or move
// credentials are never writable; G-code text only when the user allowed it in Preferences
// (app_config "mcp_allow_gcode_edit").
std::string write_refusal(const std::string& key);

// --- values -----------------------------------------------------------------------------------------

// Option value -> wire value (redaction is the caller's business: see value_json_for()).
json value_json(const ConfigOption& option);
// config[key] -> wire value, redacted when the key is a secret; null when the key is absent.
json value_json_for(const ConfigBase& config, const std::string& key);

// Wire value -> a new option of the key's type. A scalar given for a vector option is repeated
// `vector_size` times (pass the size the option currently has). Throws ToolError: unknown_key,
// invalid_value (wrong type, not one of the enum values, wrong vector length), out_of_range.
std::unique_ptr<ConfigOption> option_from_json(const std::string& key, const json& value, size_t vector_size = 0);

// --- scopes -------------------------------------------------------------------------------------------

struct Scope
{
    enum class Kind { Process, Printer, Project, Filament, Plate, Object, Volume, LayerRange };
    Kind        kind { Kind::Process };
    int         filament_slot { -1 }; // 0-based
    int         plate_idx { -1 };
    ObjectRef   object;               // Object and LayerRange
    VolumeRef   volume;               // Volume
    double      z_min { 0. }, z_max { 0. };

    // "process", "filament:2", "plate:1", "object:o57", "volume:v77", "layer_range:0-5"
    std::string name() const;
    bool        is_preset() const { return kind == Kind::Process || kind == Kind::Printer || kind == Kind::Filament; }
    bool        is_model() const { return kind == Kind::Object || kind == Kind::Volume || kind == Kind::LayerRange; }
};

// Parses a Scope argument (schema fragment Scope) and resolves its ids. Throws stale_id /
// invalid_argument. A layer range that does not exist is stale_id unless `allow_missing_range`.
Scope parse_scope(const json& value, bool allow_missing_range = false);

// Is the key settable at the scope? When not, `why` gets a sentence and `where` the scope names
// that do take it.
bool key_allowed(const Scope& scope, const std::string& key, std::string* why = nullptr, std::vector<std::string>* where = nullptr);

// The configuration stored at the scope: the edited preset's config, the project config, the
// plate's own config, or the object's / volume's / range's overrides. Never null.
const ConfigBase& stored_config(const Scope& scope);
// The preset a preset scope refers to (edited state), or nullptr for other scopes.
const Preset*     preset_of(const Scope& scope);
// Keys that differ from the saved preset (preset scopes), or the keys present (other scopes).
std::vector<std::string> modified_keys(const Scope& scope);

// The value in force for the scope and where it comes from:
//   global configuration (process / filament / printer / project) <- plate <- object <- volume or
//   layer range. Returns {"value": ..., "source": "process" | "plate:1" | "object:o57" | ...}.
// It is computed from the same inputs slicing uses, before slicing; clamping done by the slicer
// afterwards is not applied.
json effective_value(const Scope& scope, const std::string& key, const DynamicPrintConfig& full_config);

} // namespace Settings

}}} // namespace Slic3r::GUI::Mcp

#endif
