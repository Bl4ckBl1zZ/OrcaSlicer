#ifndef slic3r_Mcp_PaintStats_hpp_
#define slic3r_Mcp_PaintStats_hpp_

// Paint statistics as JSON for paint_apply / paint_get: per state area, share of the surface,
// triangle count and bounding boxes; connected painted regions; painted facet ids.

#include <string>

#include "libslic3r/FacetPaint.hpp"

#include "../../McpJson.hpp"
#include "PaintSelector.hpp"

namespace Slic3r { namespace GUI { namespace Mcp { namespace Paint {

// "support" | "seam" | "color" | "fuzzy_skin"
bool                parse_channel(const std::string& name, FacetPaint::Channel& out);
const char*         channel_name(FacetPaint::Channel channel);
// State names: support/seam "enforce" / "block", fuzzy skin "fuzzy", colour "filament_<n>"; "none".
std::string         state_name(FacetPaint::Channel channel, EnforcerBlockerType state);
// Parses a state argument ("enforce", "block", "fuzzy", "none" or {"filament": n}) for a channel.
// Throws ToolError(invalid_argument) when the channel does not accept it. `filament_count` checks
// colour states (actionable error naming filament_edit).
EnforcerBlockerType parse_state(FacetPaint::Channel channel, const json& value, const char* what, int filament_count);

// {"<state>": {"area_mm2", "fraction", "triangles", "box": {min,max} (plate frame), "box_object": {min,max}}, ...}
json states_json(FacetPaint::Channel channel, const FacetPaint::Stats& stats);

// Connected painted regions of one channel of one part, largest first, at most `limit`:
// [{state, area_mm2, triangles, box (plate), box_object, centroid (plate), mean_normal (plate)}].
// `total` receives the number of regions before the limit.
json components_json(const PaintTarget& target, FacetPaint::Channel channel, size_t limit, size_t& total);

// Original facets that carry each painted state (wholly or partly), at most `limit` per state:
// {"<state>": {"count": n, "ids": [...], "truncated": bool}}.
json facets_json(const PaintTarget& target, FacetPaint::Channel channel, size_t limit);

// Box JSON {min: [..], max: [..]} rounded to 0.001 mm, or null.
json box_json(const BoundingBoxf3& box);

}}}} // namespace Slic3r::GUI::Mcp::Paint

#endif
