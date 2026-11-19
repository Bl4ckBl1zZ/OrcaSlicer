#ifndef slic3r_Mcp_Renderer_hpp_
#define slic3r_Mcp_Renderer_hpp_

// Offscreen pictures for the `screenshot` tool. Everything is drawn into a framebuffer object
// with a camera of our own (McpCamera.hpp): the window, the user's camera, tab, selection and
// view options are neither read nor changed, and nothing is presented on screen.
//
// The editor pass draws a plain bed (plate rectangle, 10 mm grid, axes) and the scene's volumes
// through GLVolume::simple_render with the "thumbnail" shader - the path the application's own
// thumbnail generator uses offscreen - so colour painting shows as in the editor.
//
// A pass for other content (sliced toolpaths) plugs in through RenderTarget: it gets the bound
// framebuffer, the camera and a place to put its legend.
//
// Main thread only; needs the GL context of the 3D editor canvas.

#include <functional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "libslic3r/BoundingBox.hpp"

#include "McpCamera.hpp"
#include "McpJson.hpp"
#include "McpTool.hpp"

namespace Slic3r { namespace GUI { namespace Mcp {

// The validated "camera" argument of a screenshot (schema fragment Camera).
struct CameraRequest
{
    std::string preset { "iso" };
    bool        has_azimuth { false };
    double      azimuth_deg { 0. };
    bool        has_elevation { false };
    double      elevation_deg { 0. };
    bool        orthographic { false };
    double      zoom { 1. };
    bool        has_distance { false };
    double      distance { 0. };
    // Target: "plate" (default), "objects", an explicit point (plate frame), or ids.
    std::string target { "plate" };
    bool        has_target_point { false };
    Vec3d       target_point { Vec3d::Zero() };
    json        target_ids; // array of ids, or null

    // Reads a Camera object; throws ToolError(invalid_argument) on inconsistent values.
    static CameraRequest from_json(const json& value);
};

struct EditorRequest
{
    int           plate_idx { 0 };
    int           width { 1024 };
    int           height { 768 };
    CameraRequest camera;
    std::string   color_by { "filament" }; // "filament" | "object" | "type"
    bool          show_bed { true };
    bool          show_modifiers { true };
    json          only;        // ids to draw exclusively, or null
    json          highlight;   // ids drawn in the highlight colour, or null
    json          transparent; // ids drawn translucent, or null
    std::string   overlay;     // name of a registered OverlayPlugin, or empty
    json          options;     // the whole "editor" argument, passed to the overlay
    std::string   format { "png" }; // "png" | "jpeg"
};

struct Picture
{
    std::string mime_type;
    std::string bytes;
    // {"size": [w, h], "camera": {...}, "objects_in_view": [...], "legend": {...}}
    json        info;
};

class Renderer
{
public:
    // Renders the editor scene of one plate. Throws ToolError: gl_unavailable, stale_id,
    // invalid_argument.
    static Picture render_editor(const EditorRequest& request);

    // What a custom pass receives. The framebuffer is bound and cleared, the viewport set, depth
    // testing on; the pass must leave the framebuffer bound.
    struct RenderTarget
    {
        const CameraView* camera { nullptr };
        int               plate_idx { 0 };
        Vec3d             plate_origin { Vec3d::Zero() };
        json*             legend { nullptr };
    };
    // Renders with a caller-supplied pass in place of the scene volumes (used for the G-code
    // preview). `scene_box` is the world box the pass will fill, used to fit the camera and to
    // set the depth range. The bed is drawn first when `show_bed`.
    static Picture render_custom(int plate_idx, int width, int height, const CameraRequest& camera, const BoundingBoxf3& scene_box, bool show_bed,
                                 const std::string& format, const std::function<void(const RenderTarget&)>& pass);

    // The screenshot camera for a request: preset / angles, framing of `plate_box` + `content_box`
    // (or of `content_box` for target "objects", `ids_box` for target ids, a point for target
    // [x,y,z] in plate coordinates), zoom, distance and a depth range covering all three boxes.
    // Every view that wants the screenshot tool's camera conventions uses this.
    static CameraView camera_for(const CameraRequest& request, int width, int height, const Vec3d& plate_origin, const BoundingBoxf3& plate_box,
                                 const BoundingBoxf3& content_box, const BoundingBoxf3& ids_box);
};

// ---------------------------------------------------------------------------------------------
// Extension points of the `screenshot` tool, so that other tool groups can add to it without
// editing it. Register from your group's MCP_TOOL_GROUP function. The group name must sort
// before "View" (groups register alphabetically and the screenshot tool builds its schema from
// what is registered by then).
// ---------------------------------------------------------------------------------------------

// A whole view, selected with `view: "<name>"` (the built-in one is "editor"). Its options are the
// tool argument of the same name, validated against `options_schema` before `render` is called.
struct ViewRequest
{
    int           plate_idx { 0 };
    int           width { 1024 };
    int           height { 768 };
    CameraRequest camera;
    std::string   format { "png" };
    json          options; // the object given as args[<name>], or an empty object
};

struct ViewPlugin
{
    std::string name;           // "preview"
    std::string description;    // one sentence for the tool description
    json        options_schema; // object schema of the options
    std::function<Picture(ToolContext&, const ViewRequest&)> render;
};

// Extra geometry drawn over the editor scene, selected with `editor.overlay: "<name>"`
// ("support_paint", "overhang" ...). `draw` runs after the volumes, with the framebuffer bound.
struct OverlayPlugin
{
    std::string name;
    std::string description;
    // Extra properties of the `editor` argument this overlay reads ({name: schema}), merged into the
    // screenshot schema (e.g. "overhang_threshold_deg"). Names must not clash with other overlays'.
    json        options_schema = json::object();
    std::function<void(const Renderer::RenderTarget&, const json& editor_options)> draw;
};

class RenderPlugins
{
public:
    // Adding a plugin with a name that exists replaces it.
    static void add_view(ViewPlugin plugin);
    static void add_overlay(OverlayPlugin plugin);
    static const std::vector<ViewPlugin>&    views();
    static const std::vector<OverlayPlugin>& overlays();
    static const ViewPlugin*    find_view(const std::string& name);
    static const OverlayPlugin* find_overlay(const std::string& name);
};

}}} // namespace Slic3r::GUI::Mcp

#endif
