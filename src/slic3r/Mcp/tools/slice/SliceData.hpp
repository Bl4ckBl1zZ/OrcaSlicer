#ifndef slic3r_Mcp_SliceData_hpp_
#define slic3r_Mcp_SliceData_hpp_

// Reading a plate's slicing state and results for the slice tools: state, validation, warnings,
// summary figures straight from the slicer's own statistics, and the G-code header of an exported
// file. Main thread only; callers refuse while the plate is being sliced (busy_slicing).

#include <set>
#include <string>
#include <vector>

#include "../../McpJson.hpp"

namespace Slic3r {
class Print;
class ObjectBase;
struct GCodeProcessorResult;
namespace GUI {
class PartPlate;
namespace Mcp {
class ToolContext;
namespace Slice {

struct PlateHandles
{
    PartPlate*            plate { nullptr };
    Print*                print { nullptr };
    GCodeProcessorResult* result { nullptr };
    int                   index { -1 };
};

PlateHandles handles(int plate_idx);
int          plate_count();
int          current_plate();

// True while the application slices (any plate).
bool slicing_now();
// The plate is the one being sliced right now.
bool plate_busy(int plate_idx);

// {"id": "o12", "name": ...} for a model object known to the Print or the Model (by ObjectID); null if unknown.
json object_ref(const ObjectBase* object);
json object_ref_by_id(size_t object_id, const Print* print);

// {"plate", "id", "state": empty|not_sliced|slicing|sliced|invalidated|failed|cancelled, "reason"?, ...}
json plate_state(int plate_idx);
bool plate_has_result(int plate_idx);

// Pre-slice checks of a plate. errors: [{message, object?, key?, kind, fix?}], warnings likewise.
// Only the current plate is validated by the Print; others get the geometric checks.
json validation(int plate_idx, bool refresh, bool& has_errors);

// Warnings and problems of a sliced plate: slicer step warnings (per object), G-code processor
// warnings, toolpaths outside the plate, object collisions, nozzle / filament area or height issues.
json slice_warnings(int plate_idx);

// Summary figures from the slicer's statistics. sections: summary, filaments, features, objects, warnings, layers.
json result_json(int plate_idx, const std::set<std::string>& sections, const std::string& time_mode);
// Compact form for slice / slice_status results.
json compact_summary(int plate_idx);

// The statistics the G-code header and footer comments carry (time, filament, layers).
json gcode_file_info(const std::string& path);

std::string format_duration(double seconds);
double      round_to(double v, int digits);

// export_file kinds "gcode" / "gcode_3mf": copies the plate's sliced G-code (post-processing scripts are not
// run) or writes a .gcode.3mf. Reads kind, path, plate (or "all"), overwrite from ctx. Defined in ToolsSlice.cpp.
json export_gcode(ToolContext& ctx);

}}}} // namespace Slic3r::GUI::Mcp::Slice

#endif
