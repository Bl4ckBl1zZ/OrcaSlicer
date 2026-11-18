#ifndef slic3r_Mcp_Scene_hpp_
#define slic3r_Mcp_Scene_hpp_

// The scene as JSON, in the one shape every tool uses: scene_get returns it, and mutating tools
// return the subtree they changed ("changed": ...) built with the same functions, so an agent
// rarely has to re-read the scene.
//
// Frames and units (docs/mcp/README.md): millimetres and degrees; object and instance positions
// are in the frame of their plate (world minus the plate origin): "center" is the bounding-box
// centre in XY, "z_min" the lowest point. Volume data is in the object's frame.
//
// Main thread only.

#include <string>

#include "McpJson.hpp"

namespace Slic3r {

class ConfigBase;
class ConfigOption;
class ModelObject;
class ModelVolume;

namespace GUI { namespace Mcp {

namespace Scene {

enum class Detail { Summary, Normal, Full };

// "summary" | "normal" | "full" -> Detail (anything else is Normal).
Detail detail_from(const std::string& name);

// The scene revision: changes with every undoable change by the agent or the user.
size_t rev();

// One object with its instances, volumes and layer ranges. `plate_idx` restricts the instances to
// those on that plate (-1: all, -2: only those on no plate).
json object_json(int obj_idx, Detail detail = Detail::Normal, int plate_idx = -1);
// One instance: {"id", "plate", "center", "z_min", "rotation", "scale", "bbox", "outside_plate"}.
json instance_json(int obj_idx, int inst_idx, Detail detail = Detail::Normal);
// One volume: {"id", "type", "name", "filament", "facets", "overrides", ...}.
json volume_json(const ModelObject& object, int vol_idx, Detail detail = Detail::Normal);
// One plate without its objects: {"plate", "id", "name", "current", "locked", "sliced", "overrides"}.
json plate_json(int plate_idx);
// {"name", "path", "dirty", "rev"}
json project_json();
// {"preset", "bed", "height", "extruders", "nozzle"}
json printer_json();
// {"preset", "modified"}
json process_json();
// [{"slot", "preset", "type", "color"}]
json filaments_json();
// Counts and names only: what mutating tools that touch everything (project load, preset switch)
// return.
json summary_json();

// "part", "negative_part", "modifier", "support_blocker", "support_enforcer", "precise_seam_*".
std::string volume_type_name(const ModelVolume& volume);

// Rounds to 4 decimals (0.1 um): keeps results short and stable.
double round4(double value);
json   vec3(double x, double y, double z);

} // namespace Scene

// A configuration value in wire form: booleans, numbers and strings as JSON natives, percentages
// as "15%", enums as their profile string, vector options as arrays with null for a nil entry.
json config_value_json(const ConfigOption& option);
// {"key": value, ...} for the keys present in `config`, skipping `skip_key` when given.
json config_overrides_json(const ConfigBase& config, const char* skip_key = nullptr);

}}} // namespace Slic3r::GUI::Mcp

#endif
