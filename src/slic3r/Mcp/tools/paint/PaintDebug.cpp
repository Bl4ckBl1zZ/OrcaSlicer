// Test-only tools of the paint group (registered only with ORCA_MCP_DEBUG_TOOLS=1):
//   debug_paint_gizmo  open / inspect / close a paint gizmo on an object, and compare what the gizmo
//                      loaded with the model's paint (the "user opens the gizmo afterwards" check);
//   debug_paint_3mf    save the project to a 3MF copy and load it back into a separate Model,
//                      comparing every part's paint and the plates' layer events.

#include "PaintDebug.hpp"

#include <string>
#include <vector>

#include "libslic3r/FacetPaint.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Gizmos/GLGizmoPainterBase.hpp"
#include "slic3r/GUI/Gizmos/GLGizmosManager.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "slic3r/GUI/Selection.hpp"

#include "../../McpIds.hpp"
#include "../../McpRefresh.hpp"
#include "../../McpScene.hpp"
#include "../../McpSchema.hpp"
#include "../../McpTool.hpp"
#include "PaintStats.hpp"

namespace Slic3r { namespace GUI { namespace Mcp { namespace Paint {

namespace {

// Reads the painter gizmo's per-part selectors (a protected member) without touching the gizmo's
// code: a pointer to member taken through a derived class that re-declares it public.
struct PainterPeek : GLGizmoPainterBase
{
    using GLGizmoPainterBase::m_triangle_selectors;
};

GLGizmosManager::EType gizmo_of(FacetPaint::Channel channel)
{
    switch (channel) {
    case FacetPaint::Channel::Support: return GLGizmosManager::FdmSupports;
    case FacetPaint::Channel::Seam: return GLGizmosManager::Seam;
    case FacetPaint::Channel::Color: return GLGizmosManager::MmSegmentation;
    case FacetPaint::Channel::FuzzySkin: return GLGizmosManager::FuzzySkin;
    }
    return GLGizmosManager::FdmSupports;
}

json debug_paint_gizmo(ToolContext& ctx)
{
    const json&         args = ctx.args();
    FacetPaint::Channel channel;
    parse_channel(ctx.arg("channel", "support"), channel);
    const std::string action = args["action"].get<std::string>();
    Plater*           plater = wxGetApp().plater();
    GLCanvas3D*       canvas = plater->get_view3D_canvas3D();
    GLGizmosManager&  gizmos = canvas->get_gizmos_manager();
    json              out = json::object();

    if (action == "close") {
        out["was_open"] = gizmos.get_current_type() != GLGizmosManager::Undefined;
        canvas->exit_gizmo();
        out["open"] = gizmos.get_current_type() != GLGizmosManager::Undefined;
        return out;
    }
    const ObjectRef ref = Ids::object(args["object"].get<std::string>(), "object");
    if (action == "open") {
        canvas->exit_gizmo();
        canvas->get_selection().add_instance(static_cast<unsigned>(ref.obj_idx), 0, true);
        const bool opened = gizmos.open_gizmo(gizmo_of(channel));
        out["opened"] = opened;
        out["current"] = static_cast<int>(gizmos.get_current_type());
        if (!opened || gizmos.get_current_type() != gizmo_of(channel))
            fail("invalid_argument", "The gizmo did not open.");
    }
    // inspect (also after open): compare the gizmo's selectors with the model's paint.
    if (gizmos.get_current_type() != gizmo_of(channel))
        fail("invalid_argument", "The paint gizmo of that channel is not open.");
    auto* painter = dynamic_cast<GLGizmoPainterBase*>(gizmos.get_current());
    if (painter == nullptr)
        fail("invalid_argument", "The open gizmo is not a painter gizmo.");
    const auto& selectors = painter->*(&PainterPeek::m_triangle_selectors);
    json parts = json::array();
    size_t idx = 0;
    bool   all_equal = true;
    const ModelInstance* instance = ref.object->instances.front();
    for (ModelVolume* volume : ref.object->volumes) {
        if (!volume->is_model_part())
            continue;
        json e = json::object();
        e["part"] = Ids::of(*volume);
        if (idx >= selectors.size() || !selectors[idx]) {
            e["missing"] = true;
            all_equal = false;
        } else {
            const TriangleSelector::TriangleSplittingData gizmo_data = selectors[idx]->serialize();
            const bool equal = gizmo_data == FacetPaint::annotation(*volume, channel).get_data();
            e["equal_to_model"] = equal;
            all_equal = all_equal && equal;
            FacetPaint::Transforms trafo;
            trafo.volume = volume->get_matrix();
            trafo.instance = instance->get_matrix();
            const FacetPaint::Stats stats = FacetPaint::statistics(*selectors[idx], volume->mesh(), trafo);
            e["gizmo_states"] = states_json(channel, stats);
        }
        parts.push_back(std::move(e));
        ++idx;
    }
    out["parts"] = std::move(parts);
    out["all_equal"] = all_equal;
    out["open"] = true;
    return out;
}

json debug_paint_3mf(ToolContext& ctx)
{
    const std::string path = ctx.args()["path"].get<std::string>();
    Plater*           plater = wxGetApp().plater();
    if (!plater->export_3mf_copy(boost::filesystem::path(path)))
        fail("invalid_argument", "export_3mf_copy failed for " + path);

    DynamicPrintConfig        config;
    ConfigSubstitutionContext substitutions(ForwardCompatibilitySubstitutionRule::Enable);
    PlateDataPtrs             plate_data;
    std::vector<Preset*>      project_presets;
    Model loaded = Model::read_from_file(path, &config, &substitutions, LoadStrategy::LoadModel | LoadStrategy::LoadConfig, &plate_data, &project_presets);
    for (PlateData* p : plate_data)
        delete p;
    for (Preset* p : project_presets)
        delete p;

    const Model& model = plater->model();
    json         parts = json::array();
    bool         all_equal = loaded.objects.size() == model.objects.size();
    const FacetPaint::Channel channels[] = { FacetPaint::Channel::Support, FacetPaint::Channel::Seam, FacetPaint::Channel::Color,
                                             FacetPaint::Channel::FuzzySkin };
    for (size_t o = 0; o < model.objects.size() && o < loaded.objects.size(); ++o) {
        const ModelObject* a = model.objects[o];
        const ModelObject* b = loaded.objects[o];
        if (a->volumes.size() != b->volumes.size()) {
            all_equal = false;
            continue;
        }
        for (size_t v = 0; v < a->volumes.size(); ++v) {
            const ModelVolume* va = a->volumes[v];
            const ModelVolume* vb = b->volumes[v];
            if (!va->is_model_part())
                continue;
            json e = json::object();
            e["part"] = Ids::of(*va);
            for (FacetPaint::Channel c : channels) {
                const bool painted = !FacetPaint::annotation(*va, c).empty();
                const bool equal = FacetPaint::annotation(*va, c).get_data() == FacetPaint::annotation(*vb, c).get_data();
                if (painted || !equal)
                    e[channel_name(c)] = json::object({ { "painted", painted }, { "equal", equal } });
                all_equal = all_equal && equal;
            }
            parts.push_back(std::move(e));
        }
    }
    json gcodes = json::object();
    for (const auto& [plate, info] : model.plates_custom_gcodes) {
        auto it = loaded.plates_custom_gcodes.find(plate);
        const bool equal = it != loaded.plates_custom_gcodes.end() ? it->second.gcodes == info.gcodes : info.gcodes.empty();
        gcodes[std::to_string(plate + 1)] = json::object({ { "events", info.gcodes.size() }, { "equal", equal },
                                                           { "loaded_events", it != loaded.plates_custom_gcodes.end() ? it->second.gcodes.size() : 0 } });
        all_equal = all_equal && equal;
    }
    json out = json::object();
    out["path"] = path;
    out["objects"] = loaded.objects.size();
    out["parts"] = std::move(parts);
    out["layer_events"] = std::move(gcodes);
    out["all_equal"] = all_equal;
    return out;
}


// Scene set-ups the paint tests need from tools of other groups that are not on this branch.
json debug_paint_fixture(ToolContext& ctx)
{
    const json&       args = ctx.args();
    const std::string op = args["op"].get<std::string>();
    Plater*           plater = wxGetApp().plater();
    json              out = json::object();
    auto vec = [&](const char* key, const Vec3d& fallback) {
        if (!args.contains(key))
            return fallback;
        const json& v = args[key];
        return Vec3d(v[0].get<double>(), v[1].get<double>(), v[2].get<double>());
    };
    if (op == "add_filament") {
        const int count = args.value("count", 1);
        for (int i = 0; i < count; ++i)
            plater->sidebar().add_filament();
        out["filaments"] = Ids::filament_count();
        return out;
    }
    if (op == "filament_color") {
        // As the sidebar's colour picker stores it: the project's filament_colour (test only, no journal).
        const int slot = args.value("count", 1);
        auto* colors = wxGetApp().preset_bundle->project_config.option<ConfigOptionStrings>("filament_colour");
        if (colors == nullptr || slot < 1 || slot > static_cast<int>(colors->values.size()))
            fail("invalid_argument", "no such filament slot");
        colors->values[static_cast<size_t>(slot - 1)] = args.value("color", std::string("#FFFFFF"));
        plater->update_filament_colors_in_full_config();
        plater->get_view3D_canvas3D()->set_as_dirty();
        out["colors"] = colors->values;
        return out;
    }
    const ObjectRef ref = Ids::object(args["object"].get<std::string>(), "object");
    if (op == "transform") {
        ctx.snapshot("test transform " + Ids::of(*ref.object));
        ModelInstance* inst = ref.object->instances.front();
        inst->set_rotation(vec("rotation_deg", Vec3d::Zero()) * (M_PI / 180.));
        inst->set_scaling_factor(vec("scale", Vec3d::Ones()));
        inst->set_mirror(vec("mirror", Vec3d::Ones()));
        ref.object->invalidate_bounding_box();
        Refresh::object(ref.obj_idx);
    } else if (op == "add_instance") {
        ctx.snapshot("test add instance " + Ids::of(*ref.object));
        ModelInstance* inst = ref.object->add_instance(*ref.object->instances.front());
        inst->set_offset(inst->get_offset() + vec("offset", Vec3d(40., 0., 0.)));
        Refresh::structure();
    } else if (op == "add_modifier") {
        ctx.snapshot("test add modifier " + Ids::of(*ref.object));
        const Vec3d size = vec("size", Vec3d(5., 5., 5.));
        ModelVolume* v = ref.object->add_volume(make_cube(size.x(), size.y(), size.z()), ModelVolumeType::PARAMETER_MODIFIER);
        v->name = "test modifier";
        Refresh::full();
    } else if (op == "add_part") {
        ctx.snapshot("test add part " + Ids::of(*ref.object));
        const Vec3d size = vec("size", Vec3d(10., 10., 10.));
        ModelVolume* v = ref.object->add_volume(make_cube(size.x(), size.y(), size.z()), ModelVolumeType::MODEL_PART);
        v->set_offset(vec("offset", Vec3d(0., 0., 0.)));
        v->name = "test part";
        Refresh::full();
    } else
        fail("invalid_argument", "unknown op");
    out["object"] = Scene::object_json(ref.obj_idx, Scene::Detail::Full);
    return out;
}

// Slices a copy of the app's model with the app's configuration, synchronously and outside the
// app's background process, and writes the G-code: an oracle for "the paint takes effect".
json debug_paint_slice(ToolContext& ctx)
{
    const json&    args = ctx.args();
    Plater*        plater = wxGetApp().plater();
    PartPlateList& plates = plater->get_partplate_list();
    const int      plate_idx = Ids::plate(args.contains("plate") ? args["plate"] : json(nullptr));
    PartPlate*     plate = plates.get_plate(plate_idx);
    const std::string path = args["path"].get<std::string>();

    Model model(plater->model());
    // Only the instances on that plate.
    for (int o = static_cast<int>(model.objects.size()) - 1; o >= 0; --o) {
        ModelObject* object = model.objects[static_cast<size_t>(o)];
        for (int i = static_cast<int>(object->instances.size()) - 1; i >= 0; --i)
            if (Ids::plate_of_instance(o, i) != plate_idx)
                object->delete_instance(static_cast<size_t>(i));
        if (object->instances.empty())
            model.delete_object(static_cast<size_t>(o));
    }
    model.curr_plate_index = plate_idx;

    DynamicPrintConfig config = wxGetApp().preset_bundle->full_config();
    config.apply(*plate->config());
    if (args.contains("overrides"))
        for (auto it = args["overrides"].begin(); it != args["overrides"].end(); ++it) {
            ConfigSubstitutionContext sub(ForwardCompatibilitySubstitutionRule::Enable);
            config.set_deserialize(it.key(), it.value().is_string() ? it.value().get<std::string>() : it.value().dump(), sub);
        }
    const int filaments = Ids::filament_count();
    std::vector<int>& maps = config.option<ConfigOptionInts>("filament_map", true)->values;
    maps.assign(static_cast<size_t>(filaments), 1);

    Print print;
    print.set_plate_origin(plate->get_origin());
    json out = json::object();
    try {
        print.apply(model, config);
        std::vector<StringObjectException> warnings;
        const StringObjectException err = print.validate(&warnings, nullptr, nullptr);
        if (!err.string.empty())
            fail("invalid_argument", "validate: " + err.string);
        print.process();
        GCodeProcessorResult result;
        print.export_gcode(path, &result, nullptr);
        out["path"] = path;
        out["layers"] = print.objects().empty() ? 0 : print.objects().front()->layers().size();
        out["support_layers"] = print.objects().empty() ? 0 : print.objects().front()->support_layers().size();
    } catch (const ToolError&) {
        throw;
    } catch (const std::exception& e) {
        fail("invalid_argument", std::string("slicing failed: ") + e.what());
    }
    return out;
}

} // namespace

void register_debug_tools(ToolRegistry& registry)
{
    {
        ToolDef tool;
        tool.name = "debug_paint_gizmo";
        tool.title = "Paint gizmo probe (test)";
        tool.description = "Test only. open: select the object and open the paint gizmo of the channel; inspect: compare the open gizmo's data with the "
                           "model's paint; close: close any gizmo.";
        tool.input_schema = Schema::parse(R"JSON({
            "type": "object",
            "properties": {
                "action": {"enum": ["open", "inspect", "close"]},
                "object": {"$ref": "#/$defs/Id"},
                "channel": {"enum": ["support", "seam", "color", "fuzzy_skin"], "default": "support"}
            },
            "required": ["action"],
            "additionalProperties": false})JSON", "debug_paint_gizmo");
        tool.debug_only = true;
        tool.requires_idle_ui = false; // must not close the gizmo it inspects
        tool.accepts_if_rev = false;
        tool.handler = debug_paint_gizmo;
        registry.add(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "debug_paint_3mf";
        tool.title = "3MF round trip of paint (test)";
        tool.description = "Test only. Saves a 3MF copy of the project to `path` and loads it into a separate Model; compares every part's paint "
                           "and the layer events.";
        tool.input_schema = Schema::parse(R"JSON({
            "type": "object",
            "properties": {"path": {"type": "string", "minLength": 1}},
            "required": ["path"],
            "additionalProperties": false})JSON", "debug_paint_3mf");
        tool.debug_only = true;
        tool.read_only = true;
        tool.handler = debug_paint_3mf;
        registry.add(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "debug_paint_fixture";
        tool.title = "Paint test fixtures (test)";
        tool.description = "Test only. op add_filament (count), filament_color (count = slot, color), transform (object, rotation_deg, scale, mirror [+-1,..] of instance 0), "
                           "add_instance (object, offset), add_modifier (object, size), add_part (object, size, offset).";
        tool.input_schema = Schema::parse(R"JSON({
            "type": "object",
            "properties": {
                "op": {"enum": ["add_filament", "filament_color", "transform", "add_instance", "add_modifier", "add_part"]},
                "color": {"$ref": "#/$defs/Color"},
                "object": {"$ref": "#/$defs/Id"},
                "count": {"type": "integer", "minimum": 1, "maximum": 8},
                "rotation_deg": {"$ref": "#/$defs/Vec3"}, "scale": {"$ref": "#/$defs/Vec3"}, "mirror": {"$ref": "#/$defs/Vec3"},
                "offset": {"$ref": "#/$defs/Vec3"}, "size": {"$ref": "#/$defs/Vec3"}
            },
            "required": ["op"],
            "additionalProperties": false})JSON", "debug_paint_fixture");
        tool.debug_only = true;
        tool.accepts_if_rev = false;
        tool.handler = debug_paint_fixture;
        registry.add(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "debug_paint_slice";
        tool.title = "Headless slice for paint effect tests (test)";
        tool.description = "Test only. Slices a copy of the plate's objects with the app's configuration (plus `overrides`) synchronously and writes "
                           "the G-code to `path`.";
        tool.input_schema = Schema::parse(R"JSON({
            "type": "object",
            "properties": {"path": {"type": "string", "minLength": 1}, "plate": {"$ref": "#/$defs/PlateRef"},
                           "overrides": {"type": "object", "additionalProperties": true}},
            "required": ["path"],
            "additionalProperties": false})JSON", "debug_paint_slice");
        tool.debug_only = true;
        tool.read_only = true;
        tool.run_timeout_ms = 280000;
        tool.handler = debug_paint_slice;
        registry.add(std::move(tool));
    }
}

}}}} // namespace Slic3r::GUI::Mcp::Paint
