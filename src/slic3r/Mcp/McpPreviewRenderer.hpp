#ifndef slic3r_Mcp_McpPreviewRenderer_hpp_
#define slic3r_Mcp_McpPreviewRenderer_hpp_

// Offscreen renderer of the sliced G-code preview, for the MCP server's screenshot tool.
//
// It draws a plate's slice result (GCodeProcessorResult) from any viewpoint into an offscreen
// framebuffer and returns the pixels, a PNG, the legend as data and the exact camera used. It owns a
// second libvgcode::Viewer, its own camera and its own plate drawing, so nothing the user sees is
// touched: not the camera, the active tab, the view type, the layer and move sliders, nor the
// visibility toggles of the Preview tab.
//
// Threading: call render() on the wx main thread only, from an event handler, CallAfter or timer -
// never from inside a canvas paint (GLCanvas3D::render). It makes the application's shared GL
// context current on the 3D editor canvas, so it works whichever tab is shown and while the window
// is minimised or covered.

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Point.hpp"

namespace Slic3r {
struct GCodeProcessorResult;
namespace GUI { namespace Mcp {

enum class PreviewProjection { Auto, Perspective, Orthographic };

// What the camera frames when no explicit zoom is given.
enum class PreviewFit {
    VisibleToolpaths, // the toolpaths drawn by this request (layer range, toggles applied)
    Toolpaths,        // every extrusion of the result
    Plate,            // the plate's build volume footprint and the print height
    Box,              // PreviewCameraSpec::box
    None              // keep the target and use zoom (default zoom 1 px/mm)
};

struct PreviewCameraSpec
{
    // Named view: "iso", "top", "bottom", "front", "back" (alias "rear"), "left", "right".
    // Empty: an orbit camera from azimuth_deg / elevation_deg.
    std::string view{ "iso" };
    // Orbit angles in degrees. Azimuth 0 looks from the front (from -Y towards +Y), 90 from the left
    // (-X), 180 from the back, 270 from the right; elevation 90 looks straight down.
    // "iso" is azimuth 45, elevation 45; "top" is 0/90; "bottom" is 0/-90 (image up = -Y).
    double azimuth_deg{ 45.0 };
    double elevation_deg{ 45.0 };
    // Point looked at. Default: the centre of the fit box (and the fit then also centres the picture).
    std::optional<Vec3d> target;
    // Eye to target distance in mm. It sets how strong the perspective is, not the scale of the
    // picture (that is the zoom). Default 1000, as the application's camera.
    std::optional<double> distance;
    PreviewProjection projection{ PreviewProjection::Auto }; // Auto: orthographic for the axis views, else perspective
    PreviewFit fit{ PreviewFit::VisibleToolpaths };
    BoundingBoxf3 box;                                       // for PreviewFit::Box
    double margin{ 0.04 };                                   // free border, fraction of the image size on each side
    std::optional<double> zoom;                              // pixels per mm at the target; overrides the fit's scale
};

struct PreviewRequest
{
    // Plate index (0-based); -1 = the current plate.
    int plate{ -1 };
    // Optional: render this result instead of the plate's (the plate still gives the bed and colours).
    const GCodeProcessorResult* result{ nullptr };

    unsigned int width{ 1024 };
    unsigned int height{ 768 };
    PreviewCameraSpec camera;

    // One of PreviewRenderer::view_type_names(): "feature_type", "height", "width", "speed",
    // "actual_speed", "fan_speed", "temperature", "volumetric_flow", "actual_volumetric_flow",
    // "layer_time", "layer_time_log", "tool", "color_print", "summary", "pressure_advance",
    // "acceleration", "jerk".
    std::string view_type{ "feature_type" };
    std::string time_mode{ "normal" }; // "normal" or "silent" (stealth), for times and layer time colours

    // Layer range, 1-based and inclusive as the layer slider shows it. A z range picks the layers
    // whose print z lies in it. Both unset: all layers.
    std::optional<int>    layer_min, layer_max;
    std::optional<double> z_min, z_max;
    // Part of the top layer's moves to draw, as fractions 0..1 of them (the moves slider). The lower
    // bound only applies with top_layer_only (the drawn moves are one contiguous range).
    std::optional<double> moves_from, moves_to;
    // Draw only the top layer of the range (a cross-section when seen from above).
    bool top_layer_only{ false };
    // Draw the layers below the top one darkened (focus on the top layer while keeping context).
    bool dim_lower_layers{ false };
    float dim_brightness{ 0.35f };

    // Options by name (PreviewRenderer::option_names()): travels, wipes, retractions, deretractions,
    // seams, tool_changes, color_changes, pauses, custom_gcodes. Unlisted ones keep the default
    // (seams on, everything else off).
    std::map<std::string, bool> options;
    // Extrusion roles by key (PreviewRenderer::feature_keys(), e.g. "support", "sparse_infill",
    // "prime_tower"). Unlisted ones are shown.
    std::map<std::string, bool> features;
    // Shortcuts that set several features: supports (support, support_interface, support_transition),
    // wipe_tower (prime_tower). Applied before `features`.
    std::optional<bool> show_supports, show_wipe_tower;

    bool show_bed{ true };          // the plate's footprint with a 10 mm grid
    bool show_shells{ false };      // the objects of the plate, translucent
    bool translucent_filaments{ false }; // draw translucent filament colours translucent (as the GUI option)
    std::array<float, 4> background{ { 0.93f, 0.93f, 0.93f, 1.0f } }; // RGBA 0..1; alpha 0 gives a transparent PNG

    int  samples{ -1 };     // MSAA samples; -1 = min(8, GL_MAX_SAMPLES), 0 or 1 = none
    bool encode_png{ true };
};

struct PreviewCameraInfo
{
    std::string projection;          // "perspective" or "orthographic"
    Vec3d position{ Vec3d::Zero() };
    Vec3d target{ Vec3d::Zero() };
    Vec3d up{ Vec3d::UnitZ() };
    Vec3d forward{ Vec3d::UnitY() };
    double azimuth_deg{ 0.0 };
    double elevation_deg{ 0.0 };
    double zoom{ 1.0 };              // pixels per mm at the target
    double fov_deg{ 0.0 };           // vertical field of view, perspective only
    double near_z{ 0.0 };
    double far_z{ 0.0 };
    std::array<int, 4> viewport{ { 0, 0, 0, 0 } };   // x, y, width, height in pixels
    Matrix4d view{ Matrix4d::Identity() };           // world -> eye
    Matrix4d projection_matrix{ Matrix4d::Identity() };
    Matrix4d view_projection{ Matrix4d::Identity() }; // projection * view, world -> clip
};

struct PreviewLegendItem
{
    std::string key;                          // feature key, option name or "filament_<n>"; empty for range steps
    std::string label;
    std::array<uint8_t, 3> color{ { 0, 0, 0 } };
    std::optional<double> value;              // range views: the value this colour stands for
    std::optional<double> time_s;             // estimated time, feature view
    std::optional<double> percent;            // share of the total time, feature view
    bool visible{ true };                     // drawn in this picture
};

struct PreviewLegend
{
    std::string view_type;
    std::string title;
    std::string kind;   // "categories" (feature, tool, colour print) or "range"
    std::string unit;
    std::optional<std::array<double, 2>> range; // range views: min and max
    bool logarithmic{ false };
    std::vector<PreviewLegendItem> items;     // categories; for ranges one item per palette colour, low to high
    std::vector<PreviewLegendItem> options;   // options drawn (travels, seams, ...) with their colours
};

struct PreviewImage
{
    bool ok{ false };
    std::string error_code;    // not_sliced, invalid_argument, gl_unavailable, stale_id, render_failed
    std::string error_message;

    unsigned int width{ 0 };
    unsigned int height{ 0 };
    std::vector<uint8_t> rgba; // width * height * 4, rows top to bottom
    std::string png;           // PNG bytes (empty when encode_png is false)

    PreviewCameraInfo camera;
    PreviewLegend legend;
    nlohmann::ordered_json info; // plate, layers, moves, boxes, cache and timing figures
};

class PreviewRenderer
{
public:
    // The renderer of the application. It is never destroyed (its GL objects can only be freed
    // with the context current): call shutdown() before the windows go away.
    static PreviewRenderer& instance();

    // Renders; never throws. Main thread only, not from inside a canvas paint.
    PreviewImage render(const PreviewRequest& request);

    // Frees the cached toolpaths and GL objects (the next render loads again). Main thread.
    void release();
    // release() and stop the idle timer; call at application shutdown before the canvases are destroyed.
    void shutdown();

    // Toolpaths are dropped after this many seconds without a render (default 300; 0 = never).
    void set_idle_release_seconds(int seconds);

    struct Stats
    {
        size_t renders{ 0 };
        size_t loads{ 0 };            // conversions + uploads of a result
        size_t cache_hits{ 0 };
        size_t releases{ 0 };
        bool   loaded{ false };
        int    plate{ -1 };
        unsigned int result_id{ 0 };
        size_t vertices{ 0 };
        size_t cpu_bytes{ 0 };        // libvgcode's own figures
        size_t gpu_bytes{ 0 };
        double last_load_ms{ 0.0 };
        double last_render_ms{ 0.0 };
    };
    Stats stats() const;

    static const std::vector<std::string>& view_type_names();
    static const std::vector<std::string>& option_names();
    static std::vector<std::string> feature_keys();

    ~PreviewRenderer();
    PreviewRenderer(const PreviewRenderer&) = delete;
    PreviewRenderer& operator=(const PreviewRenderer&) = delete;

private:
    PreviewRenderer();
    struct Impl;
    std::unique_ptr<Impl> p;
};

// JSON forms, for the MCP tool and the test hook. preview_request_from_json() accepts:
// {"plate", "size": {"width","height"}, "camera": {"view" | "azimuth_deg","elevation_deg", "target":[x,y,z],
//  "distance", "projection": "auto|perspective|orthographic", "fit": "visible|toolpaths|plate|box|none",
//  "box": {"min":[..],"max":[..]}, "margin", "zoom"}, "view_type", "time_mode", "layers": {"from","to"},
//  "z_range": {"min","max"}, "moves": {"from","to"}, "top_layer_only", "dim_lower_layers", "dim_brightness",
//  "options": {name: bool}, "features": {key: bool}, "show_supports", "show_wipe_tower", "show_bed",
//  "show_shells", "translucent_filaments", "background": [r,g,b(,a)] (0..1) or "#rrggbb(aa)", "samples"}
// and returns false with a message naming the offending field on a bad value.
bool preview_request_from_json(const nlohmann::ordered_json& j, PreviewRequest& out, std::string& error);
nlohmann::ordered_json to_json(const PreviewCameraInfo& camera);
nlohmann::ordered_json to_json(const PreviewLegend& legend);
// Everything but the pixels and the PNG.
nlohmann::ordered_json to_json(const PreviewImage& image);

}} // namespace GUI::Mcp
} // namespace Slic3r

#endif // slic3r_Mcp_McpPreviewRenderer_hpp_
