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
// rebuilds the 3D scene and the object list from the Model and restarts the background process.
// Use it after edits that keep the set of objects and instances as it is (volumes added, removed,
// retyped or renamed; names, printable flags, filaments) and whenever the targeted call is not
// obviously sufficient. It does NOT recompute which plate an instance is on: undo/redo restores
// the plates itself, so the application's function leaves them alone.
void full();

// full(), preceded by recomputing plate membership for every instance from its position
// (PartPlateList::reload_all_objects). Use it after structural edits that add, remove or reorder
// objects or instances: the plates hold (object index, instance index) pairs, which such an edit
// invalidates. Without it new instances show up under "off_plate" or on the wrong plate.
void structure();

} // namespace Refresh

}}} // namespace Slic3r::GUI::Mcp

#endif
