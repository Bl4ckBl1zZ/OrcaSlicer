#include "SliceData.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/BackgroundSlicingProcess.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "libslic3r/PresetBundle.hpp"

#include "../../McpIds.hpp"
#include "SliceTracker.hpp"

namespace Slic3r { namespace GUI { namespace Mcp { namespace Slice {

namespace {

Plater& plater() { return *wxGetApp().plater(); }

using Stats = PrintEstimatedStatistics;

std::string role_label(ExtrusionRole role) { return ExtrusionEntity::role_to_string(role); }

// "Sparse infill" -> "sparse_infill"
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

const ModelObject* model_object_by_id(size_t id)
{
    for (const ModelObject* mo : plater().model().objects)
        if (mo->id().id == id)
            return mo;
    return nullptr;
}

std::string filament_mm3_to_g_line(double mm3, double diameter, double density, double& length_mm, double& weight_g)
{
    const double area = M_PI * diameter * diameter / 4.;
    length_mm = area > 0. ? mm3 / area : 0.;
    weight_g = mm3 * density / 1000.;
    return {};
}

double vec_at(const std::vector<float>& v, size_t i, double fallback)
{
    return i < v.size() ? static_cast<double>(v[i]) : (v.empty() ? fallback : static_cast<double>(v.front()));
}

json box_json(const BoundingBoxf3& box)
{
    if (!box.defined)
        return nullptr;
    return json::object({{"min", {round_to(box.min.x(), 2), round_to(box.min.y(), 2), round_to(box.min.z(), 2)}},
                         {"max", {round_to(box.max.x(), 2), round_to(box.max.y(), 2), round_to(box.max.z(), 2)}}});
}

// Moves of the result are in the frame of the bed plus the plate's offset; tools report plate frame.
Vec3d plate_origin(int plate_idx)
{
    PartPlate* plate = plater().get_partplate_list().get_plate(plate_idx);
    return plate != nullptr ? plate->get_origin() : Vec3d::Zero();
}

std::string kind_of_exception(const StringObjectException& e)
{
    switch (e.type) {
    case STRING_EXCEPT_FILAMENT_NOT_MATCH_BED_TYPE: return "filament_bed_type";
    case STRING_EXCEPT_FILAMENTS_DIFFERENT_TEMP: return "filament_temperatures";
    case STRING_EXCEPT_OBJECT_COLLISION_IN_SEQ_PRINT: return "object_collision_sequential";
    case STRING_EXCEPT_OBJECT_COLLISION_IN_LAYER_PRINT: return "object_collision";
    case STRING_EXCEPT_LAYER_HEIGHT_EXCEEDS_LIMIT: return "layer_height_limit";
    default: break;
    }
    if (!e.opt_key.empty())
        return "setting";
    if (e.object != nullptr)
        return "object";
    return "print";
}

json exception_json(const StringObjectException& e)
{
    json out = json::object();
    out["message"] = e.string;
    out["kind"] = kind_of_exception(e);
    if (e.object != nullptr) {
        json ref = object_ref(e.object);
        if (!ref.is_null())
            out["object"] = ref;
    }
    if (!e.opt_key.empty())
        out["key"] = e.opt_key;
    if (!e.params.empty())
        out["params"] = e.params;
    // What to do about it.
    std::string fix;
    const std::string kind = out["kind"];
    if (kind == "setting")
        fix = "Change `" + e.opt_key + "` (settings_get shows where it is set, settings_set changes it).";
    else if (kind == "object_collision" || kind == "object_collision_sequential")
        fix = "Move the objects apart (object_transform / object_arrange) or print them by layer.";
    else if (kind == "filament_bed_type")
        fix = "Pick a plate type the filament supports, or another filament.";
    else if (kind == "filament_temperatures")
        fix = "Use filaments with compatible temperatures on one plate, or allow mixed temperatures in the preferences.";
    else if (kind == "layer_height_limit")
        fix = "Lower `layer_height` (or the per-object layer height) below the nozzle's maximum.";
    else if (kind == "object")
        fix = "Fix the named object (scale, move, or change its per-object settings).";
    if (!fix.empty())
        out["fix"] = fix;
    return out;
}

} // namespace

double round_to(double v, int digits)
{
    const double f = std::pow(10., digits);
    return std::round(v * f) / f;
}

std::string format_duration(double seconds)
{
    long s = static_cast<long>(std::lround(seconds));
    const long d = s / 86400; s %= 86400;
    const long h = s / 3600; s %= 3600;
    const long m = s / 60; s %= 60;
    char buf[64];
    if (d > 0)
        std::snprintf(buf, sizeof buf, "%ldd %ldh %ldm %lds", d, h, m, s);
    else if (h > 0)
        std::snprintf(buf, sizeof buf, "%ldh %ldm %lds", h, m, s);
    else if (m > 0)
        std::snprintf(buf, sizeof buf, "%ldm %lds", m, s);
    else
        std::snprintf(buf, sizeof buf, "%lds", s);
    return buf;
}

int plate_count() { return plater().get_partplate_list().get_plate_count(); }
int current_plate() { return plater().get_partplate_list().get_curr_plate_index(); }

PlateHandles handles(int plate_idx)
{
    PlateHandles h;
    h.index = plate_idx;
    h.plate = plater().get_partplate_list().get_plate(plate_idx);
    if (h.plate == nullptr)
        return h;
    PrintBase*   print = nullptr;
    GCodeResult* result = nullptr;
    h.plate->get_print(&print, &result, nullptr);
    h.print = dynamic_cast<Print*>(print);
    h.result = result;
    return h;
}

bool slicing_now() { return plater().is_background_process_slicing() || plater().background_process().running(); }

bool plate_busy(int plate_idx) { return slicing_now() && bsp_plate_index() == plate_idx; }

json object_ref(const ObjectBase* object)
{
    if (object == nullptr)
        return nullptr;
    const ModelObject* mo = dynamic_cast<const ModelObject*>(object);
    if (mo == nullptr) {
        if (const PrintObject* po = dynamic_cast<const PrintObject*>(object))
            mo = po->model_object();
    }
    if (mo == nullptr)
        return nullptr;
    // The Print holds copies of the model objects with the same ids.
    const ModelObject* live = model_object_by_id(mo->id().id);
    json out = json::object();
    out["id"] = Ids::of(live != nullptr ? *live : *mo);
    out["name"] = (live != nullptr ? live : mo)->name;
    return out;
}

json object_ref_by_id(size_t object_id, const Print* print)
{
    if (print != nullptr)
        if (const PrintObject* po = print->get_object(ObjectID(object_id)))
            return object_ref(po);
    if (const ModelObject* mo = model_object_by_id(object_id))
        return object_ref(mo);
    return nullptr;
}

bool plate_has_result(int plate_idx)
{
    PlateHandles h = handles(plate_idx);
    return h.plate != nullptr && h.plate->is_slice_result_valid() && h.result != nullptr && !h.result->moves.empty();
}

json plate_state(int plate_idx)
{
    PlateHandles h = handles(plate_idx);
    json         out = json::object();
    out["plate"] = plate_idx + 1;
    out["id"] = Ids::of(*h.plate);
    out["current"] = plate_idx == current_plate();
    const bool valid = h.plate->is_slice_result_valid();
    Tracker::instance().observe_plate(plate_idx, valid);
    const PlateRecord* rec = Tracker::instance().find_record(plate_idx);
    std::string state, reason;
    if (plate_busy(plate_idx)) {
        state = "slicing";
        out["percent"] = round_to(h.plate->get_slicing_percent(), 1);
    } else if (!h.plate->has_printable_instances()) {
        state = "empty";
        reason = "No printable object on this plate.";
    } else if (valid) {
        state = "sliced";
    } else if (rec != nullptr && rec->last_outcome == "error") {
        state = "failed";
        reason = rec->error_message;
    } else if (h.plate->is_apply_result_invalid()) {
        state = "invalid";
        reason = "The plate does not pass validation; plate_validate lists the errors.";
    } else if (rec != nullptr && rec->had_valid_result) {
        state = "invalidated";
        reason = "The scene or settings changed after the last slice; slice again.";
    } else if (rec != nullptr && rec->last_outcome == "cancelled") {
        state = "cancelled";
        reason = "The last slice was stopped (" + rec->cancel_reason + ").";
    } else
        state = "not_sliced";
    out["state"] = state;
    if (!reason.empty())
        out["reason"] = reason;
    if (rec != nullptr && rec->slices > 0)
        out["last_slice_s"] = round_to(rec->duration_s, 1);
    return out;
}

json validation(int plate_idx, bool refresh, bool& has_errors)
{
    PlateHandles h = handles(plate_idx);
    Model&       model = plater().model();
    json         errors = json::array();
    json         warnings = json::array();
    const bool   is_current = plate_idx == current_plate();

    if (is_current && refresh && plater().is_background_process_update_scheduled() && !slicing_now())
        // A change is waiting for the 0.5 s background-process timer: apply it now, as the timer would.
        plater().update(false, true);

    // Objects on this plate and whether they stick out.
    int printable = 0;
    for (int o = 0; o < static_cast<int>(model.objects.size()); ++o) {
        ModelObject* object = model.objects[static_cast<size_t>(o)];
        for (int i = 0; i < static_cast<int>(object->instances.size()); ++i) {
            if (!h.plate->contain_instance(o, i))
                continue;
            if (object->instances[static_cast<size_t>(i)]->printable)
                ++printable;
            if (!h.plate->contain_instance_totally(o, i)) {
                json e = json::object();
                e["message"] = "Object \"" + object->name + "\" is partly outside the plate.";
                e["kind"] = "outside_plate";
                e["object"] = json::object({{"id", Ids::of(*object)}, {"name", object->name}});
                e["instance"] = Ids::of(*object->instances[static_cast<size_t>(i)]);
                e["box"] = box_json(object->instance_bounding_box(static_cast<size_t>(i)));
                e["fix"] = "Move it fully onto the plate or fully off it (object_transform), or scale it down.";
                errors.push_back(std::move(e));
            }
        }
    }
    if (printable == 0) {
        json e = json::object();
        e["message"] = "Nothing to slice: no printable object on plate " + std::to_string(plate_idx + 1) + ".";
        e["kind"] = "empty_plate";
        e["fix"] = "Add or move an object onto this plate, or mark its objects printable (object_update).";
        errors.push_back(std::move(e));
    }

    // Filament checks the Slice button runs (validate_current_plate).
    {
        const DynamicPrintConfig config = wxGetApp().preset_bundle->full_config();
        wxString msg;
        if (!h.plate->check_filament_printable(config, msg)) {
            json e = json::object();
            e["message"] = into_u8(msg);
            e["kind"] = "filament_not_printable";
            e["fix"] = "Assign a filament this printer / nozzle can print (object_update filament, filament settings).";
            errors.push_back(std::move(e));
        }
    }

    // The slicer's own validation (Print::validate) on the plate's print.
    if (h.print != nullptr && !slicing_now()) {
        std::vector<StringObjectException> ws;
        Polygons                           polys;
        std::vector<std::pair<Polygon, float>> heights;
        StringObjectException err;
        if (is_current) {
            plater().background_process().fff_print()->set_check_multi_filaments_compatibility(
                wxGetApp().app_config->get("enable_high_low_temp_mixed_printing") == "false");
            err = plater().background_process().validate(&ws, &polys, &heights);
        } else if (!h.print->empty())
            err = h.print->validate(&ws, &polys, &heights);
        plater().post_process_string_object_exception(err);
        if (!err.string.empty())
            errors.push_back(exception_json(err));
        for (StringObjectException& w : ws) {
            plater().post_process_string_object_exception(w);
            if (!w.string.empty())
                warnings.push_back(exception_json(w));
        }
    }

    has_errors = !errors.empty();
    json out = json::object();
    out["plate"] = plate_idx + 1;
    out["ok"] = !has_errors;
    out["errors"] = std::move(errors);
    out["warnings"] = std::move(warnings);
    out["printable_instances"] = printable;
    if (!is_current)
        out["checked"] = "geometry and the plate's last applied print (the plate is not the current one; slice selects it and re-validates)";
    return out;
}

json slice_warnings(int plate_idx)
{
    PlateHandles h = handles(plate_idx);
    json         out = json::array();
    if (h.print != nullptr) {
        auto add_state = [&](const PrintStateBase::StateWithWarnings& state, const ObjectBase* obj, const char* step) {
            for (const PrintStateBase::Warning& w : state.warnings) {
                if (!w.current)
                    continue;
                json e = json::object();
                e["level"] = w.level == PrintStateBase::WarningLevel::CRITICAL ? "critical" : "warning";
                e["message"] = w.message;
                e["step"] = step;
                if (obj != nullptr) {
                    json ref = object_ref(obj);
                    if (!ref.is_null())
                        e["object"] = ref;
                }
                out.push_back(std::move(e));
            }
        };
        static const char* print_steps[] = {"wipe_tower", "skirt_brim", "gcode_export", "conflict_check"};
        for (int s = 0; s < static_cast<int>(psCount); ++s)
            add_state(h.print->step_state_with_warnings(static_cast<PrintStep>(s)), nullptr, print_steps[std::min(s, 3)]);
        static const char* object_steps[] = {"slice", "perimeters", "estimate_curled_extrusions", "prepare_infill", "infill", "ironing",
                                             "contouring", "support_material", "simplify_path", "simplify_support_path",
                                             "detect_overhangs_for_lift", "simplify_wall", "simplify_infill"};
        for (const PrintObject* po : h.print->objects())
            for (int s = 0; s < static_cast<int>(posCount); ++s)
                add_state(po->step_state_with_warnings(static_cast<PrintObjectStep>(s)), po, object_steps[std::min(s, 12)]);
    }
    if (h.result != nullptr && h.plate->is_slice_result_valid()) {
        const GCodeProcessorResult& r = *h.result;
        for (const auto& w : r.warnings) {
            // The processor stores some warnings as codes; the GUI shows them through
            // Plater::get_slice_warning_string. Same text here, the code kept in `code`.
            GCodeProcessorResult::SliceWarning copy = w;
            const std::string text = Plater::get_slice_warning_string(copy).ToUTF8().data();
            if (text.empty())
                continue; // the GUI does not show it either (e.g. not_generate_timelapse)
            json e = json::object();
            e["level"] = w.level >= 2 ? "error" : (w.level == 1 ? "warning" : "tip");
            e["message"] = text;
            if (text != w.msg && w.error_code.empty())
                e["code"] = w.msg;
            if (!w.error_code.empty())
                e["code"] = w.error_code;
            if (!w.params.empty())
                e["params"] = w.params;
            e["step"] = "gcode";
            out.push_back(std::move(e));
        }
        if (r.toolpath_outside) {
            out.push_back(json::object({{"level", "error"}, {"kind", "toolpath_outside"},
                                        {"message", "A toolpath goes outside the printable area of the plate."},
                                        {"fix", "Move the objects (or the prime tower) inward; check brim and skirt sizes."}}));
        }
        if (r.conflict_result) {
            const auto& c = *r.conflict_result;
            json e = json::object({{"level", "warning"}, {"kind", "object_collision"},
                                   {"message", "Toolpaths of \"" + c._objName1 + "\" and \"" + c._objName2 + "\" conflict at z = " +
                                                   std::to_string(round_to(c._height, 2)) + " mm."},
                                   {"z", round_to(c._height, 3)},
                                   {"objects", {c._objName1, c._objName2}},
                                   {"fix", "Move the objects apart (or the prime tower away from them)."}});
            if (c.layer >= 0)
                e["layer"] = c.layer + 1;
            out.push_back(std::move(e));
        }
        if (r.gcode_check_result.error_code != 0) {
            json e = json::object({{"level", "error"}, {"kind", "printable_area"}, {"code", r.gcode_check_result.error_code}});
            std::string what;
            const int code = r.gcode_check_result.error_code;
            if (code & 0b0001) what += "an object is outside the area a nozzle can reach; ";
            if (code & 0b0010) what += "an object exceeds the height a nozzle can reach; ";
            if (code & 0b0100) what += "toolpaths leave the plate's printable area; ";
            if (code & 0b1000) what += "toolpaths exceed the printable height; ";
            if (code & 0b10000) what += "toolpaths enter the wrapping detection area; ";
            if (!what.empty())
                what.resize(what.size() - 2);
            e["message"] = "G-code check failed: " + what + ".";
            e["fix"] = "Move or scale the objects, or assign the filaments to the nozzle that can reach them.";
            out.push_back(std::move(e));
        }
        if (!r.filament_printable_reuslt.conflict_filament.empty()) {
            std::vector<int> slots;
            for (int f : r.filament_printable_reuslt.conflict_filament)
                slots.push_back(f + 1);
            out.push_back(json::object({{"level", "error"}, {"kind", "filament_not_printable"}, {"filaments", slots},
                                        {"message", "Some filaments cannot be printed on this plate type / nozzle."},
                                        {"fix", "Change the plate type or the filaments."}}));
        }
    }
    return out;
}

namespace {

struct RoleAgg
{
    double time_s { 0. };
    double length_mm { 0. };
    double volume_mm3 { 0. };
};

struct MoveAgg
{
    std::map<ExtrusionRole, RoleAgg> roles;
    double travel_time { 0. }, other_time { 0. }, all_time { 0. };
    unsigned layers { 0 };
    double max_z { 0. };
    std::map<double, unsigned> heights; // layer height -> count (rounded to 0.001)
    double first_layer_height { 0. };
    BoundingBoxf3 extrusion_box;
    unsigned tool_changes { 0 }, retractions { 0 }, seams { 0 };
};

MoveAgg aggregate(const GCodeProcessorResult& r, int mode)
{
    MoveAgg agg;
    unsigned max_layer = 0;
    bool     any_layer = false;
    std::map<unsigned, double> layer_height;
    Vec3f last = r.moves.empty() ? Vec3f::Zero() : r.moves.front().position;
    for (const auto& m : r.moves) {
        if (m.internal_only)
            continue;
        const double t = m.time[static_cast<size_t>(mode)];
        if (m.type == EMoveType::Seam) {
            ++agg.seams;
            continue;
        }
        agg.all_time += t;
        switch (m.type) {
        case EMoveType::Extrude: {
            RoleAgg& ra = agg.roles[m.extrusion_role];
            ra.time_s += t;
            const double len = (m.position - last).norm();
            ra.length_mm += len;
            ra.volume_mm3 += len * m.mm3_per_mm;
            agg.extrusion_box.merge(m.position.cast<double>());
            agg.max_z = std::max(agg.max_z, static_cast<double>(m.position.z()));
            layer_height[m.layer_id] = std::max(layer_height[m.layer_id], static_cast<double>(m.position.z())); // top z of the layer
            max_layer = std::max(max_layer, m.layer_id);
            any_layer = true;
            break;
        }
        case EMoveType::Travel: agg.travel_time += t; break;
        case EMoveType::Tool_change: ++agg.tool_changes; agg.other_time += t; break;
        case EMoveType::Retract: ++agg.retractions; agg.other_time += t; break;
        default: agg.other_time += t; break;
        }
        last = m.position;
    }
    agg.layers = any_layer ? static_cast<unsigned>(layer_height.size()) : 0;
    // Layer heights from the steps between the layers' top z (extrusion heights vary within a layer:
    // bridges are thicker).
    bool   first = true;
    double prev_z = 0.;
    for (const auto& [id, z] : layer_height) {
        if (first) {
            agg.first_layer_height = std::round(z * 1000.) / 1000.;
            first = false;
        } else if (z - prev_z > 1e-4)
            agg.heights[std::round((z - prev_z) * 1000.) / 1000.]++;
        prev_z = z;
    }
    (void) max_layer;
    return agg;
}

} // namespace

json result_json(int plate_idx, const std::set<std::string>& sections, const std::string& time_mode)
{
    PlateHandles h = handles(plate_idx);
    const GCodeProcessorResult& r = *h.result;
    const Stats&                st = r.print_statistics;
    const int                   mode = time_mode == "silent" ? 1 : 0;
    const MoveAgg               agg = aggregate(r, mode);
    const Vec3d                 origin = plate_origin(plate_idx);
    json out = json::object();
    out["plate"] = plate_idx + 1;
    out["time_mode"] = mode == 1 ? "silent" : "normal";

    // Filament per slot from the slicer's volumes.
    struct Fil { double model, support, tower, flush, total, length, weight, cost; };
    std::map<size_t, Fil> fil;
    auto take = [&](const std::map<size_t, double>& m, double Fil::*field) {
        for (const auto& [e, v] : m) fil[e].*field += v;
    };
    take(st.model_volumes_per_extruder, &Fil::model);
    take(st.support_volumes_per_extruder, &Fil::support);
    take(st.wipe_tower_volumes_per_extruder, &Fil::tower);
    take(st.flush_per_filament, &Fil::flush);
    take(st.total_volumes_per_extruder, &Fil::total);
    double total_len = 0., total_g = 0., total_cost = 0.;
    for (auto& [e, f] : fil) {
        if (f.total <= 0.)
            f.total = f.model + f.support + f.tower + f.flush;
        const double diameter = vec_at(r.filament_diameters, e, 1.75);
        const double density = vec_at(r.filament_densities, e, 1.24);
        const double cost_kg = vec_at(r.filament_costs, e, 0.);
        filament_mm3_to_g_line(f.total, diameter, density, f.length, f.weight);
        f.cost = f.weight / 1000. * cost_kg;
        total_len += f.length;
        total_g += f.weight;
        total_cost += f.cost;
    }

    if (sections.count("summary")) {
        json s = json::object();
        s["time_s"] = round_to(st.modes[0].time, 1);
        s["time"] = format_duration(st.modes[0].time);
        if (st.modes[1].time > 0.)
            s["silent_time_s"] = round_to(st.modes[1].time, 1);
        s["prepare_time_s"] = round_to(st.modes[mode].prepare_time, 1);
        s["first_layer_time_s"] = round_to(r.initial_layer_time, 1);
        s["filament_m"] = round_to(total_len / 1000., 3);
        s["filament_g"] = round_to(total_g, 2);
        s["cost"] = round_to(total_cost, 2);
        s["layers"] = agg.layers;
        s["first_layer_height"] = round_to(agg.first_layer_height, 3);
        if (!agg.heights.empty()) {
            auto common = std::max_element(agg.heights.begin(), agg.heights.end(), [](auto& a, auto& b) { return a.second < b.second; });
            s["layer_height"] = common->first;
            s["min_layer_height"] = agg.heights.begin()->first;
            s["max_layer_height"] = agg.heights.rbegin()->first;
        }
        s["height_mm"] = round_to(agg.max_z, 3);
        s["filament_changes"] = st.total_filament_changes;
        s["extruder_changes"] = st.total_extruder_changes;
        s["tool_change_time_s"] = round_to(st.total_tool_change_time, 1);
        s["travel_m"] = round_to(st.total_travel_distance / 1000., 2);
        s["travel_moves"] = st.total_travel_moves;
        s["retractions"] = agg.retractions;
        double tower = 0., flush = 0., support = 0.;
        for (auto& [e, f] : fil) { tower += f.tower; flush += f.flush; support += f.support; }
        s["support_mm3"] = round_to(support, 1);
        s["wipe_tower_mm3"] = round_to(tower, 1);
        s["flush_mm3"] = round_to(flush, 1);
        s["time_split_s"] = json::object({{"extrusion", round_to(agg.all_time - agg.travel_time - agg.other_time, 1)},
                                          {"travel", round_to(agg.travel_time, 1)},
                                          {"other", round_to(agg.other_time, 1)}});
        if (agg.extrusion_box.defined) {
            BoundingBoxf3 b = agg.extrusion_box;
            b.min -= origin; b.max -= origin;
            s["toolpath_box"] = box_json(b);
        }
        out["summary"] = std::move(s);
    }
    if (sections.count("filaments")) {
        json list = json::array();
        for (auto& [e, f] : fil) {
            json j = json::object();
            j["filament"] = e + 1;
            j["model_mm3"] = round_to(f.model, 1);
            j["support_mm3"] = round_to(f.support, 1);
            j["wipe_tower_mm3"] = round_to(f.tower, 1);
            j["flush_mm3"] = round_to(f.flush, 1);
            j["total_mm3"] = round_to(f.total, 1);
            j["length_m"] = round_to(f.length / 1000., 3);
            j["weight_g"] = round_to(f.weight, 2);
            j["cost"] = round_to(f.cost, 2);
            list.push_back(std::move(j));
        }
        out["filaments"] = std::move(list);
    }
    if (sections.count("features")) {
        json list = json::array();
        std::vector<std::pair<ExtrusionRole, RoleAgg>> roles(agg.roles.begin(), agg.roles.end());
        std::sort(roles.begin(), roles.end(), [](auto& a, auto& b) { return a.second.time_s > b.second.time_s; });
        const double total_time = st.modes[mode].time > 0. ? st.modes[mode].time : agg.all_time;
        for (auto& [role, ra] : roles) {
            json j = json::object();
            j["feature"] = role_label(role);
            j["key"] = snake(role_label(role));
            j["time_s"] = round_to(ra.time_s, 1);
            j["percent"] = total_time > 0. ? round_to(100. * ra.time_s / total_time, 1) : 0.;
            j["length_m"] = round_to(ra.length_mm / 1000., 3);
            j["volume_mm3"] = round_to(ra.volume_mm3, 1);
            auto used = st.used_filaments_per_role.find(role);
            if (used != st.used_filaments_per_role.end()) {
                j["filament_m"] = round_to(used->second.first, 3);
                j["filament_g"] = round_to(used->second.second, 2);
            }
            list.push_back(std::move(j));
        }
        out["features"] = std::move(list);
    }
    if (sections.count("objects")) {
        json list = json::array();
        for (size_t k = 0; k < r.object_masses.size(); ++k) {
            const auto& om = r.object_masses[k];
            json j = json::object();
            j["name"] = om.name;
            if (om.instance >= 0)
                j["instance"] = om.instance;
            // Map the name back to an object id when it is unique on the plate.
            int matches = 0;
            const ModelObject* match = nullptr;
            for (const ModelObject* mo : plater().model().objects)
                if (mo->name == om.name) { ++matches; match = mo; }
            if (matches == 1)
                j["id"] = Ids::of(*match);
            const auto total = om.total();
            // ObjectMass is in mg (mm³ at g/cm³), as GCodeStatistics notes.
            j["weight_g"] = round_to(0.001 * total.mass, 2);
            j["volume_mm3"] = round_to(total.volume, 1);
            if (k < r.support_masses.size())
                j["support_g"] = round_to(0.001 * r.support_masses[k].total().mass, 2);
            BoundingBoxf3 b = om.box;
            if (b.defined) {
                b.min -= origin; b.max -= origin;
                j["box"] = box_json(b);
            }
            list.push_back(std::move(j));
        }
        out["objects"] = std::move(list);
    }
    if (sections.count("warnings"))
        out["warnings"] = slice_warnings(plate_idx);
    return out;
}

json compact_summary(int plate_idx)
{
    json out = plate_state(plate_idx);
    if (plate_has_result(plate_idx)) {
        json r = result_json(plate_idx, {"summary"}, "normal");
        const json& s = r["summary"];
        for (const char* key : {"time_s", "time", "filament_g", "filament_m", "cost", "layers", "layer_height", "height_mm", "filament_changes"})
            if (s.contains(key))
                out[key] = s[key];
        json w = slice_warnings(plate_idx);
        out["warning_count"] = w.size();
        if (!w.empty()) {
            json first = json::array();
            for (size_t i = 0; i < w.size() && i < 5; ++i)
                first.push_back(w[i].value("message", ""));
            out["warnings"] = std::move(first);
        }
    } else if (out.value("state", "") != "slicing" && out.value("state", "") != "empty") {
        bool has_errors = false;
        json v = validation(plate_idx, false, has_errors);
        if (has_errors)
            out["errors"] = v["errors"];
    }
    return out;
}

json gcode_file_info(const std::string& path)
{
    json out = json::object();
    boost::system::error_code ec;
    const auto size = boost::filesystem::file_size(path, ec);
    out["bytes"] = ec ? 0 : static_cast<long long>(size);
    boost::nowide::ifstream in(path, std::ios::binary);
    if (!in)
        return out;
    // Header comments (first lines) and footer comments (last ~64 kB) carry the statistics.
    std::vector<std::string> lines;
    std::string              line;
    for (int i = 0; i < 400 && std::getline(in, line); ++i)
        lines.push_back(line);
    in.clear();
    in.seekg(0, std::ios::end);
    const std::streamoff end = in.tellg();
    const std::streamoff tail = std::min<std::streamoff>(end, 96 * 1024);
    in.seekg(end - tail);
    bool skip_partial = tail < end;
    while (std::getline(in, line)) {
        if (skip_partial) { skip_partial = false; continue; }
        lines.push_back(line);
    }
    json comments = json::object();
    long layer_changes = 0;
    for (std::string l : lines) {
        if (!l.empty() && l.back() == '\r')
            l.pop_back();
        if (l.rfind("; ", 0) != 0)
            continue;
        std::string body = l.substr(2);
        // "key = value" and "key: value" forms.
        size_t eq = body.find(" = ");
        size_t colon = body.find(": ");
        std::string key, value;
        if (eq != std::string::npos) { key = body.substr(0, eq); value = body.substr(eq + 3); }
        else if (colon != std::string::npos) { key = body.substr(0, colon); value = body.substr(colon + 2); }
        else continue;
        if (key.size() > 80 || value.size() > 400)
            continue;
        static const char* interesting[] = {"estimated printing time", "total estimated time", "model printing time", "filament used",
                                            "total filament used", "total filament", "filament cost", "total cost", "total layer",
                                            "total layers count", "max_z_height", "first layer printing time", "total filament change",
                                            "printing_time", "estimated first layer", "filament_density", "filament_diameter"};
        bool keep = false;
        for (const char* k : interesting)
            if (key.find(k) != std::string::npos) { keep = true; break; }
        if (!keep && key.rfind("HEADER", 0) != 0)
            continue;
        if (!comments.contains(key))
            comments[key] = value;
    }
    (void) layer_changes;
    out["comments"] = std::move(comments);
    return out;
}

}}}} // namespace Slic3r::GUI::Mcp::Slice
