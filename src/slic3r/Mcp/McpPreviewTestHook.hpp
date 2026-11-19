#ifndef slic3r_Mcp_McpPreviewTestHook_hpp_
#define slic3r_Mcp_McpPreviewTestHook_hpp_

// Test hook for the offscreen G-code preview renderer (McpPreviewRenderer). It exists so the
// renderer can be exercised in the running application without the MCP server, and is replaced by
// the server's screenshot tool. It is compiled in but does nothing unless the environment variable
// ORCA_MCP_PREVIEW_TEST names a job file (JSON) when the application starts; see
// McpPreviewTestHook.cpp for the job format.

namespace Slic3r { namespace GUI { namespace Mcp {

class PreviewTestHook
{
public:
    // Called once from GUI_App::post_init(). Reads ORCA_MCP_PREVIEW_TEST; if it is unset or empty,
    // returns without doing anything. Otherwise schedules the job on the main thread.
    static void start_if_requested();
};

}}} // namespace Slic3r::GUI::Mcp

#endif // slic3r_Mcp_McpPreviewTestHook_hpp_
