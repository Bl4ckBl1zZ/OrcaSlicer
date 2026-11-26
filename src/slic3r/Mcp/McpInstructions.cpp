#include "McpService.hpp"

namespace Slic3r { namespace GUI { namespace Mcp {

// Sent to every client in server/discover and initialize. Clients cut long instructions (Claude
// Code at 4096 characters), so this stays under 3000 and puts the workflow first. Tool names are
// the final catalogue; tools/list is the authority on what a given build has.
const char* server_instructions()
{
    return "OrcaSlicer control. Drives the running OrcaSlicer window: the user sees every change live and can undo it.\n"
           "\n"
           "Workflow: app_status -> scene_get -> change (model_import / object_* / settings_set / modifier_add / paint_apply ...) -> "
           "plate_validate -> slice -> slice_result / analyze / slice_layers -> screenshot -> export_file (kind gcode or gcode_3mf).\n"
           "Verify every change: screenshot view=editor (overlays show paint and overhangs) before slicing, view=preview after; then numbers: "
           "scene_get, settings_get effective=true, paint_get, slice_result, analyze (checks, and check region to compare inside vs outside a "
           "box), slice_layers detail=section. Do not rely on pictures alone.\n"
           "\n"
           "Conventions: mm, degrees, Z up, bed at z=0. Positions default to the frame of the object's plate (center = bounding-box centre in "
           "XY, z_min = lowest point); frame \"world\" is the whole bed, \"object\" the object's own coordinates (default for body placement in "
           "modifier_add / modifier_update, which also take center_rel 0..1 across the object's box). Ids: o=object, i=instance, v=volume "
           "(part, modifier or other body), pl=plate. Plates, filament slots (argument `filament`) and layers are 1-based. Ids stay valid until "
           "the project is reloaded; on stale_id call scene_get.\n"
           "\n"
           "Settings: settings_search finds keys and where each can be set. settings_set scope = \"process\" | \"printer\" | {\"filament\": n} | "
           "\"project\" | {\"plate\": n} | {\"object\": id} | {\"volume\": id} | {\"layer_range\": {object, z_min, z_max}}. Narrower scopes "
           "override wider ones. Prefer an object or modifier override to changing the process preset when only one model needs it.\n"
           "\n"
           "Local changes, cheapest first: (1) per-object or per-part setting; (2) a modifier body (modifier_add) or height range (layer_range); "
           "(3) paint (paint_apply) only when the region must follow the surface. Supports: enable_support plus support_type normal(manual) or "
           "tree(manual) to print only enforced areas; enforce/block with paint or support_enforcer / support_blocker bodies. Colour: filament "
           "on a part (object_update), a modifier with a filament, plate_layer_events for a change at a height, or paint_apply channel=color "
           "(filament_edit adds slots).\n"
           "\n"
           "Long operations (slice, arrange, orient, repair) wait up to wait_s, then return status \"running\" with a job: poll job_status "
           "(or slice_status). Any edit invalidates the slice; slice again before reading results or exporting.\n"
           "\n"
           "Errors are results with isError and {error: {code, message, hint}}: read the hint, fix the call, retry. ui_busy: the user is "
           "busy in the window; wait, retry.\n"
           "\n"
           "Undo: every change is one undo step named \"MCP: ...\" (history); preset, filament-slot and layer-event changes are in its "
           "journal and can mostly be reverted.\n"
           "\n"
           "Files: only inside the folders app_status lists.\n"
           "\n"
           "Printers (only if the user allowed it in Preferences): printer_list, printer_status; print_send asks the user in a "
           "confirmation window (a job) unless they switched that off. Do not retry a declined or timed-out send.";
}

}}} // namespace Slic3r::GUI::Mcp
