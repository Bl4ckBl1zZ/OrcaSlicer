#ifndef slic3r_Mcp_PaintDebug_hpp_
#define slic3r_Mcp_PaintDebug_hpp_

// Test-only tools of the paint group (debug_only: registered with ORCA_MCP_DEBUG_TOOLS=1).

namespace Slic3r { namespace GUI { namespace Mcp {
class ToolRegistry;
namespace Paint {

void register_debug_tools(ToolRegistry& registry);

}}}} // namespace Slic3r::GUI::Mcp::Paint

#endif
