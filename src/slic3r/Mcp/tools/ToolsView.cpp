// View tools: screenshot.
//
// The group is called "View" on purpose: groups register in alphabetical order, and this one
// builds the schema of `screenshot` from the views and overlays other groups registered before it
// (RenderPlugins in McpRenderer.hpp).

#include <string>
#include <vector>

#include "../McpIds.hpp"
#include "../McpRenderer.hpp"
#include "../McpSchema.hpp"
#include "../McpTool.hpp"

namespace Slic3r { namespace GUI { namespace Mcp {

namespace {

json screenshot(ToolContext& ctx)
{
    const json&       args = ctx.args();
    const std::string view = ctx.arg("view", "editor");
    const int         plate_idx = Ids::plate(args.contains("plate") ? args["plate"] : json(nullptr));
    const json        size = args.contains("size") ? args["size"] : json(json::object());
    const int         width = size.value("width", 1024);
    const int         height = size.value("height", 768);
    const std::string format = ctx.arg("format", "png");
    const CameraRequest camera = CameraRequest::from_json(args.contains("camera") ? args["camera"] : json(nullptr));

    Picture picture;
    if (view == "editor") {
        const json options = args.contains("editor") ? args["editor"] : json(json::object());
        EditorRequest request;
        request.plate_idx = plate_idx;
        request.width = width;
        request.height = height;
        request.camera = camera;
        request.format = format;
        request.color_by = options.value("color_by", std::string("filament"));
        request.show_bed = options.value("show_bed", true);
        request.show_modifiers = options.value("show_modifiers", true);
        request.only = options.contains("only") ? options["only"] : json(nullptr);
        request.highlight = options.contains("highlight") ? options["highlight"] : json(nullptr);
        request.transparent = options.contains("transparent") ? options["transparent"] : json(nullptr);
        const std::string overlay = options.value("overlay", std::string("none"));
        request.overlay = overlay == "none" ? std::string() : overlay;
        request.options = options;
        picture = Renderer::render_editor(request);
    } else {
        const ViewPlugin* plugin = RenderPlugins::find_view(view);
        if (plugin == nullptr || !plugin->render)
            fail("invalid_argument", "`view`: there is no view \"" + view + "\" in this build.", "Use view \"editor\".");
        ViewRequest request;
        request.plate_idx = plate_idx;
        request.width = width;
        request.height = height;
        request.camera = camera;
        request.format = format;
        request.options = args.contains(view) ? args[view] : json(json::object());
        if (const std::string problem = Schema::validate(Schema::with_shared_defs(plugin->options_schema), request.options); !problem.empty())
            fail("invalid_argument", "`" + view + "`: " + problem, "The tool's input schema lists the options of this view.");
        picture = plugin->render(ctx, request);
    }

    json result = picture.info.is_object() ? picture.info : json(json::object());
    result["view"] = view;
    ctx.add_image(picture.mime_type, std::move(picture.bytes));
    return result;
}

} // namespace

MCP_TOOL_GROUP(View)
{
    json schema = Schema::parse(R"JSON({
        "type": "object",
        "properties": {
            "view": {"enum": ["editor"], "default": "editor"},
            "plate": {"$ref": "#/$defs/PlateRef"},
            "camera": {"$ref": "#/$defs/Camera", "description": "Default: preset iso, target plate, zoom 1, perspective. azimuth_deg / elevation_deg override the preset's angles."},
            "size": {"type": "object", "properties": {"width": {"type": "integer", "minimum": 64, "maximum": 2048},
                                                       "height": {"type": "integer", "minimum": 64, "maximum": 2048}},
                     "additionalProperties": false, "description": "Picture size in pixels. Default 1024 x 768."},
            "editor": {"type": "object", "properties": {
                "color_by": {"enum": ["filament", "object", "type"], "default": "filament",
                             "description": "filament: as the editor shows it, with colour painting. object: one colour per object. type: parts grey, modifiers and other bodies in their type colour."},
                "overlay": {"enum": ["none"], "default": "none"},
                "show_modifiers": {"type": "boolean", "default": true, "description": "Draw modifiers, negative parts, support blockers and enforcers (translucent)."},
                "show_bed": {"type": "boolean", "default": true},
                "only": {"type": "array", "items": {"$ref": "#/$defs/Id"}, "description": "Draw only these objects, instances or volumes."},
                "highlight": {"type": "array", "items": {"$ref": "#/$defs/Id"}, "description": "Draw these in orange."},
                "transparent": {"type": "array", "items": {"$ref": "#/$defs/Id"}, "description": "Draw these translucent, to see what is inside or behind."}},
                "additionalProperties": false},
            "format": {"enum": ["png", "jpeg"], "default": "png", "description": "A PNG over 900 kB is sent as JPEG instead."}
        },
        "additionalProperties": false})JSON", "screenshot");

    std::string description =
        "Picture of the scene from any viewpoint, rendered offscreen with a camera of its own: the user's view is not touched and the window "
        "does not have to be visible. view \"editor\" shows the objects of one plate as in the Prepare tab, on a plain bed (10 mm grid, bold "
        "lines every 50 mm, X axis red, Y green, Z blue from the plate origin). Camera: a preset (iso, iso_left, iso_back, iso_back_left, "
        "top, bottom, front, back, left, right) or azimuth_deg / elevation_deg (azimuth 0 looks from the front, 90 from the right; "
        "elevation 90 looks straight down), a target (\"plate\", \"objects\", ids, or a point in plate coordinates), zoom (1 fits the "
        "target) and perspective or orthographic projection. Returns the image and, as data: the camera actually used (pass it to "
        "mesh_query raycast to turn a pixel into a point on the model), for each object its pixel position and pixel box, and the legend "
        "(which colour means what). Pixels are (u right, v down) from the top-left corner.";

    // Views and overlays contributed by other groups.
    for (const ViewPlugin& plugin : RenderPlugins::views()) {
        schema["properties"]["view"]["enum"].push_back(plugin.name);
        schema["properties"][plugin.name] = plugin.options_schema;
        if (plugin.options_schema.is_object() && plugin.options_schema.contains("$defs")) {
            for (auto it = plugin.options_schema["$defs"].begin(); it != plugin.options_schema["$defs"].end(); ++it)
                schema["$defs"][it.key()] = it.value();
            schema["properties"][plugin.name].erase("$defs");
        }
        description += " view \"" + plugin.name + "\": " + plugin.description;
    }
    for (const OverlayPlugin& plugin : RenderPlugins::overlays()) {
        schema["properties"]["editor"]["properties"]["overlay"]["enum"].push_back(plugin.name);
        description += " overlay \"" + plugin.name + "\": " + plugin.description;
    }

    ToolDef tool;
    tool.name = "screenshot";
    tool.title = "Screenshot";
    tool.description = std::move(description);
    tool.input_schema = std::move(schema);
    tool.read_only = true;
    tool.idempotent = true;
    tool.handler = screenshot;
    registry.add(std::move(tool));
}

}}} // namespace Slic3r::GUI::Mcp
