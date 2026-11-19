#ifndef slic3r_Mcp_tools_settings_SettingsCommon_hpp_
#define slic3r_Mcp_tools_settings_SettingsCommon_hpp_

// Helpers shared by the settings group (settings_set, presets, filaments, modifiers, layer ranges):
// where a key may be written, which keys are never writable, parsing an agent's JSON values into a
// configuration with errors the agent can act on, writing model-level configs (object, part,
// modifier, layer range) and bringing the GUI in sync afterwards.
//
// Main thread only.

#include <string>
#include <vector>

#include "../../McpJson.hpp"

namespace Slic3r {

class DynamicPrintConfig;
class ModelConfig;
class ModelObject;
class ModelVolume;

namespace GUI { namespace Mcp { namespace SettingsEdit {

// Where a setting is written.
enum class Where { Process, Printer, Filament, Project, Plate, Object, Part, Modifier, LayerRange };

// "process", "printer", "filament", "project", "plate", "object", "part", "modifier", "layer_range".
const char* where_name(Where where);

// True when `key` may be written at `where`. Model scopes follow the key sets of the GUI's override
// tabs (TabPrintObject / TabPrintPart / TabPrintLayer / TabPrintPlate), preset scopes the preset
// type's own option list.
bool key_allowed(const std::string& key, Where where);

// The scope names where `key` can be written, in the order of Where.
std::vector<std::string> scopes_of(const std::string& key);

// Fails with unknown_key (with the nearest keys), forbidden_key or key_not_in_scope (with the scopes
// that do take the key).
void check_key(const std::string& key, Where where);

// Never writable through MCP (DESIGN.md section 4): post-processing scripts, print-host
// credentials, the printer agent, plugin-backed options, and G-code text unless the user enabled
// the `mcp_allow_gcode_edit` preference. Returns the reason, or an empty string.
std::string forbidden_reason(const std::string& key);

// One validated value in the serialised form the profiles use.
struct Parsed
{
    std::string key;
    std::string text;
};

// Converts an agent's {key: value} object into serialised values, validating each against the
// option definition: type, enum (the error lists the accepted values), min/max, vector length.
// `reference` supplies the current length of vector options (a scalar is applied to every entry);
// it may be null. Fails before anything is written.
std::vector<Parsed> parse_values(const json& values, Where where, const DynamicPrintConfig* reference);

// The value of `key` in `config` in wire form, or null when absent.
json value_json(const DynamicPrintConfig& config, const std::string& key);

// Applies `values` (an object, may be null) and `reset` (array of keys, may be null) to a model-level
// config. Validation happens first; nothing is written on failure. Returns [{key, old, new}] with
// null for "not overridden". Duplicate keys between values and reset fail with invalid_argument.
json apply_to_model_config(ModelConfig& config, Where where, const json& values, const json& reset);

// After changing the config of an object (volume == nullptr) or of one of its volumes: the settings
// rows of the object list, the per-object settings panel, the 3D scene and the background process.
void refresh_after_model_config(int obj_idx, ModelObject* object, ModelVolume* volume);

// After changing an object's layer ranges: rebuilds the object list (layer rows) and refreshes.
void refresh_after_layer_ranges(int obj_idx);

// The full print config the slicer would see now (presets + project config).
DynamicPrintConfig full_config();

}}}} // namespace Slic3r::GUI::Mcp::SettingsEdit

#endif
