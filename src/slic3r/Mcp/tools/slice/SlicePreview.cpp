#include "SlicePreview.hpp"

#if __has_include("../../McpRenderer.hpp") && __has_include("../../McpPreviewRenderer.hpp")
#define MCP_SLICE_HAS_PREVIEW 1
#else
#define MCP_SLICE_HAS_PREVIEW 0
#endif

#if MCP_SLICE_HAS_PREVIEW

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Model.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"

#include "../../McpCamera.hpp"
#include "../../McpError.hpp"
#include "../../McpIds.hpp"
#include "../../McpPreviewRenderer.hpp"
#include "../../McpRenderer.hpp"
#include "../../McpSchema.hpp"
#include "SliceData.hpp"

namespace Slic3r { namespace GUI { namespace Mcp { namespace Slice {

namespace {

// Camera presets of the screenshot tool (McpRenderer.cpp), azimuth in its convention (90 = from the right).
struct Preset { const char* name; double az, el; };
const Preset PRESETS[] = {
    { "iso", 45., 35. },    { "iso_left", 315., 35. }, { "iso_back", 135., 35. }, { "iso_back_left", 225., 35. }, { "top", 0., 90. },
    { "bottom", 0., -90. }, { "front", 0., 0. },       { "back", 180., 0. },      { "left", 270., 0. },           { "right", 90., 0. },
};

const std::map<std::string, std::string>& view_types()
{
    static const std::map<std::string, std::string> m = {
        { "feature", "feature_type" },     { "filament", "tool" },          { "color_print", "color_print" },
        { "speed", "speed" },              { "actual_speed", "actual_speed" }, { "layer_height", "height" },
        { "line_width", "width" },         { "flow", "volumetric_flow" },   { "actual_flow", "actual_volumetric_flow" },
        { "layer_time", "layer_time" },    { "layer_time_log", "layer_time_log" }, { "fan_speed", "fan_speed" },
        { "temperature", "temperature" },  { "pressure_advance", "pressure_advance" }, { "acceleration", "acceleration" },
        { "jerk", "jerk" },
    };
    return m;
}

const std::map<std::string, std::string>& show_options()
{
    static const std::map<std::string, std::string> m = {
        { "travel", "travels" },           { "seams", "seams" },         { "retractions", "retractions" },
        { "deretractions", "deretractions" }, { "wipes", "wipes" },       { "tool_changes", "tool_changes" },
        { "color_changes", "color_changes" }, { "pauses", "pauses" },     { "custom_gcode", "custom_gcodes" },
    };
    return m;
}

std::string snake(const std::string& label)
{
    std::string out;
    for (char ch : label) {
        if (std::isalnum(static_cast<unsigned char>(ch)))
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        else if (!out.empty() && out.back() != '_')
            out += '_';
    }
    while (!out.empty() && out.back() == '_')
        out.pop_back();
    return out;
}

BoundingBoxf3 plate_footprint(int plate_idx)
{
    PartPlate*    plate = wxGetApp().plater()->get_partplate_list().get_plate(plate_idx);
    BoundingBoxf3 box = plate->get_build_volume();
    box.min.z() = 0.;
    box.max.z() = 0.;
    return box;
}

// Extrusions of the result inside the requested layers (0-based ids) or z range: what the picture shows.
BoundingBoxf3 toolpath_box(const GCodeProcessorResult& r, int layer_min, int layer_max, double z_min, double z_max)
{
    BoundingBoxf3 box;
    for (const auto& m : r.moves) {
        if (m.type != EMoveType::Extrude || m.internal_only)
            continue;
        const int l = static_cast<int>(m.layer_id);
        if (l < layer_min || l > layer_max)
            continue;
        if (m.position.z() < z_min - 1e-4 || m.position.z() > z_max + 1e-4)
            continue;
        box.merge(m.position.cast<double>());
    }
    return box;
}

void merge(BoundingBoxf3& into, const BoundingBoxf3& b)
{
    if (b.defined)
        into.merge(b);
}

// Float noise off the legend numbers (50.000003814697266 -> 50).
void round_numbers(json& doc, int digits)
{
    if (doc.is_number_float())
        doc = round_to(doc.get<double>(), digits);
    else if (doc.is_structured())
        for (json& v : doc)
            round_numbers(v, digits);
}

json objects_in_view(const CameraView& cam, int plate_idx)
{
    json         list = json::array();
    const Model& model = wxGetApp().plater()->model();
    for (int o = 0; o < static_cast<int>(model.objects.size()); ++o) {
        const ModelObject* mo = model.objects[static_cast<size_t>(o)];
        for (int i = 0; i < static_cast<int>(mo->instances.size()); ++i) {
            if (Ids::plate_of_instance(o, i) != plate_idx)
                continue;
            const BoundingBoxf3 b = mo->instance_bounding_box(static_cast<size_t>(i));
            Vec2d lo(1e18, 1e18), hi(-1e18, -1e18), px;
            bool  any = false;
            for (const Vec3d& c : box_corners(b.min, b.max))
                if (cam.project(c, px)) {
                    lo = lo.cwiseMin(px);
                    hi = hi.cwiseMax(px);
                    any = true;
                }
            Vec2d centre;
            if (!any || !cam.project(b.center(), centre))
                continue;
            if (hi.x() < 0 || hi.y() < 0 || lo.x() > cam.width || lo.y() > cam.height)
                continue;
            list.push_back(json::object({ { "id", Ids::of(*mo) },
                                          { "instance", Ids::of(*mo->instances[static_cast<size_t>(i)]) },
                                          { "name", mo->name },
                                          { "pixel", { std::round(centre.x()), std::round(centre.y()) } },
                                          { "pixel_box", { std::round(lo.x()), std::round(lo.y()), std::round(hi.x()), std::round(hi.y()) } } }));
        }
    }
    return list;
}

Picture render_preview(ToolContext& ctx, const ViewRequest& req)
{
    const json&  opt = req.options;
    const int    plate_idx = req.plate_idx;
    PlateHandles h = handles(plate_idx);
    if (plate_busy(plate_idx))
        fail("busy_slicing", "The plate is being sliced right now.", "Wait with slice_status, then take the picture.");
    if (!plate_has_result(plate_idx)) {
        json state = plate_state(plate_idx);
        fail("not_sliced", "Plate " + std::to_string(plate_idx + 1) + " has no valid slice result (state: " + state.value("state", "") + ").",
             "Call slice first; view \"editor\" shows the model without slicing.", state);
    }
    const GCodeProcessorResult& result = *h.result;

    PreviewRequest pr;
    pr.plate = plate_idx;
    pr.width = static_cast<unsigned>(req.width);
    pr.height = static_cast<unsigned>(req.height);
    const std::string color_by = opt.value("color_by", std::string("feature"));
    pr.view_type = view_types().at(color_by);
    pr.time_mode = opt.value("time_mode", std::string("normal"));

    // Layers (1-based) or z range.
    int    layer_min = 0, layer_max = std::numeric_limits<int>::max();
    double z_min = -1e9, z_max = 1e9;
    if (opt.contains("layers")) {
        const json& l = opt["layers"];
        if (l.contains("from")) { pr.layer_min = l["from"].get<int>(); layer_min = *pr.layer_min - 1; }
        if (l.contains("to")) { pr.layer_max = l["to"].get<int>(); layer_max = *pr.layer_max - 1; }
        if (pr.layer_min && pr.layer_max && *pr.layer_min > *pr.layer_max)
            fail("invalid_argument", "`preview.layers`: from must not exceed to.");
    }
    if (opt.contains("z_range")) {
        const json& z = opt["z_range"];
        if (z.contains("min")) { pr.z_min = z["min"].get<double>(); z_min = *pr.z_min; }
        if (z.contains("max")) { pr.z_max = z["max"].get<double>(); z_max = *pr.z_max; }
    }
    pr.top_layer_only = opt.value("section", false);
    pr.dim_lower_layers = opt.value("dim_lower_layers", false);
    if (pr.top_layer_only) {
        // Only the top layer is drawn: frame that layer.
        if (pr.layer_max)
            layer_min = layer_max;
    }
    if (opt.contains("show"))
        for (auto it = opt["show"].begin(); it != opt["show"].end(); ++it)
            pr.options[show_options().at(it.key())] = it.value().get<bool>();
    if (opt.contains("show_supports"))
        pr.show_supports = opt["show_supports"].get<bool>();
    if (opt.contains("show_wipe_tower"))
        pr.show_wipe_tower = opt["show_wipe_tower"].get<bool>();
    if (opt.contains("hide_features")) {
        const std::vector<std::string> keys = PreviewRenderer::feature_keys();
        for (const json& f : opt["hide_features"]) {
            std::string key = snake(f.get<std::string>());
            if (std::find(keys.begin(), keys.end(), key) == keys.end()) {
                json known = keys;
                fail("invalid_argument", "`preview.hide_features`: unknown feature \"" + f.get<std::string>() + "\".",
                     "Use a feature key or name as the legend shows it.", json::object({ { "features", known } }));
            }
            pr.features[key] = false;
        }
    }
    pr.show_bed = opt.value("show_bed", true);
    pr.show_shells = opt.value("show_shells", false);

    // Camera: the screenshot tool's conventions, computed with its CameraView.
    const CameraRequest& cr = req.camera;
    CameraView           cam;
    cam.width = req.width;
    cam.height = req.height;
    cam.perspective = !cr.orthographic;
    for (const Preset& p : PRESETS)
        if (cr.preset == p.name) {
            cam.azimuth_deg = p.az;
            cam.elevation_deg = p.el;
        }
    if (cr.has_azimuth)
        cam.azimuth_deg = cr.azimuth_deg;
    if (cr.has_elevation)
        cam.elevation_deg = cr.elevation_deg;
    const Vec3d         origin = h.plate->get_origin();
    const BoundingBoxf3 plate_box = plate_footprint(plate_idx);
    BoundingBoxf3       content = toolpath_box(result, layer_min, layer_max, z_min, z_max);
    if (!content.defined)
        content = toolpath_box(result, 0, std::numeric_limits<int>::max(), -1e9, 1e9);
    BoundingBoxf3 ids_box;
    if (cr.target == "ids" && cr.target_ids.is_array())
        for (const ItemRef& ref : Ids::items(cr.target_ids, "camera.target")) {
            if (ref.kind == IdKind::Plate)
                continue;
            if (ref.kind == IdKind::Instance)
                merge(ids_box, ref.object->instance_bounding_box(static_cast<size_t>(ref.inst_idx)));
            else
                for (size_t i = 0; i < ref.object->instances.size(); ++i)
                    merge(ids_box, ref.object->instance_bounding_box(i));
        }
    BoundingBoxf3 frame = plate_box;
    merge(frame, content);
    if (cr.target == "objects" && content.defined)
        frame = content;
    else if (cr.target == "ids" && ids_box.defined)
        frame = ids_box;
    cam.target = frame.center();
    cam.fit(box_corners(frame.min, frame.max), cr.zoom);
    if (cr.has_target_point)
        cam.target = origin + cr.target_point;
    if (cr.has_distance && cam.perspective)
        cam.distance = cr.distance;
    BoundingBoxf3 depth = plate_box;
    merge(depth, content);
    depth.min.z() = std::min(depth.min.z(), -1.);
    cam.set_depth_range(box_corners(depth.min, depth.max));
    cam.update();

    // Hand the renderer the same eye: orbit angles (its azimuth turns the other way), target,
    // distance and the scale at the target in pixels per mm.
    pr.camera.view.clear();
    double az = std::fmod(360. - cam.azimuth_deg, 360.);
    if (az < 0)
        az += 360.;
    pr.camera.azimuth_deg = az;
    pr.camera.elevation_deg = cam.elevation_deg;
    pr.camera.target = cam.target;
    pr.camera.projection = cam.perspective ? PreviewProjection::Perspective : PreviewProjection::Orthographic;
    pr.camera.fit = PreviewFit::None;
    if (cam.perspective) {
        pr.camera.distance = cam.distance;
        pr.camera.zoom = double(req.height) / (2. * cam.distance * std::tan(0.5 * cam.fov_y_deg * M_PI / 180.));
    } else {
        pr.camera.zoom = double(req.height) / cam.ortho_height;
    }

    // The renderer's GL objects must go before the canvases (McpPreviewRenderer.hpp). Without a hook
    // in GUI_App::shutdown, release them when the main window closes (a vetoed close only drops the cache).
    static bool shutdown_bound = false;
    if (!shutdown_bound && wxGetApp().mainframe != nullptr) {
        wxGetApp().mainframe->Bind(wxEVT_CLOSE_WINDOW, [](wxCloseEvent& event) {
            PreviewRenderer::instance().shutdown();
            event.Skip();
        });
        shutdown_bound = true;
    }

    const auto   start = std::chrono::steady_clock::now();
    PreviewImage img = PreviewRenderer::instance().render(pr);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    if (!img.ok) {
        const std::string code = img.error_code.empty() ? "internal" : img.error_code;
        fail(code == "render_failed" ? "gl_unavailable" : code, "Preview picture failed: " + img.error_message,
             code == "gl_unavailable" || code == "render_failed" ? "Bring the OrcaSlicer window on screen and retry." : "");
    }

    Picture picture;
    picture.mime_type = "image/png";
    picture.bytes = std::move(img.png);
    json info = json::object();
    info["size"] = json::array({ req.width, req.height });
    info["format"] = "png";
    info["plate"] = plate_idx + 1;
    info["camera"] = cam.to_json();
    info["legend"] = to_json(img.legend);
    round_numbers(info["legend"], 3);
    info["objects_in_view"] = objects_in_view(cam, plate_idx);
    json details = to_json(img);
    for (const char* key : { "layers", "moves", "visible_box", "z_range" })
        if (details.contains("info") && details["info"].contains(key))
            info[key] = details["info"][key];
    info["render_ms"] = round_to(ms, 1);
    if (req.format == "jpeg")
        ctx.note("The preview picture is always PNG.");
    picture.info = std::move(info);
    return picture;
}

} // namespace

bool register_preview_view()
{
    ViewPlugin plugin;
    plugin.name = "preview";
    plugin.description =
        "the sliced toolpaths of the plate (slice first), drawn offscreen without touching the Preview tab. preview.color_by picks the "
        "colouring (feature, filament, speed, flow, layer_time, fan_speed, temperature, layer_height, line_width ...); layers {from, to} "
        "(1-based, as the layer slider) or z_range limit what is drawn; section true draws only the top layer of the range (a cross-section; "
        "use camera preset top); show toggles travel, seams, retractions, wipes, tool changes; hide_features hides features by key "
        "(support, sparse_infill ...). The result's legend gives each colour's meaning (feature with time share, or the value range).";
    plugin.options_schema = Schema::parse(R"JSON({
        "type": "object",
        "properties": {
            "color_by": {"enum": ["feature", "filament", "color_print", "speed", "actual_speed", "layer_height", "line_width", "flow", "actual_flow",
                                  "layer_time", "layer_time_log", "fan_speed", "temperature", "pressure_advance", "acceleration", "jerk"], "default": "feature"},
            "layers": {"type": "object", "properties": {"from": {"type": "integer", "minimum": 1}, "to": {"type": "integer", "minimum": 1}}, "additionalProperties": false},
            "z_range": {"type": "object", "properties": {"min": {"type": "number"}, "max": {"type": "number"}}, "additionalProperties": false},
            "section": {"type": "boolean", "default": false, "description": "Only the top layer of the range."},
            "dim_lower_layers": {"type": "boolean", "default": false, "description": "Darken the layers below the top one."},
            "show": {"type": "object", "properties": {"travel": {"type": "boolean"}, "seams": {"type": "boolean"}, "retractions": {"type": "boolean"},
                     "deretractions": {"type": "boolean"}, "wipes": {"type": "boolean"}, "tool_changes": {"type": "boolean"}, "color_changes": {"type": "boolean"},
                     "pauses": {"type": "boolean"}, "custom_gcode": {"type": "boolean"}}, "additionalProperties": false},
            "hide_features": {"type": "array", "items": {"type": "string"}, "description": "Feature keys or names (\"support\", \"Sparse infill\")."},
            "show_supports": {"type": "boolean"},
            "show_wipe_tower": {"type": "boolean"},
            "show_bed": {"type": "boolean", "default": true},
            "show_shells": {"type": "boolean", "default": false, "description": "The objects, translucent."},
            "time_mode": {"enum": ["normal", "silent"], "default": "normal"}
        },
        "additionalProperties": false})JSON", "screenshot.preview");
    plugin.render = render_preview;
    RenderPlugins::add_view(std::move(plugin));
    return true;
}

}}}} // namespace Slic3r::GUI::Mcp::Slice

#else

namespace Slic3r { namespace GUI { namespace Mcp { namespace Slice {
bool register_preview_view() { return false; }
}}}} // namespace Slic3r::GUI::Mcp::Slice

#endif
