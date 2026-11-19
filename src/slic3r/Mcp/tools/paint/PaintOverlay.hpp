#ifndef slic3r_Mcp_PaintOverlay_hpp_
#define slic3r_Mcp_PaintOverlay_hpp_

// Editor screenshot overlays of the paint group, registered with the core's RenderPlugins
// (McpRenderer.hpp): "support_paint", "seam_paint", "color_paint", "fuzzy_paint" draw a channel's
// painted states in flat colours over the scene; "overhang" tints faces steeper than a threshold.
// Compiled to a no-op while the core's renderer is not in the tree.

namespace Slic3r { namespace GUI { namespace Mcp { namespace Paint {

void register_overlays();
// True when this build has the overlays (the core renderer was present at compile time).
bool overlays_available();

}}}} // namespace Slic3r::GUI::Mcp::Paint

#endif
