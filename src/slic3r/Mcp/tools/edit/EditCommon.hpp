#ifndef slic3r_Mcp_EditCommon_hpp_
#define slic3r_Mcp_EditCommon_hpp_

// Helpers shared by the edit group (ToolsProject.cpp, ToolsObjects.cpp, ToolsPlates.cpp).
// Main thread only unless a function says otherwise.

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "libslic3r/Point.hpp"

#include "../../McpJson.hpp"
#include "../../McpTool.hpp"

namespace Slic3r {

class Model;
class ModelObject;
class ModelInstance;
class ModelVolume;
class AppConfig;

namespace GUI {

class Plater;
class PartPlateList;

namespace Mcp { namespace Edit {

Plater&        plater();
Model&         model();
PartPlateList& plates();

// World position of a plate's origin (its front-left corner); plate 0 for an index out of range.
Vec3d plate_origin(int plate_idx);
// World position of the centre of a plate's bed, at z = 0.
Vec3d plate_center(int plate_idx);
// Bed size of one plate (x, y) and printable height.
Vec3d plate_size();

// Index of an object in Model::objects by its ObjectID, or -1.
int object_index_by_id(size_t id);
// ObjectIDs of all objects, to find the ones a GUI function added.
std::set<size_t> object_id_set();
// Indices of objects whose id is not in `before`, in model order.
std::vector<int> new_object_indices(const std::set<size_t>& before);

// After a structural edit made directly on the Model (objects, instances or volumes added or removed):
// rebuild the plate membership of every instance from its position, then the full GUI resync.
// Refresh::full() alone keeps the plates' (object index, instance index) lists, which go stale when
// indices shift.
void refresh_structure();

// [object_json(idx)...] in scene_get shape.
json objects_json(const std::vector<int>& obj_idxs);

// True when nothing would be disturbed by changing the Model from a timer: the top-level event loop
// runs (no modal dialog, no wxYield of a progress dialog) and no project is loading.
bool main_loop_idle();

// Registers `poll`, called about ten times a second from the main loop (only while main_loop_idle())
// until it returns true. Used to finish work that completes after the tool call returned: arrange,
// orient, mesh jobs running on a worker thread.
void watch(std::function<bool()> poll);

// Sets app_config keys for the duration of a scope and puts the old values back afterwards.
class ScopedAppConfig
{
public:
    ScopedAppConfig() = default;
    ~ScopedAppConfig();
    ScopedAppConfig(const ScopedAppConfig&) = delete;
    ScopedAppConfig& operator=(const ScopedAppConfig&) = delete;
    void set(const std::string& key, const std::string& value);
    void set(const std::string& section, const std::string& key, const std::string& value);

private:
    struct Saved { std::string section, key, value; };
    std::vector<Saved> m_saved;
    bool               m_any { false };
};

// Runs pending events now (the deferred dialogs of show_error() and the like) so the handler can
// look at what was intercepted before it decides the result. The dispatcher does the same after the
// handler returns; this is the same work done earlier, under the same dialog guard and snapshot.
void flush_pending_events();

// Error dialogs raised during the call (title or text mentions failure); their texts.
std::vector<std::string> error_dialog_texts(const ToolContext& ctx);

// Path in UTF-8 for results.
std::string path_utf8(const std::string& native_or_utf8);

// A short "o12 o14 ..." list for undo names.
std::string ids_summary(const std::vector<std::string>& ids, size_t max_items = 3);

}}}} // namespace Slic3r::GUI::Mcp::Edit

#endif
