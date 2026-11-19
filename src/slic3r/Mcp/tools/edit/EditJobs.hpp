#ifndef slic3r_Mcp_EditJobs_hpp_
#define slic3r_Mcp_EditJobs_hpp_

// Long-running operations of the edit group, each reported through the core's jobs registry:
//   arrange   the application's own ArrangeJob on the UI worker (settings set for the call)
//   orient    orientation::orient on a worker thread of ours, committed on the main thread
//   simplify  its_quadric_edge_collapse on copies of the meshes, committed on the main thread
//   repair    repair_mesh (CGAL) on copies of the meshes, committed on the main thread
// Commits check that the target still exists with the same mesh; otherwise the job fails with
// `conflict` and nothing is changed.

#include "../../McpTool.hpp"

namespace Slic3r { namespace GUI { namespace Mcp { namespace Edit {

json start_arrange(ToolContext& ctx);
json start_orient(ToolContext& ctx);
// op: "simplify" or "repair". Volumes are given as (object index, volume index) pairs.
json start_mesh_job(ToolContext& ctx, const std::string& op, const std::vector<std::pair<int, int>>& volumes);

}}}} // namespace Slic3r::GUI::Mcp::Edit

#endif
