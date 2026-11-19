#ifndef slic3r_Mcp_tools_project_LoadGuard_hpp_
#define slic3r_Mcp_tools_project_LoadGuard_hpp_

// What a project file (3MF) may bring in when the agent opens or imports it, and what slicing may
// run. The application's own check (the "Modified G-code" / "Customized Preset" question) relies on
// a list the file itself provides, skips "published" projects, and does not cover plate custom
// G-code, post-processing scripts, plug-ins or the printer's network settings. So after a load the
// tools compare what the file brought with the user's own presets:
//
//   code     G-code text (machine_start_gcode, filament_start_gcode, ...; any *_gcode or code option)
//            and plate custom G-code. Different from the user's presets -> the load is refused
//            (untrusted_gcode) unless the agent was allowed to trust it.
//   script   post_process: programs run on the G-code.
//   plugin   plug-in backed options (slicing_pipeline_plugin: Python run while slicing; printer_agent),
//            the "plugins" manifest and the *_plugin_config_overrides.
//   network  where the printer is and how to reach it: print_host, printhost_*, host_type, ...
//   script, plugin and network values are put back to the user's (reset_unsafe), in the edited
//   presets and in the presets that came with the file, so "discard changes" cannot bring them back.
//
// "The user's presets": every preset in the library before the load that did not itself come from a
// project file (not project-embedded, not external). A preset of the file is compared with its
// nearest ancestor (inherits) that is one, or with the defaults when there is none.
//
// Main thread only.

#include <map>
#include <set>
#include <string>
#include <vector>

#include "../../McpJson.hpp"

namespace Slic3r {

class Model;

namespace GUI { namespace Mcp { namespace LoadGuard {

enum class Kind { None, Code, Script, Plugin, Network };
Kind        kind_of(const std::string& key);
const char* kind_name(Kind kind);

// The library before a load.
struct Library
{
    std::set<std::string>              print, filament, printer;
    std::map<std::string, std::string> project_code; // G-code options of the project config, serialised
};
Library snapshot();

struct Finding
{
    std::string scope;  // "process", "printer", "filament", "plate", "object", "part", "layer range"
    std::string owner;  // preset, object or plate name
    std::string key;    // option key ("custom_gcode" for a plate's custom G-code)
    Kind        kind { Kind::None };
    int         slot { 0 }; // filament slot (1-based), plate (1-based); 0 when not applicable
};
json to_json(const std::vector<Finding>& findings);

// After a load: G-code of the selected presets, the project config and the plates' custom G-code
// that the user's presets do not have.
std::vector<Finding> foreign_code(const Library& library, const Model& model);

// After a load: puts script, plug-in and network values back to the user's. Returns what changed.
// May throw (a settings tab refusing the values).
std::vector<Finding> reset_unsafe(const Library& library);

// Removes code (unless `keep_code`), script, plug-in and network options from the configs of objects
// [first_object, end), their parts and layer ranges. Returns what was removed.
std::vector<Finding> strip_model(Model& model, size_t first_object, bool keep_code);

// Before slicing or exporting G-code: post-processing scripts or slicing plug-ins that the slice
// would run (BBL printers run post_process while slicing) but that the user has not made their own,
// i.e. that come from a preset of a project file or from an unsaved edit. Empty when there is none;
// otherwise {key, preset, reason} entries.
json untrusted_scripts();

}}} // namespace GUI::Mcp::LoadGuard

} // namespace Slic3r

#endif
