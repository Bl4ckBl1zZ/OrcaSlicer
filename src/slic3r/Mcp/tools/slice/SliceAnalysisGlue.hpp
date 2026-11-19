#ifndef slic3r_Mcp_SliceAnalysisGlue_hpp_
#define slic3r_Mcp_SliceAnalysisGlue_hpp_

// The slice-analysis core (libslic3r/SliceAnalysis.hpp, branch mcp/analysis-core) behind a JSON
// interface for the tools, with a per-plate cache and the previous summary of every plate kept for
// before/after diffs. When the core is not compiled in, available() is false and every call fails
// with not_available.
//
// Frames and numbering in everything returned: XY (and boxes) in the plate's frame (the analysis
// core works in the frame of the G-code moves, which adds the plate's offset), layers 1-based as the
// preview's layer slider shows them.

#include <string>
#include <vector>

#include "../../McpJson.hpp"

#if __has_include("libslic3r/SliceAnalysis.hpp")
#define MCP_SLICE_HAS_ANALYSIS 1
#else
#define MCP_SLICE_HAS_ANALYSIS 0
#endif

namespace Slic3r { namespace GUI { namespace Mcp { namespace Slice { namespace Analysis {

bool available();
// Fails with not_available when the analysis core is not built in.
void require();

// Makes sure the plate's analysis is current (call after require_result()). Returns the time it took
// when it had to be computed (ms), 0 when cached.
double ensure(int plate_idx, const std::string& time_mode = "normal");
// Remember the plate's current summary as the "previous" one for diffs (called after each slice).
void remember(int plate_idx);

json summary(int plate_idx);
// Rows [from, to] (1-based, inclusive), every n-th; columns filter (empty = default set).
json layers(int plate_idx, int from, int to, int every, const std::vector<std::string>& columns, int limit);
// One named check with its parameters ({} = defaults). max_items caps every list.
json check(int plate_idx, const std::string& name, const json& params, int max_items);
// Cross-section at a layer (1-based) or z, optionally inside an XY box (plate frame).
json section(int plate_idx, int layer, double z, bool by_z, const json& box);
// Extrusions (or only supports) inside a box (plate frame; z range optional).
json box_query(int plate_idx, const json& box, const std::vector<std::string>& roles, bool support_only);
// Difference between the plate's previous summary and the current one.
json diff(int plate_idx, int max_items, double rel_tolerance);

// Plate frame <-> move frame and 0-based -> 1-based layers on a JSON document produced by the core.
json to_plate_frame(json doc, int plate_idx);

}}}}} // namespace Slic3r::GUI::Mcp::Slice::Analysis

#endif
