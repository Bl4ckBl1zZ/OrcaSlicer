#include "McpService.hpp"

namespace Slic3r { namespace GUI { namespace Mcp {

// Sent to every client in server/discover and initialize. Clients cut long instructions (Claude
// Code at 4096 characters), so this stays under 3000 and puts the workflow first. It names the
// tools of the full catalogue; tools/list is the authority on what a given build has.
const char* server_instructions()
{
    return "OrcaSlicer control. Drives the running OrcaSlicer window: the user sees every change live and can undo it.\n"
           "\n"
           "Workflow: app_status -> scene_get -> make changes -> plate_validate -> slice -> slice_result / analyze -> screenshot to verify -> export_file. "
           "Only the tools in tools/list exist in this build; if a step's tool is missing, say so instead of guessing.\n"
           "Look before and after: screenshot view=editor for the model, view=preview after slicing. Verify with numbers "
           "(scene_get, settings_get, slice_result, analyze, paint_get), not only pictures.\n"
           "\n"
           "Conventions: mm, degrees, Z up, bed at z=0. Object positions are in the frame of their plate (center = bounding-box centre in XY, "
           "z_min = lowest point). Volume (part/modifier) placement is in the object's frame, or 0..1 across its bounding box with center_rel. "
           "Ids: o=object, i=instance, v=volume (part or modifier), pl=plate; plates also by 1-based number. Filament slots are 1-based. "
           "Ids are stable until the project is reloaded; on stale_id call scene_get.\n"
           "\n"
           "Settings: one API. settings_search finds keys and says where each can be set. settings_set scope = \"process\" | \"printer\" | "
           "{\"filament\": n} | \"project\" | {\"plate\": n} | {\"object\": id} | {\"volume\": id} | {\"layer_range\": {...}}. Narrower scopes "
           "override wider ones; settings_get effective=true shows the value in force and where it comes from. Prefer an object or modifier "
           "override to changing the process preset when only one model needs it.\n"
           "\n"
           "Local changes, cheapest first: (1) per-object or per-part setting; (2) modifier body (modifier_add) or height range (layer_range) "
           "for a region; (3) paint (paint_apply) only when the region must follow the surface. Supports: enable in settings, then enforce or "
           "block with paint or support_enforcer / support_blocker bodies. Colour: assign a filament to a part, or a modifier with a filament, "
           "or plate_layer_events for a change at a height, or paint_apply channel=color.\n"
           "\n"
           "Long operations (slice, arrange) return status \"running\" with a job id when not finished in wait_s; then call job_status "
           "(or slice_status). Editing after slicing invalidates the result.\n"
           "\n"
           "Errors are results with isError and {error: {code, message, hint}}: read the hint, fix the call, retry. ui_busy means the user "
           "is in the middle of something (a dialog, a drag): wait and retry.\n"
           "\n"
           "Undo: every change is one undo step named \"MCP: ...\" (history tool). Preset and filament changes are not in the undo stack; "
           "history lists them and can revert them.\n"
           "\n"
           "Files: only inside the folders listed by app_status. This server cannot send anything to a printer.";
}

}}} // namespace Slic3r::GUI::Mcp
