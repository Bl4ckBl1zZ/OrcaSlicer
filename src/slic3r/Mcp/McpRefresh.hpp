#ifndef slic3r_Mcp_Refresh_hpp_
#define slic3r_Mcp_Refresh_hpp_

// The two ways a tool brings the window, the object list, the plates and the background slicer
// back in sync after editing the Model directly. Main thread only.

#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

namespace Refresh {

// Targeted: one object changed (transform, mesh, per-object data). Drops the object to the bed,
// reloads its volumes in the 3D scene, schedules the background process and recomputes which plate
// its instances are on. Also refreshes the object's info rows (paint, layer ranges, settings).
void object(int obj_idx);
void objects(const std::vector<int>& obj_idxs);

// Full resynchronisation, what the application does after undo/redo replaced the whole model:
// rebuilds the 3D scene, the object list and the plate assignment from the Model. Heavier but
// always correct; use it after structural edits (objects, instances or volumes added, removed,
// retyped or renamed) and whenever the targeted call is not obviously sufficient.
void full();

} // namespace Refresh

}}} // namespace Slic3r::GUI::Mcp

#endif
