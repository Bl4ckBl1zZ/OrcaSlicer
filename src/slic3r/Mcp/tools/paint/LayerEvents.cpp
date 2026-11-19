// plate_layer_events: the layer slider's per-layer events, written into
// Model::plates_custom_gcodes[plate] the way the slider's EVT_CUSTOMEVT_TICKSCHANGED handler does
// (Plater.cpp), then the slider is re-synchronised so its own copy cannot overwrite them.
//
// These events are not on the undo stack (the GUI does not undo slider ticks either); each change
// is recorded in the MCP journal and `history` can revert it.

#include "LayerEvents.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <vector>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/CustomGCode.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GCodeViewer.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/IMSlider.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"

#include "../../McpIds.hpp"
#include "../../McpSchema.hpp"
#include "../../McpTool.hpp"
#include "../../McpUtil.hpp"

namespace Slic3r { namespace GUI { namespace Mcp { namespace Paint {

namespace {

const double Z_EPS = 1e-4;

double r4(double v) { return std::round(v * 1e4) / 1e4; }

const char* type_name(CustomGCode::Type type)
{
    switch (type) {
    case CustomGCode::ColorChange: return "color_change";
    case CustomGCode::PausePrint: return "pause";
    case CustomGCode::ToolChange: return "filament_change";
    case CustomGCode::Template: return "template";
    case CustomGCode::Custom: return "custom";
    default: return "unknown";
    }
}

bool parse_type(const std::string& name, CustomGCode::Type& out)
{
    if (name == "color_change") out = CustomGCode::ColorChange;
    else if (name == "filament_change") out = CustomGCode::ToolChange;
    else if (name == "pause") out = CustomGCode::PausePrint;
    else if (name == "custom") out = CustomGCode::Custom;
    else if (name == "template") out = CustomGCode::Template;
    else return false;
    return true;
}

// Layer tops of a plate: the sliced layers when the plate has a valid slice, else nominal ones
// from the first layer height and the layer height of the process.
struct Layers
{
    std::vector<double> z;
    bool                sliced { false };
    double              top { 0. }; // highest object top on the plate
};

Layers plate_layers(int plate_idx)
{
    Plater*        plater = wxGetApp().plater();
    PartPlateList& plates = plater->get_partplate_list();
    PartPlate*     plate = plates.get_plate(plate_idx);
    const Model&   model = plater->model();
    Layers         out;
    for (int o = 0; o < static_cast<int>(model.objects.size()); ++o) {
        const ModelObject* object = model.objects[static_cast<size_t>(o)];
        for (int i = 0; i < static_cast<int>(object->instances.size()); ++i)
            if (Ids::plate_of_instance(o, i) == plate_idx)
                out.top = std::max(out.top, object->instance_bounding_box(static_cast<size_t>(i)).max.z());
    }
    if (plate != nullptr && plate->is_slice_result_valid()) {
        PrintBase*  base = nullptr;
        GCodeResult* result = nullptr;
        int          index = -1;
        plate->get_print(&base, &result, &index);
        if (const Print* print = dynamic_cast<const Print*>(base); print != nullptr) {
            std::set<double> zs;
            for (const PrintObject* object : print->objects()) {
                for (const Layer* layer : object->layers())
                    zs.insert(r4(layer->print_z));
                for (const SupportLayer* layer : object->support_layers())
                    zs.insert(r4(layer->print_z));
            }
            if (!zs.empty()) {
                out.z.assign(zs.begin(), zs.end());
                out.sliced = true;
                return out;
            }
        }
    }
    const DynamicPrintConfig& config = wxGetApp().preset_bundle->prints.get_edited_preset().config;
    const double first = config.has("initial_layer_print_height") ? config.opt_float("initial_layer_print_height") : 0.2;
    const double height = config.has("layer_height") ? config.opt_float("layer_height") : 0.2;
    if (height > 0. && first > 0.)
        for (double z = first; z <= out.top + Z_EPS && out.z.size() < 100000; z = first + height * double(out.z.size()))
            out.z.push_back(r4(z));
    return out;
}

// 1-based layer number of the layer whose top is z (or the first above it).
int layer_of(const Layers& layers, double z)
{
    auto it = std::lower_bound(layers.z.begin(), layers.z.end(), z - Z_EPS);
    return it == layers.z.end() ? -1 : static_cast<int>(it - layers.z.begin()) + 1;
}

json event_json(const CustomGCode::Item& item, const Layers& layers, size_t index)
{
    json e = json::object();
    e["index"] = index;
    e["z"] = r4(item.print_z);
    const int layer = layer_of(layers, item.print_z);
    e["layer"] = layer > 0 ? json(layer) : json(nullptr);
    e["type"] = type_name(item.type);
    if (item.type == CustomGCode::ColorChange || item.type == CustomGCode::ToolChange)
        e["filament"] = item.extruder;
    if (!item.color.empty())
        e["color"] = item.color;
    if (item.type == CustomGCode::PausePrint && !item.extra.empty())
        e["message"] = item.extra;
    if (item.type == CustomGCode::Custom)
        e["gcode"] = item.extra;
    return e;
}

json events_json(const CustomGCode::Info& info, const Layers& layers)
{
    json list = json::array();
    for (size_t i = 0; i < info.gcodes.size(); ++i)
        list.push_back(event_json(info.gcodes[i], layers, i));
    return list;
}

const char* mode_name(CustomGCode::Mode mode)
{
    switch (mode) {
    case CustomGCode::SingleExtruder: return "single_extruder";
    case CustomGCode::MultiAsSingle: return "multi_as_single";
    case CustomGCode::MultiExtruder: return "multi_extruder";
    default: return "undefined";
    }
}

// The mode the layer slider would give the plate (Preview::update_layers_slider_mode +
// IMSlider::SetModeAndOnlyExtruder): one filament -> single extruder, several -> "multi as single".
CustomGCode::Mode slider_mode(int plate_idx)
{
    PartPlate* plate = wxGetApp().plater()->get_partplate_list().get_plate(plate_idx);
    if (wxGetApp().filaments_cnt() > 1 && plate != nullptr && !plate->get_extruders_without_support().empty())
        return CustomGCode::MultiAsSingle;
    return CustomGCode::SingleExtruder;
}

// After the model's events of a plate changed: what the slider's tick-change handler does.
void refresh_plate(int plate_idx)
{
    Plater*        plater = wxGetApp().plater();
    PartPlateList& plates = plater->get_partplate_list();
    if (PartPlate* plate = plates.get_plate(plate_idx); plate != nullptr)
        plate->update_slice_result_valid_state(false);
    // The slider keeps its own copy and writes it back on its next edit: give it ours.
    if (plate_idx == plates.get_curr_plate_index()) {
        if (GLCanvas3D* preview = plater->get_preview_canvas3D(); preview != nullptr) {
            if (IMSlider* slider = preview->get_gcode_viewer().get_layers_slider(); slider != nullptr) {
                // First the slider's mode for the plate as it is now (Preview::update_layers_slider_mode,
                // private): with a mode left from an earlier slice that used several filaments, the
                // slider takes filament changes as not allowed, erases them and writes that back.
                bool can_change_color = true;
                int  only_extruder    = -1;
                if (wxGetApp().filaments_cnt() > 1) {
                    if (PartPlate* plate = plates.get_plate(plate_idx); plate != nullptr) {
                        const std::vector<int> used = plate->get_extruders_without_support();
                        for (int extruder : used)
                            if (extruder != used.front())
                                can_change_color = false;
                        if (!used.empty())
                            only_extruder = used.front();
                    }
                }
                slider->SetModeAndOnlyExtruder(true, only_extruder, can_change_color);
                slider->SetTicksValues(plater->model().get_curr_plate_custom_gcodes());
            }
            preview->set_as_dirty();
        }
    }
    plater->set_plater_dirty(true);
    plater->schedule_background_process();
}

json plate_layer_events(ToolContext& ctx)
{
    // The pause message is saved in the project and may be written into G-code (M117 in some firmware paths): one line only.
    if (ctx.has("message"))
        check_single_line(ctx.arg("message", ""), "message");
    const json&    args = ctx.args();
    Plater*        plater = wxGetApp().plater();
    Model&         model = plater->model();
    const int      plate_idx = Ids::plate(ctx.has("plate") ? args["plate"] : json(nullptr));
    const std::string action = ctx.arg("action", "list");
    const Layers   layers = plate_layers(plate_idx);
    // Do not create an entry for the plate just to list it.
    CustomGCode::Info empty;
    auto              found = model.plates_custom_gcodes.find(plate_idx);
    CustomGCode::Info* infop = found != model.plates_custom_gcodes.end() ? &found->second : &empty;

    auto result = [&](bool changed) {
        json out = json::object();
        if (changed)
            out["ok"] = true;
        out["plate"] = plate_idx + 1;
        out["mode"] = mode_name(infop->mode);
        out["layers"] = json::object({ { "source", layers.sliced ? "sliced" : "nominal (layer heights of the process; slice for exact layers)" },
                                       { "count", layers.z.size() },
                                       { "first_z", layers.z.empty() ? json(nullptr) : json(layers.z.front()) },
                                       { "top_z", layers.z.empty() ? json(nullptr) : json(layers.z.back()) } });
        out["events"] = events_json(*infop, layers);
        return out;
    };

    // Resolves `at` to a print_z.
    auto resolve_at = [&]() -> double {
        if (!ctx.has("at"))
            fail("invalid_argument", "`at` is required for action \"" + action + "\": {\"z\": mm} or {\"layer\": n} (1-based).");
        const json& at = args["at"];
        if (at.contains("layer")) {
            const int n = at["layer"].get<int>();
            if (layers.z.empty())
                fail("invalid_argument", "The plate has no layers (no objects on it).");
            if (n < 1 || n > static_cast<int>(layers.z.size()))
                fail("invalid_argument", "`at.layer` = " + std::to_string(n) + " is outside 1.." + std::to_string(layers.z.size()) + ".",
                     layers.sliced ? "" : "Layer numbers are nominal until the plate is sliced.");
            return layers.z[static_cast<size_t>(n - 1)];
        }
        const double z = at["z"].get<double>();
        if (!(z > 0.))
            fail("invalid_argument", "`at.z` must be > 0.");
        if (layers.z.empty() || z > layers.z.back() + Z_EPS)
            fail("invalid_argument",
                 "`at.z` = " + std::to_string(z) + " is above the top of the objects on plate " + std::to_string(plate_idx + 1) + " (" +
                     std::to_string(layers.top) + " mm).",
                 "Events above the last layer are dropped by the app.");
        // Snap to the layer that z falls in (the event runs before that layer).
        const int layer = layer_of(layers, z);
        return layers.z[static_cast<size_t>(layer - 1)];
    };

    if (action == "list")
        return result(false);

    infop = &model.plates_custom_gcodes[plate_idx];
    CustomGCode::Info&      info = *infop;
    const CustomGCode::Info before = info;
    const json             before_json = events_json(info, layers);

    if (action == "add") {
        if (!ctx.has("type"))
            fail("invalid_argument", "`type` is required for action \"add\": filament_change, color_change, pause, custom or template.");
        CustomGCode::Type type;
        if (!parse_type(args["type"].get<std::string>(), type))
            fail("invalid_argument", "`type` must be filament_change, color_change, pause, custom or template.");
        const DynamicPrintConfig& printer = wxGetApp().preset_bundle->printers.get_edited_preset().config;
        const DynamicPrintConfig  full = wxGetApp().preset_bundle->full_config();
        PartPlate*                plate = plater->get_partplate_list().get_plate(plate_idx);

        // The slider's own restrictions (IMSlider::render_add_menu, SetTicksValues).
        if (plate != nullptr && plate->get_real_print_seq() == PrintSequence::ByObject)
            fail("invalid_argument", "Plate " + std::to_string(plate_idx + 1) + " prints object by object; the app allows no layer events then.",
                 "Set print_sequence to by layer for this plate, or use paint_apply / a modifier for colour.");
        if (full.has("spiral_mode") && full.opt_bool("spiral_mode"))
            fail("invalid_argument", "Spiral vase mode is on; the app removes layer events in that mode.");

        const int filaments = wxGetApp().filaments_cnt();
        int       filament = ctx.arg<int>("filament", 0);
        std::vector<std::string> colors = plater->get_extruder_colors_from_plater_config();
        CustomGCode::Item item;
        item.type = type;
        item.extruder = 1;
        if (type == CustomGCode::ToolChange) {
            if (filaments < 2)
                fail("invalid_argument", "A filament change needs at least two filament slots; the project has " + std::to_string(filaments) + ".",
                     "Add a filament slot (filament_edit action \"add\"), or use type \"color_change\" (M600) on a single-filament printer.",
                     json::object({ { "filament_count", filaments } }));
            if (filament < 1)
                fail("invalid_argument", "`filament` (1-based slot) is required for a filament change.");
            if (filament > filaments)
                fail("invalid_argument", "There is no filament slot " + std::to_string(filament) + "; the project has " + std::to_string(filaments) + ".",
                     "Add slots with filament_edit (action \"add\"); filament_list shows them.", json::object({ { "filament_count", filaments } }));
            if (plate != nullptr) {
                std::vector<int> used = plate->get_extruders_without_support();
                std::sort(used.begin(), used.end());
                used.erase(std::unique(used.begin(), used.end()), used.end());
                if (used.size() > 1) {
                    std::string list;
                    for (int e : used)
                        list += (list.empty() ? "" : ", ") + std::to_string(e);
                    fail("invalid_argument",
                         "Plate " + std::to_string(plate_idx + 1) + " already prints with several filaments (" + list +
                             "); the app allows filament changes by layer only when the plate prints with one filament.",
                         "Assign one filament to every object on the plate (object_update), or colour by paint (paint_apply channel color) or by "
                         "modifier instead.");
                }
            }
            item.extruder = filament;
            item.color = filament <= static_cast<int>(colors.size()) ? colors[static_cast<size_t>(filament - 1)] : "";
        } else if (type == CustomGCode::ColorChange) {
            if (filament > filaments)
                fail("invalid_argument", "There is no filament slot " + std::to_string(filament) + "; the project has " + std::to_string(filaments) + ".");
            item.extruder = filament > 0 ? filament : 1;
            item.color = ctx.has("color") ? args["color"].get<std::string>() :
                         (item.extruder <= static_cast<int>(colors.size()) ? colors[static_cast<size_t>(item.extruder - 1)] : "#FFFFFF");
            if (!printer.has("color_change_gcode") || printer.opt_string("color_change_gcode").empty())
                ctx.note("The printer's color_change_gcode is empty: the colour change will be marked in the G-code but no M600 is sent.");
            if (filaments > 1)
                ctx.note("This project has several filament slots; the app's layer slider offers filament changes (type filament_change) then. A colour "
                         "change (M600) only applies where filament " + std::to_string(item.extruder) + " prints above it.");
        } else if (type == CustomGCode::PausePrint) {
            item.extra = ctx.arg("message", "");
            if (!printer.has("machine_pause_gcode") || printer.opt_string("machine_pause_gcode").empty())
                ctx.note("The printer's machine_pause_gcode is empty: the pause will be marked in the G-code but the printer is not told to pause.");
        } else if (type == CustomGCode::Template) {
            if (!printer.has("template_custom_gcode") || printer.opt_string("template_custom_gcode").empty())
                fail("invalid_argument", "The printer has no template_custom_gcode; the app offers \"Add Custom Template\" only when it is set.");
        } else if (type == CustomGCode::Custom) {
            if (!wxGetApp().app_config->get_bool("mcp_allow_gcode_edit"))
                fail("forbidden_key", "Custom G-code is writable only when the user allows the agent to edit G-code.",
                     "Ask the user to enable \"Allow the agent to edit G-code settings\" (mcp_allow_gcode_edit) in Preferences, MCP server section; or use type "
                     "pause / filament_change / color_change.");
            item.extra = ctx.arg("gcode", "");
            if (item.extra.empty())
                fail("invalid_argument", "`gcode` (the G-code text) is required for type \"custom\".");
        }
        if (ctx.has("message") && type != CustomGCode::PausePrint)
            fail("invalid_argument", "`message` is only used by type \"pause\".");
        if (ctx.has("gcode") && type != CustomGCode::Custom)
            fail("invalid_argument", "`gcode` is only used by type \"custom\".");
        if (ctx.has("color") && type != CustomGCode::ColorChange)
            fail("invalid_argument", "`color` is only used by type \"color_change\" (a filament change takes the slot's colour).");

        item.print_z = resolve_at();
        for (const CustomGCode::Item& existing : info.gcodes)
            if (std::abs(existing.print_z - item.print_z) < Z_EPS)
                fail("invalid_argument",
                     "There is already a " + std::string(type_name(existing.type)) + " event at z " + std::to_string(r4(item.print_z)) +
                         " (layer " + std::to_string(layer_of(layers, item.print_z)) + "); the app keeps one event per layer.",
                     "Remove it first (action \"remove\" with the same `at`), or pick another layer.");
        info.gcodes.push_back(item);
        std::sort(info.gcodes.begin(), info.gcodes.end());
        if (info.mode == CustomGCode::Undef || (before.gcodes.empty() && info.mode != slider_mode(plate_idx)))
            info.mode = slider_mode(plate_idx);
    } else if (action == "remove") {
        // An event may sit above the current top (objects moved or scaled since): match the raw z too.
        if (!ctx.has("at"))
            fail("invalid_argument", "`at` is required for action \"remove\": {\"z\": mm} or {\"layer\": n}.");
        double z = 0.;
        if (args["at"].contains("layer"))
            z = resolve_at();
        else
            z = args["at"]["z"].get<double>();
        const int    z_layer = layer_of(layers, z);
        std::string  only_type;
        if (ctx.has("type"))
            only_type = args["type"].get<std::string>();
        const size_t n = info.gcodes.size();
        info.gcodes.erase(std::remove_if(info.gcodes.begin(), info.gcodes.end(),
                                         [&](const CustomGCode::Item& item) {
                                             const bool here = std::abs(item.print_z - z) < Z_EPS ||
                                                               (z_layer > 0 && layer_of(layers, item.print_z) == z_layer);
                                             return here && (only_type.empty() || only_type == type_name(item.type));
                                         }),
                          info.gcodes.end());
        if (info.gcodes.size() == n)
            fail("invalid_argument", "There is no " + (only_type.empty() ? std::string("event") : only_type + " event") + " at z " + std::to_string(r4(z)) + ".",
                 "action \"list\" shows the events with their z and layer.");
    } else if (action == "clear") {
        info.gcodes.clear();
    } else
        fail("invalid_argument", "`action` must be list, add, remove or clear.");

    const bool changed = info != before;
    if (changed) {
        const json after_json = events_json(info, layers);
        refresh_plate(plate_idx);
        ctx.set_slice_invalidated();
        ctx.journal(json::object({ { "plate", plate_idx + 1 } }), "layer_events", before_json, after_json, [plate_idx, before]() {
            wxGetApp().plater()->model().plates_custom_gcodes[plate_idx] = before;
            refresh_plate(plate_idx);
        });
    }
    return result(changed);
}

} // namespace

void register_layer_events(ToolRegistry& registry)
{
    ToolDef tool;
    tool.name = "plate_layer_events";
    tool.title = "Colour change, pause, custom G-code at a layer";
    tool.description =
        "Events at a layer of one plate, as the layer slider's menu adds them: filament_change (switch to filament slot n at that layer - the "
        "cheapest way to print a top in another colour; needs >= 2 slots and a plate printed with one filament), color_change (M600 on a "
        "single-extruder printer, optional color #RRGGBB for the preview), pause (machine_pause_gcode, optional message), template (the "
        "printer's template_custom_gcode) and custom (your G-code text; only when the user allowed G-code edits in Preferences, else "
        "forbidden_key). action list | add | remove | clear; `at` is {\"z\": mm} (snapped to the layer that height falls in) or {\"layer\": n} "
        "(1-based; exact after slicing, nominal before). The event runs before that layer prints. Returns the plate's events with z, layer "
        "and type. Not on the undo stack (like the slider): changes are journaled and `history` can revert them. Re-slice afterwards.";
    tool.input_schema = Schema::parse(R"JSON({
        "type": "object",
        "properties": {
            "action": {"enum": ["list", "add", "remove", "clear"], "default": "list"},
            "plate": {"$ref": "#/$defs/PlateRef"},
            "at": {"oneOf": [{"type": "object", "properties": {"z": {"type": "number", "exclusiveMinimum": 0}}, "required": ["z"], "additionalProperties": false},
                             {"type": "object", "properties": {"layer": {"type": "integer", "minimum": 1}}, "required": ["layer"], "additionalProperties": false}],
                   "description": "{\"z\": mm} or {\"layer\": n} (1-based)."},
            "type": {"enum": ["filament_change", "color_change", "pause", "custom", "template"]},
            "filament": {"type": "integer", "minimum": 1, "description": "filament_change: the slot to switch to. color_change: the extruder (default 1)."},
            "color": {"$ref": "#/$defs/Color", "description": "color_change only: the new colour shown in the preview."},
            "message": {"type": "string", "maxLength": 200, "description": "pause only."},
            "gcode": {"type": "string", "maxLength": 20000, "description": "custom only: the G-code text."}
        },
        "additionalProperties": false})JSON", "plate_layer_events");
    tool.handler = plate_layer_events;
    registry.add(std::move(tool));
}

}}}} // namespace Slic3r::GUI::Mcp::Paint
