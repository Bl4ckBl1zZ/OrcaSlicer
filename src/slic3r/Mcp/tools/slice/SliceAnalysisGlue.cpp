#include "SliceAnalysisGlue.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <set>

#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"

#include "../../McpError.hpp"
#include "../../McpIds.hpp"
#include "SliceData.hpp"

#if MCP_SLICE_HAS_ANALYSIS
#include "libslic3r/SliceAnalysis.hpp"
#include "libslic3r/SliceAnalysisJson.hpp"
#endif

namespace Slic3r { namespace GUI { namespace Mcp { namespace Slice { namespace Analysis {

namespace {

json from_basic(const nlohmann::json& j) { return json::parse(j.dump()); }

// Cap every array in `doc` at max_items, recording the full length next to it.
void cap_lists(json& doc, int max_items)
{
    if (doc.is_object()) {
        std::vector<std::pair<std::string, size_t>> totals;
        for (auto it = doc.begin(); it != doc.end(); ++it) {
            json& v = it.value();
            if (v.is_array() && static_cast<int>(v.size()) > max_items) {
                totals.emplace_back(it.key(), v.size());
                json kept = json::array();
                for (int i = 0; i < max_items; ++i)
                    kept.push_back(v[static_cast<size_t>(i)]);
                v = std::move(kept);
            }
            cap_lists(v, max_items);
        }
        for (auto& [key, n] : totals)
            doc[key + "_total"] = n;
    } else if (doc.is_array())
        for (json& v : doc)
            cap_lists(v, max_items);
}

struct Ctx
{
    double ox { 0. }, oy { 0. };
    std::map<int, json> label_to_object; // G-code label id -> {id, instance, name}
    std::vector<json>   print_objects;   // Print::objects() index -> {id, name}
};

bool is_layer_key(const std::string& k)
{
    static const std::set<std::string> keys = {"layer", "first_layer", "last_layer", "first_layer_at_limit", "last_layer_at_limit",
                                               "min_layer_time_layer", "first_contact_layer", "last_contact_layer", "index"};
    return keys.count(k) > 0;
}

void transform(json& doc, const Ctx& c, const std::string& parent_key)
{
    if (doc.is_object()) {
        json extra = json::object();
        for (auto it = doc.begin(); it != doc.end(); ++it) {
            const std::string& k = it.key();
            json&              v = it.value();
            if (v.is_number_float())
                v = round_to(v.get<double>(), 3);
            if (v.is_number()) {
                if (k == "x" || k == "cx" || k == "mean_x" || k == "wipe_tower_x")
                    v = round_to(v.get<double>() - c.ox, 3);
                else if (k == "y" || k == "cy" || k == "mean_y" || k == "min_y" || k == "max_y" || k == "wipe_tower_y")
                    v = round_to(v.get<double>() - c.oy, 3);
                else if (is_layer_key(k) && v.is_number_integer() && v.get<long long>() >= 0)
                    v = v.get<long long>() + 1;
                else if ((k == "filament" || ((k == "from" || k == "to") && parent_key == "transitions")) && v.is_number_integer() &&
                         v.get<long long>() >= 0)
                    v = v.get<long long>() + 1; // filament slots are 1-based in every tool
                else if (k == "object" && v.is_number_integer()) {
                    auto f = c.label_to_object.find(v.get<int>());
                    if (f != c.label_to_object.end())
                        extra["object_id"] = f->second.value("id", "");
                } else if (k == "object_index" && v.is_number_integer()) {
                    const int i = v.get<int>();
                    if (i >= 0 && i < static_cast<int>(c.print_objects.size()))
                        extra["object_id"] = c.print_objects[static_cast<size_t>(i)].value("id", "");
                }
            } else if ((k == "min" || k == "max") && v.is_array() && v.size() >= 2 && v[0].is_number() && v[1].is_number()) {
                v[0] = round_to(v[0].get<double>() - c.ox, 3);
                v[1] = round_to(v[1].get<double>() - c.oy, 3);
            } else if ((k == "filaments" || k == "length_by_filament" || k == "flush_per_filament") && v.is_array() && !v.empty() && v[0].is_array()) {
                for (json& pair : v)
                    if (pair.is_array() && !pair.empty() && pair[0].is_number_integer())
                        pair[0] = pair[0].get<long long>() + 1;
            } else if (k == "transition_counts" && v.is_array()) {
                for (json& entry : v)
                    if (entry.is_array() && !entry.empty() && entry[0].is_array())
                        for (json& f : entry[0])
                            if (f.is_number_integer())
                                f = f.get<long long>() + 1;
            } else if ((k == "changes_per_layer" || k == "ranges") && v.is_array()) {
                for (json& pair : v)
                    if (pair.is_array())
                        for (size_t i = 0; i < pair.size() && i < (k == "ranges" ? 2u : 1u); ++i)
                            if (pair[i].is_number_integer() && pair[i].get<long long>() >= 0)
                                pair[i] = pair[i].get<long long>() + 1;
            } else
                transform(v, c, k);
        }
        for (auto it = extra.begin(); it != extra.end(); ++it)
            doc[it.key()] = it.value();
    } else if (doc.is_array())
        for (json& v : doc) {
            if (v.is_number_float())
                v = round_to(v.get<double>(), 3);
            transform(v, c, parent_key);
        }
}

Ctx context_for(int plate_idx)
{
    Ctx c;
    PlateHandles h = handles(plate_idx);
    if (h.plate != nullptr) {
        const Vec3d o = h.plate->get_origin();
        c.ox = o.x();
        c.oy = o.y();
    }
    for (const ModelObject* mo : wxGetApp().plater()->model().objects)
        for (const ModelInstance* mi : mo->instances)
            c.label_to_object[static_cast<int>(mi->get_labeled_id())] =
                json::object({{"id", Ids::of(*mo)}, {"instance", Ids::of(*mi)}, {"name", mo->name}});
    if (h.print != nullptr)
        for (const PrintObject* po : h.print->objects()) {
            json ref = object_ref(po);
            c.print_objects.push_back(ref.is_null() ? json::object() : ref);
        }
    return c;
}

#if MCP_SLICE_HAS_ANALYSIS
namespace SA = Slic3r::SliceAnalysis;

struct Entry
{
    std::string                        key;
    int                                mode { 0 };
    std::unique_ptr<SA::MoveAnalysis>  analysis;
    SA::Limits                         limits;
    const Print*                       print { nullptr };
    bool                               has_prev { false };
    SA::Summary                        prev;
    double                             prev_time_s { 0. };
    std::chrono::system_clock::time_point computed_at, prev_at;
};

std::map<int, Entry>& cache()
{
    static std::map<int, Entry> entries;
    return entries;
}

std::string key_of(const PlateHandles& h, int mode)
{
    const GCodeProcessorResult& r = *h.result;
    return std::to_string(reinterpret_cast<uintptr_t>(h.result)) + ":" + std::to_string(r.id) + ":" + std::to_string(r.moves.size()) + ":" +
           std::to_string(r.print_statistics.modes[0].time) + ":" + std::to_string(mode);
}

Entry& entry(int plate_idx)
{
    ensure(plate_idx);
    return cache()[plate_idx];
}

SA::Box2 box2_from(const json& box, const Ctx& c)
{
    SA::Box2 b;
    if (box.is_object() && box.contains("min") && box.contains("max")) {
        const json& mn = box["min"];
        const json& mx = box["max"];
        b = SA::Box2(mn[0].get<double>() + c.ox, mn[1].get<double>() + c.oy, mx[0].get<double>() + c.ox, mx[1].get<double>() + c.oy);
    }
    return b;
}
#endif

} // namespace

bool available() { return MCP_SLICE_HAS_ANALYSIS != 0; }

void require()
{
    if (!available())
        fail("not_available", "The slice analysis module is not built into this version.",
             "Use slice_result for the slicer's own statistics.");
}

json to_plate_frame(json doc, int plate_idx)
{
    transform(doc, context_for(plate_idx), "");
    return doc;
}

#if MCP_SLICE_HAS_ANALYSIS

double ensure(int plate_idx, const std::string& time_mode)
{
    PlateHandles h = handles(plate_idx);
    if (h.result == nullptr || h.print == nullptr)
        fail("not_sliced", "Plate " + std::to_string(plate_idx + 1) + " has no slice result.", "Call slice first.");
    const int         mode = time_mode == "silent" ? 1 : 0;
    const std::string key = key_of(h, mode);
    Entry&            e = cache()[plate_idx];
    if (e.analysis && e.key == key)
        return 0.;
    const auto start = std::chrono::steady_clock::now();
    if (e.analysis && e.mode == mode) {
        // A new result for this plate: the old summary becomes the "previous" one.
        e.prev = e.analysis->summary();
        e.prev_time_s = e.prev.time_s;
        e.has_prev = true;
        e.prev_at = e.computed_at;
    }
    SA::Options opt;
    opt.time_mode = mode;
    e.analysis = std::make_unique<SA::MoveAnalysis>(*h.result, h.print, opt);
    e.limits = SA::limits_from_print(*h.print);
    e.print = h.print;
    e.key = key;
    e.mode = mode;
    e.computed_at = std::chrono::system_clock::now();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

void remember(int plate_idx)
{
    if (plate_has_result(plate_idx))
        ensure(plate_idx);
}

json summary(int plate_idx)
{
    Entry& e = entry(plate_idx);
    nlohmann::json j = e.analysis->summary();
    return to_plate_frame(from_basic(j), plate_idx);
}

json layers(int plate_idx, int from, int to, int every, const std::vector<std::string>& columns, int limit)
{
    Entry&       e = entry(plate_idx);
    const auto&  rows = e.analysis->layers();
    const int    count = static_cast<int>(rows.size());
    from = std::max(1, from);
    to = to <= 0 ? count : std::min(count, to);
    every = std::max(1, every);
    std::set<std::string> want(columns.begin(), columns.end());
    static const std::vector<std::string> default_cols = {"index", "z", "height", "time_s", "volume_mm3", "max_flow", "max_speed", "fan_max",
                                                          "temp_max", "tool_changes", "has_support", "has_bridge", "has_overhang_wall"};
    if (want.empty())
        want.insert(default_cols.begin(), default_cols.end());
    want.insert("index");
    json out = json::object();
    json list = json::array();
    int  emitted = 0;
    int  next = -1;
    for (int l = from; l <= to; l += every) {
        if (emitted >= limit) {
            next = l;
            break;
        }
        nlohmann::json j = rows[static_cast<size_t>(l - 1)];
        json row = from_basic(j);
        json kept = json::object();
        for (auto it = row.begin(); it != row.end(); ++it)
            if (want.count(it.key()))
                kept[it.key()] = it.value();
        list.push_back(std::move(kept));
        ++emitted;
    }
    out["plate"] = plate_idx + 1;
    out["layer_count"] = count;
    out["rows"] = to_plate_frame(std::move(list), plate_idx);
    if (next > 0) {
        out["truncated"] = true;
        out["next"] = next;
    }
    return out;
}

json check(int plate_idx, const std::string& name, const json& params, int max_items)
{
    Entry& e = entry(plate_idx);
    const SA::MoveAnalysis& a = *e.analysis;
    const Print*            print = e.print;
    const size_t            n = static_cast<size_t>(std::max(1, max_items));
    nlohmann::json          j;
    auto num = [&](const char* key, double dflt) { return params.contains(key) ? params[key].get<double>() : dflt; };
    if (name == "flow")
        j = SA::check_flow(a, e.limits, num("tolerance", 0.02), n);
    else if (name == "speeds")
        j = SA::check_speeds(a, e.limits, num("tolerance", 0.02));
    else if (name == "cooling")
        j = SA::check_cooling(a, e.limits);
    else if (name == "supports")
        j = SA::check_supports(a, print, n);
    else if (name == "overhangs")
        j = SA::check_overhangs(a, print, n);
    else if (name == "unsupported") {
        SA::UnsupportedOptions opt;
        opt.tolerance_mm = num("tolerance_mm", -1.);
        opt.min_area_mm2 = num("min_area_mm2", 0.05);
        opt.report_distance_mm = num("report_distance_mm", 1.);
        opt.max_regions = n;
        j = SA::check_unsupported(*print, opt);
    } else if (name == "first_layer")
        j = SA::check_first_layer(*print, &a, num("aspect_warning", 3.));
    else if (name == "seams")
        j = SA::check_seams(a, num("align_distance_mm", 1.), n);
    else if (name == "thin_features")
        j = SA::check_thin_features(a, print, num("narrow_width_mm", -1.), num("min_area_mm2", 0.1), n);
    else if (name == "multi_material")
        j = SA::check_multi_material(a, print);
    else
        fail("invalid_argument", "Unknown check \"" + name + "\".");
    json out = to_plate_frame(from_basic(j), plate_idx);
    cap_lists(out, max_items);
    return out;
}

json section(int plate_idx, int layer, double z, bool by_z, const json& box)
{
    Entry&   e = entry(plate_idx);
    Ctx      c = context_for(plate_idx);
    SA::Box2 b = box2_from(box, c);
    int      l = by_z ? e.analysis->layer_at_z(z) : layer - 1;
    if (l < 0 || l >= static_cast<int>(e.analysis->layers().size()))
        fail("invalid_argument", by_z ? "No layer near z = " + std::to_string(z) + "." : "`layer` " + std::to_string(layer) + " is out of range.",
             "Layers are numbered 1 to " + std::to_string(e.analysis->layers().size()) + ".");
    const double layer_z = e.analysis->layers()[static_cast<size_t>(l)].z;
    json out = json::object();
    nlohmann::json moves = SA::section_from_moves(*e.analysis, l, b);
    out["moves"] = from_basic(moves);
    if (e.print != nullptr) {
        nlohmann::json geo = SA::section_from_print(*e.print, by_z ? z : layer_z, b);
        out["geometry"] = from_basic(geo);
    }
    out = to_plate_frame(std::move(out), plate_idx);
    out["layer"] = l + 1;
    out["z"] = round_to(layer_z, 3);
    cap_lists(out, 20);
    return out;
}

json box_query(int plate_idx, const json& box, const std::vector<std::string>& roles, bool support_only)
{
    Entry&   e = entry(plate_idx);
    Ctx      c = context_for(plate_idx);
    SA::Box3 b3(box2_from(box, c));
    if (box["min"].size() >= 3)
        b3.min_z = box["min"][2].get<double>();
    if (box["max"].size() >= 3)
        b3.max_z = box["max"][2].get<double>();
    nlohmann::json j = support_only ? SA::support_in_box(*e.analysis, b3) : SA::extrusions_in_box(*e.analysis, b3, roles);
    json out = to_plate_frame(from_basic(j), plate_idx);
    cap_lists(out, 20);
    return out;
}

json diff(int plate_idx, int max_items, double rel_tolerance)
{
    Entry& e = entry(plate_idx);
    json   out = json::object();
    out["plate"] = plate_idx + 1;
    if (!e.has_prev) {
        out["available"] = false;
        out["message"] = "No earlier result of this plate is known: the diff compares with the result before the last re-slice.";
        return out;
    }
    const auto entries = SA::diff(e.prev, e.analysis->summary(), rel_tolerance);
    json list = json::array();
    for (const auto& d : entries) {
        if (static_cast<int>(list.size()) >= max_items)
            break;
        nlohmann::json j = d;
        list.push_back(from_basic(j));
    }
    out["available"] = true;
    out["changes"] = std::move(list);
    out["changes_total"] = entries.size();
    out["before"] = json::object({{"time_s", round_to(e.prev.time_s, 1)}, {"weight_g", round_to(e.prev.total_weight_g, 2)},
                                  {"layers", e.prev.layer_count}});
    const SA::Summary& now = e.analysis->summary();
    out["after"] = json::object({{"time_s", round_to(now.time_s, 1)}, {"weight_g", round_to(now.total_weight_g, 2)}, {"layers", now.layer_count}});
    return out;
}

#else // !MCP_SLICE_HAS_ANALYSIS

double ensure(int, const std::string&) { require(); return 0.; }
void   remember(int) {}
json   summary(int) { require(); return nullptr; }
json   layers(int, int, int, int, const std::vector<std::string>&, int) { require(); return nullptr; }
json   check(int, const std::string&, const json&, int) { require(); return nullptr; }
json   section(int, int, double, bool, const json&) { require(); return nullptr; }
json   box_query(int, const json&, const std::vector<std::string>&, bool) { require(); return nullptr; }
json   diff(int, int, double) { require(); return nullptr; }

#endif

}}}}} // namespace Slic3r::GUI::Mcp::Slice::Analysis
