#ifndef slic3r_Mcp_SlicePreview_hpp_
#define slic3r_Mcp_SlicePreview_hpp_

// screenshot view "preview": pictures of a plate's sliced toolpaths, drawn offscreen by the
// G-code preview renderer (McpPreviewRenderer.hpp) with the screenshot tool's camera conventions
// (McpCamera.hpp), registered through RenderPlugins (McpRenderer.hpp). Nothing the user sees in the
// Preview tab changes.

namespace Slic3r { namespace GUI { namespace Mcp { namespace Slice {

// Registers the view when the renderer and the plug-in point are built in; returns whether it did.
bool register_preview_view();

}}}} // namespace Slic3r::GUI::Mcp::Slice

#endif
