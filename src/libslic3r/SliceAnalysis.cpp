#include "SliceAnalysis.hpp"

#include "libslic3r/AABBTreeLines.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <set>
#include <unordered_map>

namespace Slic3r { namespace SliceAnalysis {

namespace {

constexpr double kPi = 3.14159265358979323846;

inline double sq(double x) { return x * x; }

// Length of the part of segment (x0,y0)-(x1,y1) inside box (Liang-Barsky); returns the parameter
// interval in t0/t1. An undefined box means "everything".
double clip_fraction(double x0, double y0, double x1, double y1, const Box2 &box, double &t0, double &t1)
{
    t0 = 0.;
    t1 = 1.;
    if (!box.defined())
        return 1.;
    const double dx = x1 - x0, dy = y1 - y0;
    const double p[4] = {-dx, dx, -dy, dy};
    const double q[4] = {x0 - box.min_x, box.max_x - x0, y0 - box.min_y, box.max_y - y0};
    for (int i = 0; i < 4; ++i) {
        if (p[i] == 0.) {
            if (q[i] < 0.)
                return 0.;
        } else {
            const double r = q[i] / p[i];
            if (p[i] < 0.) {
                if (r > t1) return 0.;
                if (r > t0) t0 = r;
            } else {
                if (r < t0) return 0.;
                if (r < t1) t1 = r;
            }
        }
    }
    return std::max(0., t1 - t0);
}

double clip_fraction(double x0, double y0, double x1, double y1, const Box2 &box)
{
    double t0, t1;
    return clip_fraction(x0, y0, x1, y1, box, t0, t1);
}

bool z_in(double z, const Box3 &b) { return z >= b.min_z && z <= b.max_z; }

int verdict_rank(const std::string &v)
{
    if (v == "problem") return 3;
    if (v == "warning") return 2;
    if (v == "ok") return 1;
    return 0; // not_applicable
}

struct ModeHistogram
{
    std::map<long long, double> weights;
    void   add(double value, double weight) { weights[(long long) std::llround(value * 1000.)] += weight; }
    double mode() const
    {
        double best = -1., value = 0.;
        for (const auto &kv : weights)
            if (kv.second > best) { best = kv.second; value = double(kv.first) / 1000.; }
        return value;
    }
};

double filament_area(const GCodeProcessorResult &r, int f)
{
    const double d = (f >= 0 && size_t(f) < r.filament_diameters.size()) ? double(r.filament_diameters[f]) : 1.75;
    return kPi * sq(0.5 * d);
}
// As configured (0 when not set), like the slicer's own weight figures.
double filament_density(const GCodeProcessorResult &r, int f)
{
    return (f >= 0 && size_t(f) < r.filament_densities.size()) ? double(r.filament_densities[f]) : 0.;
}
double filament_cost(const GCodeProcessorResult &r, int f)
{
    return (f >= 0 && size_t(f) < r.filament_costs.size()) ? double(r.filament_costs[f]) : 0.;
}

// ---- Clustering of segments within a layer and across consecutive layers ---------------------------------

struct Seg
{
    int   layer;
    float z;
    Box2  box;
    double length;
    double volume;
    int   object;
};

// Union-find.
struct DSU
{
    std::vector<size_t> p;
    explicit DSU(size_t n) : p(n) { for (size_t i = 0; i < n; ++i) p[i] = i; }
    size_t find(size_t x) { while (p[x] != x) { p[x] = p[p[x]]; x = p[x]; } return x; }
    void   unite(size_t a, size_t b) { a = find(a); b = find(b); if (a != b) p[b] = a; }
};

// Segments (sorted by layer) are grouped when their boxes grown by gap overlap in XY and their layers
// are equal or adjacent (layer difference <= 1).
std::vector<Cluster> cluster_segments(std::vector<Seg> segs, double gap, size_t max_clusters)
{
    std::vector<Cluster> out;
    if (segs.empty())
        return out;
    std::sort(segs.begin(), segs.end(), [](const Seg &a, const Seg &b) { return a.layer < b.layer || (a.layer == b.layer && a.box.min_x < b.box.min_x); });
    DSU dsu(segs.size());
    // Sweep per window of two consecutive layers on min_x.
    size_t layer_begin = 0;
    std::vector<size_t> window;
    while (layer_begin < segs.size()) {
        size_t layer_end = layer_begin;
        while (layer_end < segs.size() && segs[layer_end].layer == segs[layer_begin].layer)
            ++layer_end;
        // Candidates: this layer and the previous layer (if adjacent).
        window.clear();
        for (size_t i = layer_begin; i < layer_end; ++i)
            window.push_back(i);
        if (layer_begin > 0) {
            const int prev_layer = segs[layer_begin - 1].layer;
            if (prev_layer >= segs[layer_begin].layer - 1) {
                size_t k = layer_begin;
                while (k > 0 && segs[k - 1].layer == prev_layer)
                    --k;
                for (size_t i = k; i < layer_begin; ++i)
                    window.push_back(i);
            }
        }
        std::sort(window.begin(), window.end(), [&segs](size_t a, size_t b) { return segs[a].box.min_x < segs[b].box.min_x; });
        // Active list sweep.
        std::vector<size_t> active;
        for (size_t idx : window) {
            const Box2 bi = segs[idx].box.inflated(gap);
            size_t     w  = 0;
            for (size_t a : active) {
                if (segs[a].box.max_x + gap < bi.min_x)
                    continue; // drop
                active[w++] = a;
                const bool cur_layer_i = segs[idx].layer == segs[layer_begin].layer;
                const bool cur_layer_a = segs[a].layer == segs[layer_begin].layer;
                if ((cur_layer_i || cur_layer_a) && bi.intersects(segs[a].box))
                    dsu.unite(a, idx);
            }
            active.resize(w);
            active.push_back(idx);
        }
        layer_begin = layer_end;
    }
    std::unordered_map<size_t, size_t> root_to_cluster;
    for (size_t i = 0; i < segs.size(); ++i) {
        const size_t r  = dsu.find(i);
        auto         it = root_to_cluster.find(r);
        if (it == root_to_cluster.end()) {
            it = root_to_cluster.emplace(r, out.size()).first;
            Cluster c;
            c.first_layer = c.last_layer = segs[i].layer;
            c.min_z = c.max_z = segs[i].z;
            c.object          = segs[i].object;
            out.push_back(c);
        }
        Cluster &c = out[it->second];
        c.first_layer = std::min(c.first_layer, segs[i].layer);
        c.last_layer  = std::max(c.last_layer, segs[i].layer);
        c.min_z       = std::min(c.min_z, double(segs[i].z));
        c.max_z       = std::max(c.max_z, double(segs[i].z));
        c.box.merge(segs[i].box);
        c.length_mm += segs[i].length;
        c.volume_mm3 += segs[i].volume;
        c.longest_segment_mm = std::max(c.longest_segment_mm, segs[i].length);
        if (c.object != segs[i].object)
            c.object = -1;
    }
    std::sort(out.begin(), out.end(), [](const Cluster &a, const Cluster &b) { return a.length_mm > b.length_mm; });
    if (out.size() > max_clusters)
        out.resize(max_clusters);
    return out;
}

// ---- Config helpers --------------------------------------------------------------------------------------

double cfg_value(const DynamicPrintConfig &config, const std::string &key, size_t idx = 0, double percent_of = 0.)
{
    const ConfigOption *opt = config.option(key);
    if (opt == nullptr)
        return 0.;
    // Most derived first: FloatOrPercent -> Percent -> Float.
    if (auto o = dynamic_cast<const ConfigOptionFloatOrPercent *>(opt)) return o->percent ? o->value * percent_of / 100. : o->value;
    if (auto o = dynamic_cast<const ConfigOptionFloat *>(opt)) return o->value;
    if (auto o = dynamic_cast<const ConfigOptionInt *>(opt)) return o->value;
    if (auto o = dynamic_cast<const ConfigOptionFloats *>(opt)) return o->values.empty() ? 0. : o->get_at(idx);
    if (auto o = dynamic_cast<const ConfigOptionFloatsNullable *>(opt)) {
        if (o->values.empty()) return 0.;
        const double v = o->get_at(idx);
        return std::isnan(v) ? 0. : v;
    }
    if (auto o = dynamic_cast<const ConfigOptionFloatsOrPercents *>(opt)) {
        if (o->values.empty()) return 0.;
        const FloatOrPercent v = o->get_at(idx);
        return v.percent ? v.value * percent_of / 100. : v.value;
    }
    if (auto o = dynamic_cast<const ConfigOptionFloatsOrPercentsNullable *>(opt)) {
        if (o->values.empty()) return 0.;
        const FloatOrPercent v = o->get_at(idx);
        if (std::isnan(v.value)) return 0.;
        return v.percent ? v.value * percent_of / 100. : v.value;
    }
    if (auto o = dynamic_cast<const ConfigOptionInts *>(opt)) return o->values.empty() ? 0. : o->get_at(idx);
    if (auto o = dynamic_cast<const ConfigOptionPercents *>(opt)) return o->values.empty() ? 0. : o->get_at(idx);
    return 0.;
}

std::vector<double> cfg_vector(const DynamicPrintConfig &config, const std::string &key)
{
    std::vector<double> out;
    const ConfigOption *opt = config.option(key);
    if (opt == nullptr)
        return out;
    if (auto o = dynamic_cast<const ConfigOptionFloats *>(opt)) out.assign(o->values.begin(), o->values.end());
    else if (auto o = dynamic_cast<const ConfigOptionFloatsNullable *>(opt)) out.assign(o->values.begin(), o->values.end());
    else if (auto o = dynamic_cast<const ConfigOptionInts *>(opt)) out.assign(o->values.begin(), o->values.end());
    else out.push_back(cfg_value(config, key));
    return out;
}

// ---- Print geometry helpers -------------------------------------------------------------------------------

// Visit every ExtrusionPath with the inset index of its nearest entity that has one.
void visit_paths(const ExtrusionEntity &e, int inset, const std::function<void(const ExtrusionPath &, int)> &fn)
{
    const int my_inset = e.inset_idx >= 0 ? e.inset_idx : inset;
    if (auto c = dynamic_cast<const ExtrusionEntityCollection *>(&e)) {
        for (const ExtrusionEntity *ee : c->entities)
            visit_paths(*ee, my_inset, fn);
    } else if (auto l = dynamic_cast<const ExtrusionLoop *>(&e)) {
        for (const ExtrusionPath &p : l->paths)
            fn(p, p.inset_idx >= 0 ? p.inset_idx : my_inset);
    } else if (auto m = dynamic_cast<const ExtrusionMultiPath *>(&e)) {
        for (const ExtrusionPath &p : m->paths)
            fn(p, p.inset_idx >= 0 ? p.inset_idx : my_inset);
    } else if (auto p = dynamic_cast<const ExtrusionPath *>(&e)) {
        fn(*p, my_inset);
    }
}

void visit_paths(const ExtrusionEntityCollection &c, const std::function<void(const ExtrusionPath &, int)> &fn)
{
    for (const ExtrusionEntity *e : c.entities)
        visit_paths(*e, -1, fn);
}

// Area covered by the support extrusions of every support layer: (print_z, covered area), object frame.
std::vector<std::pair<double, ExPolygons>> support_footprints(const PrintObject &object)
{
    std::vector<std::pair<double, ExPolygons>> out(object.support_layers().size());
    tbb::parallel_for(tbb::blocked_range<size_t>(0, out.size()), [&](const tbb::blocked_range<size_t> &range) {
        for (size_t i = range.begin(); i < range.end(); ++i) {
            const SupportLayer *sl = object.support_layers()[i];
            if (!sl->support_islands.empty()) {
                // The support area the generator filled (normal supports).
                out[i] = {sl->print_z, sl->support_islands};
            } else {
                // Tree supports: the area the extrusions cover, closed over the gaps between lines.
                Polygons covered;
                sl->support_fills.polygons_covered_by_width(covered, float(scale_(0.01)));
                out[i] = {sl->print_z, offset_ex(offset_ex(union_ex(covered), float(scale_(1.))), -float(scale_(1.)))};
            }
        }
    });
    return out;
}

// Union of the support footprints whose top is in [bottom_z - gap, bottom_z + eps].
ExPolygons support_below(const std::vector<std::pair<double, ExPolygons>> &footprints, double bottom_z, double gap)
{
    ExPolygons out;
    for (const auto &f : footprints)
        if (f.first <= bottom_z + EPSILON + 0.001 && f.first >= bottom_z - gap)
            append(out, f.second);
    return out.size() > 1 ? union_ex(out) : out;
}

Box2 to_box(const BoundingBox &bb, const Point &shift)
{
    if (!bb.defined)
        return Box2();
    return Box2(unscale<double>(bb.min.x() + shift.x()), unscale<double>(bb.min.y() + shift.y()), unscale<double>(bb.max.x() + shift.x()),
                unscale<double>(bb.max.y() + shift.y()));
}

Polygon box_polygon(const Box2 &b, const Point &shift)
{
    // World box -> object frame (minus the instance shift), scaled.
    const coord_t x0 = coord_t(scale_(b.min_x)) - shift.x(), y0 = coord_t(scale_(b.min_y)) - shift.y();
    const coord_t x1 = coord_t(scale_(b.max_x)) - shift.x(), y1 = coord_t(scale_(b.max_y)) - shift.y();
    Polygon p;
    p.points = {Point(x0, y0), Point(x1, y0), Point(x1, y1), Point(x0, y1)};
    return p;
}

double area_mm2(const ExPolygons &e) { return area(e) * SCALING_FACTOR * SCALING_FACTOR; }
double area_mm2(const ExPolygon &e) { return e.area() * SCALING_FACTOR * SCALING_FACTOR; }

std::string object_name(const PrintObject &o) { return o.model_object() ? o.model_object()->name : std::string(); }

} // namespace

// ------------------------------------------------------------------------------------------------
// Common
// ------------------------------------------------------------------------------------------------

void Box2::merge(double x, double y)
{
    min_x = std::min(min_x, x);
    min_y = std::min(min_y, y);
    max_x = std::max(max_x, x);
    max_y = std::max(max_y, y);
}

void Box2::merge(const Box2 &b)
{
    if (!b.defined())
        return;
    merge(b.min_x, b.min_y);
    merge(b.max_x, b.max_y);
}

bool Box2::intersects(const Box2 &b) const
{
    return defined() && b.defined() && min_x <= b.max_x && b.min_x <= max_x && min_y <= b.max_y && b.min_y <= max_y;
}

void Verdict::raise(const char *level, const std::string &msg)
{
    if (verdict_rank(level) > verdict_rank(verdict))
        verdict = level;
    if (!msg.empty())
        message = message.empty() ? msg : message + "; " + msg;
}

static const char *const s_role_keys[] = {
    "none", "inner_wall", "outer_wall", "overhang_wall", "sparse_infill", "internal_solid_infill", "top_surface",
    "bottom_surface", "ironing", "bridge", "internal_bridge", "gap_fill", "skirt", "brim", "support", "support_interface",
    "support_transition", "prime_tower", "custom", "mixed"};
static_assert(sizeof(s_role_keys) / sizeof(s_role_keys[0]) == size_t(erCount), "role key table out of date");

const char *role_key(ExtrusionRole role) { return size_t(role) < size_t(erCount) ? s_role_keys[role] : "none"; }

ExtrusionRole role_from_key(const std::string &key)
{
    for (size_t i = 0; i < size_t(erCount); ++i)
        if (key == s_role_keys[i])
            return ExtrusionRole(i);
    return erNone;
}

bool is_support_role(ExtrusionRole r) { return r == erSupportMaterial || r == erSupportMaterialInterface || r == erSupportTransition; }
bool is_bridge_role(ExtrusionRole r) { return r == erBridgeInfill || r == erInternalBridgeInfill; }

double Limits::get(const std::vector<double> &v, int filament, double dflt) const
{
    if (v.empty() || filament < 0)
        return dflt;
    return size_t(filament) < v.size() ? v[filament] : v.front();
}

// ------------------------------------------------------------------------------------------------
// MoveAnalysis
// ------------------------------------------------------------------------------------------------

namespace {
struct RoleAcc
{
    double time = 0., length = 0., volume = 0., filament = 0., weight = 0., cost = 0., cmd_time = 0., max_speed = 0., max_flow = 0.;
    size_t moves = 0;
};
} // namespace

MoveAnalysis::MoveAnalysis(const GCodeProcessorResult &result, const Print *print, const Options &options)
    : m_result(&result), m_options(options)
{
    const auto  &moves = result.moves;
    const size_t mode  = (options.time_mode == 1) ? 1 : 0;
    Summary     &s     = m_summary;

    s.time_s          = result.print_statistics.modes[0].time;
    s.silent_time_s   = result.print_statistics.modes[1].time;
    s.has_silent_mode = s.silent_time_s > 0.;
    s.prepare_time_s  = result.print_statistics.modes[mode].prepare_time;
    s.first_layer_time_s = result.initial_layer_time;
    s.travel_distance_mm = result.print_statistics.total_travel_distance;
    s.travel_moves       = result.print_statistics.total_travel_moves;
    s.filament_changes   = result.print_statistics.total_filament_changes;
    s.extruder_changes   = result.print_statistics.total_extruder_changes;
    s.tool_change_time_s = result.print_statistics.total_tool_change_time;
    s.move_count         = moves.size();
    s.plate_id           = result.id;
    for (const auto &w : result.warnings)
        s.warnings.push_back(w.msg);

    // Start and end of the printing part: the first and the last extrusion move of a non-custom feature.
    size_t first_print = moves.size(), last_extrusion = 0;
    for (size_t i = 0; i < moves.size(); ++i)
        if (!moves[i].internal_only && moves[i].type == EMoveType::Extrude && moves[i].extrusion_role != erCustom) {
            first_print = i;
            break;
        }
    for (size_t i = moves.size(); i > 0; --i)
        if (!moves[i - 1].internal_only && moves[i - 1].type == EMoveType::Extrude && moves[i - 1].extrusion_role != erCustom) {
            last_extrusion = i - 1;
            break;
        }
    // Custom-role moves directly before the first print move belong to the start G-code.
    size_t start_end = 0;
    while (start_end < first_print && moves[start_end].extrusion_role == erCustom)
        ++start_end;
    if (first_print == moves.size())
        start_end = 0;

    // Layers.
    unsigned max_layer = 0;
    for (const auto &m : moves)
        if (!m.internal_only)
            max_layer = std::max(max_layer, m.layer_id);
    std::vector<std::array<RoleAcc, erCount>> layer_roles(moves.empty() ? 0 : max_layer + 1);
    std::vector<std::map<int, std::pair<double, double>>> layer_fils(layer_roles.size());
    std::vector<ModeHistogram> layer_heights(layer_roles.size());
    m_layers.assign(layer_roles.size(), LayerRow());
    std::vector<bool> layer_seen(layer_roles.size(), false), layer_extruded(layer_roles.size(), false);
    for (size_t l = 0; l < m_layers.size(); ++l) {
        m_layers[l].index      = int(l);
        m_layers[l].first_move = moves.size();
        m_layers[l].fan_min = m_layers[l].temp_min = std::numeric_limits<double>::max();
    }

    // Time stored on Seam markers belongs to the next real move.
    for (size_t i = 0; i < moves.size(); ++i) {
        if (moves[i].type != EMoveType::Seam || moves[i].time[mode] == 0.f)
            continue;
        size_t j = i + 1;
        while (j < moves.size() && (moves[j].internal_only || moves[j].type == EMoveType::Seam))
            ++j;
        if (j < moves.size())
            m_carried_time[j] += moves[i].time[mode];
    }

    // Object attribution.
    bool has_labels = false;
    for (const auto &mv : moves)
        if (mv.object_label_id >= 0) { has_labels = true; break; }
    if (has_labels)
        m_object_source = "gcode_labels";
    else if (print != nullptr) {
        struct InstanceBox { Box2 box; int label; };
        // Nearest instance box within 1 mm (0 inside); boxes of separate objects do not overlap unless the objects do.
        std::vector<InstanceBox> boxes;
        for (const PrintObject *po : print->objects()) {
            BoundingBox bb;
            for (const Layer *layer : po->layers())
                bb.merge(get_extents(layer->lslices));
            for (const SupportLayer *layer : po->support_layers())
                bb.merge(get_extents(layer->support_fills.polygons_covered_by_width()));
            for (const PrintInstance &inst : po->instances())
                if (inst.model_instance)
                    boxes.push_back({to_box(bb, inst.shift), int(inst.model_instance->get_labeled_id())});
        }
        m_move_object.assign(moves.size(), -1);
        for (size_t i = 0; i < moves.size(); ++i) {
            const double x = moves[i].position.x(), y = moves[i].position.y();
            double       best = 1.;
            for (const InstanceBox &ib : boxes) {
                const double dx = std::max({ib.box.min_x - x, 0., x - ib.box.max_x});
                const double dy = std::max({ib.box.min_y - y, 0., y - ib.box.max_y});
                const double d  = std::sqrt(dx * dx + dy * dy);
                if (d <= best) {
                    best             = d;
                    m_move_object[i] = ib.label;
                    if (d == 0.)
                        break;
                }
            }
        }
        m_object_source = "print_footprint";
    }

    std::array<RoleAcc, erCount>   roles;
    std::map<int, double>          moves_mm3_per_filament, support_mm3_per_filament, tower_mm3_per_filament;
    std::map<int, ObjectStats>     objects;
    int                            current_filament = -1;
    bool                           prev_outer_wall  = false;
    std::vector<Seam>              run_seams;

    for (size_t i = 0; i < moves.size(); ++i) {
        const auto &m = moves[i];
        s.moves_time_s += m.time[mode];
    }

    for_each_move(result, [&](size_t i, float fx, float fy, float fz, const GCodeProcessorResult::MoveVertex &m) {
        const int    obj = this->object_at(i);
        const double t   = this->move_time(i);
        const double len = std::sqrt(sq(m.position.x() - fx) + sq(m.position.y() - fy) + sq(m.position.z() - fz));
        const bool   in_start = i < start_end;
        const bool   in_end   = i > last_extrusion && first_print < moves.size();
        if (in_start) s.start_gcode_time_s += t;
        if (in_end) s.end_gcode_time_s += t;
        const unsigned l = m.layer_id;
        LayerRow &row = m_layers[l];
        if (!in_start && !in_end) {
            row.time_s += t;
            layer_seen[l] = true;
            row.first_move = std::min(row.first_move, i);
            row.last_move  = std::max(row.last_move, i);
        }
        const bool outer_wall = m.type == EMoveType::Extrude && m.extrusion_role == erExternalPerimeter;
        if (outer_wall && !prev_outer_wall)
            run_seams.push_back({int(l), fx, fy, m.position.z(), obj});
        prev_outer_wall = outer_wall;

        switch (m.type) {
        case EMoveType::Extrude: {
            const int    f    = m.extruder_id;
            const double vol  = double(m.delta_extruder) * filament_area(result, f);
            const double flow = m.volumetric_rate();
            const ExtrusionRole r = m.extrusion_role;
            current_filament = f;
            RoleAcc &ra = roles[r];
            ra.time += t;
            ra.length += len;
            ra.volume += vol;
            ra.filament += m.delta_extruder;
            ra.weight += vol * filament_density(result, f) * 0.001;
            ra.cost += vol * filament_density(result, f) * 1e-6 * filament_cost(result, f);
            ra.moves++;
            if (m.feedrate > 0.f) {
                ra.cmd_time += len / m.feedrate;
                ra.max_speed = std::max(ra.max_speed, double(m.feedrate));
            }
            ra.max_flow = std::max(ra.max_flow, flow);
            s.extrusion_time_s += t;
            moves_mm3_per_filament[f] += vol;
            if (is_support_role(r))
                support_mm3_per_filament[f] += vol;
            else if (r == erWipeTower)
                tower_mm3_per_filament[f] += vol;
            s.moves_volume_mm3 += vol;
            if (r == erCustom)
                break;
            // Layer.
            layer_extruded[l] = true;
            RoleAcc &lr = layer_roles[l][r];
            lr.time += t;
            lr.length += len;
            lr.volume += vol;
            if (m.feedrate > 0.f) {
                lr.cmd_time += len / m.feedrate;
                lr.max_speed = std::max(lr.max_speed, double(m.feedrate));
            }
            auto &lf = layer_fils[l][f];
            lf.first += len;
            lf.second += vol;
            row.length_mm += len;
            row.volume_mm3 += vol;
            row.z = std::max(row.z, double(m.position.z()));
            layer_heights[l].add(m.height, len);
            if (flow > row.max_flow) {
                row.max_flow    = flow;
                row.max_flow_at = {int(l), m.position.z(), m.position.x(), m.position.y(), obj, role_key(r)};
            }
            row.max_speed = std::max(row.max_speed, double(m.feedrate));
            row.fan_min   = std::min(row.fan_min, double(m.fan_speed));
            row.fan_max   = std::max(row.fan_max, double(m.fan_speed));
            row.temp_min  = std::min(row.temp_min, double(m.temperature));
            row.temp_max  = std::max(row.temp_max, double(m.temperature));
            if (is_support_role(r)) row.has_support = true;
            if (is_bridge_role(r)) row.has_bridge = true;
            if (r == erOverhangPerimeter) row.has_overhang_wall = true;
            if (r == erIroning) row.has_ironing = true;
            if (r == erGapFill) row.has_gap_fill = true;
            if (r == erWipeTower)
                s.wipe_tower_box.merge(m.position.x(), m.position.y());
            s.max_z = std::max(s.max_z, double(m.position.z()));
            // Object.
            ObjectStats &o = objects[obj];
            o.label_id = obj;
            o.volume_mm3 += vol;
            if (is_support_role(r))
                o.support_mm3 += vol;
            o.box.merge(m.position.x(), m.position.y());
            if (o.first_layer < 0) {
                o.first_layer = o.last_layer = int(l);
                o.min_z = o.max_z = m.position.z();
            }
            o.first_layer = std::min(o.first_layer, int(l));
            o.last_layer  = std::max(o.last_layer, int(l));
            o.min_z       = std::min(o.min_z, double(m.position.z()));
            o.max_z       = std::max(o.max_z, double(m.position.z()));
            break;
        }
        case EMoveType::Travel:
            s.travel_time_s += t;
            break;
        case EMoveType::Retract:
            s.retractions++;
            s.other_time_s += t;
            break;
        case EMoveType::Tool_change: {
            s.tool_changes++;
            s.other_time_s += t;
            const int to = m.extruder_id;
            if (current_filament >= 0 && to != current_filament) {
                m_tool_changes.push_back({int(l), double(m.position.z()), current_filament, to, i});
                row.tool_changes++;
            } else if (current_filament < 0) {
                m_tool_changes.push_back({int(l), double(m.position.z()), -1, to, i});
            }
            current_filament = to;
            break;
        }
        default:
            s.other_time_s += t;
            break;
        }
        if (obj >= 0 || m.type == EMoveType::Extrude)
            objects[obj].time_s += t;
    });

    // Seams.
    for (size_t i = 0; i < moves.size(); ++i)
        if (moves[i].type == EMoveType::Seam)
            m_seams.push_back({int(moves[i].layer_id), moves[i].position.x(), moves[i].position.y(), moves[i].position.z(), object_at(i)});
    if (m_seams.empty() && !run_seams.empty()) {
        m_seams            = std::move(run_seams);
        m_seams_from_moves = false;
    }
    s.seams = unsigned(m_seams.size());
    if (s.wipe_tower_box.defined()) {
        s.wipe_tower_x = s.wipe_tower_box.center_x();
        s.wipe_tower_y = s.wipe_tower_box.center_y();
    }

    // Roles.
    for (size_t r = 0; r < size_t(erCount); ++r) {
        const RoleAcc &ra = roles[r];
        const auto     sit = result.print_statistics.used_filaments_per_role.find(ExtrusionRole(r));
        if (ra.moves == 0 && sit == result.print_statistics.used_filaments_per_role.end())
            continue;
        RoleStats rs;
        rs.role        = role_key(ExtrusionRole(r));
        rs.time_s      = ra.time;
        rs.length_mm   = ra.length;
        rs.volume_mm3  = ra.volume;
        rs.filament_mm = ra.filament;
        rs.weight_g    = ra.weight;
        rs.cost        = ra.cost;
        rs.moves       = ra.moves;
        rs.max_speed   = ra.max_speed;
        rs.mean_speed  = ra.cmd_time > 0. ? ra.length / ra.cmd_time : 0.;
        rs.max_flow    = ra.max_flow;
        rs.mean_flow   = ra.cmd_time > 0. ? ra.volume / ra.cmd_time : 0.;
        if (sit != result.print_statistics.used_filaments_per_role.end()) {
            rs.slicer_filament_m = sit->second.first;
            rs.slicer_weight_g   = sit->second.second;
        }
        s.roles.push_back(rs);
    }
    std::sort(s.roles.begin(), s.roles.end(), [](const RoleStats &a, const RoleStats &b) { return a.time_s > b.time_s; });

    // Filaments.
    std::set<int> fils;
    const auto &ps = result.print_statistics;
    for (const auto &kv : ps.total_volumes_per_extruder) fils.insert(int(kv.first));
    for (const auto &kv : ps.model_volumes_per_extruder) fils.insert(int(kv.first));
    for (const auto &kv : moves_mm3_per_filament) fils.insert(kv.first);
    auto get = [](const std::map<size_t, double> &m, int f) { auto it = m.find(size_t(f)); return it == m.end() ? 0. : it->second; };
    for (int f : fils) {
        FilamentStats fs;
        fs.filament       = f;
        fs.model_mm3      = get(ps.model_volumes_per_extruder, f);
        fs.support_mm3    = get(ps.support_volumes_per_extruder, f);
        fs.wipe_tower_mm3 = get(ps.wipe_tower_volumes_per_extruder, f);
        fs.flush_mm3      = get(ps.flush_per_filament, f);
        fs.total_mm3      = get(ps.total_volumes_per_extruder, f);
        fs.diameter_mm    = (size_t(f) < result.filament_diameters.size()) ? result.filament_diameters[f] : 0.;
        fs.density        = filament_density(result, f);
        fs.cost_per_kg    = filament_cost(result, f);
        fs.length_mm      = fs.total_mm3 / filament_area(result, f);
        fs.weight_g       = fs.total_mm3 * fs.density * 0.001;
        fs.cost           = fs.weight_g * 0.001 * fs.cost_per_kg;
        auto it           = moves_mm3_per_filament.find(f);
        fs.moves_mm3      = it == moves_mm3_per_filament.end() ? 0. : it->second;
        fs.moves_support_mm3 = support_mm3_per_filament.count(f) ? support_mm3_per_filament[f] : 0.;
        fs.moves_tower_mm3   = tower_mm3_per_filament.count(f) ? tower_mm3_per_filament[f] : 0.;
        s.total_mm3 += fs.total_mm3;
        s.model_mm3 += fs.model_mm3;
        s.support_mm3 += fs.support_mm3;
        s.wipe_tower_mm3 += fs.wipe_tower_mm3;
        s.flush_mm3 += fs.flush_mm3;
        s.total_length_mm += fs.length_mm;
        s.total_weight_g += fs.weight_g;
        s.total_cost += fs.cost;
        s.filaments.push_back(fs);
    }

    // Layers.
    ModeHistogram heights;
    double        min_h = std::numeric_limits<double>::max(), max_h = 0.;
    for (size_t l = 0; l < m_layers.size(); ++l) {
        LayerRow &row = m_layers[l];
        if (!layer_seen[l])
            row.first_move = row.last_move = 0;
        if (row.fan_min > row.fan_max) row.fan_min = row.fan_max = 0.;
        if (row.temp_min > row.temp_max) row.temp_min = row.temp_max = 0.;
        row.height = layer_heights[l].mode();
        double cmd_time = 0.;
        for (size_t r = 0; r < size_t(erCount); ++r) {
            const RoleAcc &lr = layer_roles[l][r];
            if (lr.length <= 0. && lr.volume <= 0.)
                continue;
            LayerRoleStats ls;
            ls.role       = role_key(ExtrusionRole(r));
            ls.length_mm  = lr.length;
            ls.volume_mm3 = lr.volume;
            ls.time_s     = lr.time;
            ls.max_speed  = lr.max_speed;
            ls.mean_speed = lr.cmd_time > 0. ? lr.length / lr.cmd_time : 0.;
            cmd_time += lr.cmd_time;
            row.roles.push_back(ls);
        }
        row.mean_flow = cmd_time > 0. ? row.volume_mm3 / cmd_time : 0.;
        for (const auto &kv : layer_fils[l])
            row.filaments.push_back({kv.first, kv.second.first, kv.second.second});
        if (layer_extruded[l]) {
            ++s.layer_count;
            if (l == 0 || s.layer_count == 1)
                s.first_layer_height = row.height;
            else
                heights.add(row.height, 1.);
            min_h = std::min(min_h, row.height);
            max_h = std::max(max_h, row.height);
        }
    }
    s.layer_height     = heights.weights.empty() ? s.first_layer_height : heights.mode();
    s.min_layer_height = s.layer_count ? min_h : 0.;
    s.max_layer_height = max_h;

    // Objects with names.
    std::map<int, std::pair<std::string, int>> names;
    if (print != nullptr)
        for (const PrintObject *po : print->objects())
            for (size_t k = 0; k < po->instances().size(); ++k)
                if (po->instances()[k].model_instance)
                    names[int(po->instances()[k].model_instance->get_labeled_id())] = {object_name(*po), int(k)};
    for (auto &kv : objects) {
        ObjectStats o = kv.second;
        o.label_id    = kv.first;
        auto nit      = names.find(kv.first);
        if (nit != names.end()) {
            o.name     = nit->second.first;
            o.instance = nit->second.second;
            for (const auto &om : result.object_masses)
                if (om.name == o.name && om.instance == o.instance)
                    o.mass_g = om.total().mass;
            for (const auto &om : result.support_masses)
                if (om.name == o.name && om.instance == o.instance)
                    o.support_mass_g = om.total().mass;
        }
        s.objects.push_back(o);
    }
}

double MoveAnalysis::move_time(size_t i) const
{
    if (m_result == nullptr || i >= m_result->moves.size())
        return 0.;
    const auto &m = m_result->moves[i];
    if (m.type == EMoveType::Seam)
        return 0.;
    double t = m.time[m_options.time_mode == 1 ? 1 : 0];
    if (!m_carried_time.empty()) {
        auto it = m_carried_time.find(i);
        if (it != m_carried_time.end())
            t += it->second;
    }
    return t;
}

int MoveAnalysis::object_at(size_t i) const
{
    if (!m_move_object.empty())
        return i < m_move_object.size() ? m_move_object[i] : -1;
    return (m_result != nullptr && i < m_result->moves.size()) ? m_result->moves[i].object_label_id : -1;
}

std::vector<LayerRow> MoveAnalysis::layer_page(size_t from, size_t count, size_t every) const
{
    std::vector<LayerRow> out;
    every = std::max<size_t>(1, every);
    for (size_t i = from; i < m_layers.size() && out.size() < count; i += every)
        out.push_back(m_layers[i]);
    return out;
}

int MoveAnalysis::layer_at_z(double z) const
{
    int    best = -1;
    double bd   = std::numeric_limits<double>::max();
    for (const LayerRow &r : m_layers) {
        if (r.length_mm <= 0.)
            continue;
        const double d = std::abs(r.z - z);
        if (d < bd) { bd = d; best = r.index; }
    }
    return best;
}

// ------------------------------------------------------------------------------------------------
// Limits
// ------------------------------------------------------------------------------------------------

Limits limits_from_config(const DynamicPrintConfig &c)
{
    Limits l;
    l.max_volumetric_speed        = cfg_vector(c, "filament_max_volumetric_speed");
    l.slow_down_layer_time        = cfg_vector(c, "slow_down_layer_time");
    l.slow_down_min_speed         = cfg_vector(c, "slow_down_min_speed");
    l.fan_cooling_layer_time      = cfg_vector(c, "fan_cooling_layer_time");
    if (auto o = c.option<ConfigOptionBools>("slow_down_for_layer_cooling"))
        for (unsigned char b : o->values) l.slow_down_for_layer_cooling.push_back(b != 0);
    const double outer = cfg_value(c, "outer_wall_speed");
    const double inner = cfg_value(c, "inner_wall_speed");
    const double sparse = cfg_value(c, "sparse_infill_speed");
    const double solid  = cfg_value(c, "internal_solid_infill_speed");
    const double bridge = cfg_value(c, "bridge_speed");
    l.role_speed = {
        {"outer_wall", outer},
        {"inner_wall", inner},
        {"overhang_wall", std::max(outer, inner)},
        {"sparse_infill", sparse},
        {"internal_solid_infill", solid},
        {"top_surface", cfg_value(c, "top_surface_speed")},
        {"bottom_surface", solid},
        {"bridge", bridge},
        {"internal_bridge", cfg_value(c, "internal_bridge_speed", 0, bridge)},
        {"gap_fill", cfg_value(c, "gap_infill_speed")},
        {"support", cfg_value(c, "support_speed")},
        {"support_interface", cfg_value(c, "support_interface_speed")},
        {"ironing", cfg_value(c, "ironing_speed")},
        {"skirt", cfg_value(c, "skirt_speed")},
    };
    l.default_acceleration = cfg_value(c, "default_acceleration");
    const double outer_acc = cfg_value(c, "outer_wall_acceleration");
    const double inner_acc = cfg_value(c, "inner_wall_acceleration");
    l.role_acceleration = {
        {"outer_wall", outer_acc},
        {"inner_wall", inner_acc},
        {"overhang_wall", std::max(outer_acc, inner_acc)},
        {"sparse_infill", cfg_value(c, "sparse_infill_acceleration", 0, l.default_acceleration)},
        {"top_surface", cfg_value(c, "top_surface_acceleration")},
    };
    l.initial_layer_speed        = cfg_value(c, "initial_layer_speed");
    l.initial_layer_acceleration = cfg_value(c, "initial_layer_acceleration");
    l.travel_speed               = cfg_value(c, "travel_speed");
    l.travel_acceleration        = cfg_value(c, "travel_acceleration");
    l.machine_max_speed_x        = cfg_value(c, "machine_max_speed_x");
    l.machine_max_speed_y        = cfg_value(c, "machine_max_speed_y");
    l.machine_max_acceleration_extruding = cfg_value(c, "machine_max_acceleration_extruding");
    l.machine_max_acceleration_travel    = cfg_value(c, "machine_max_acceleration_travel");
    if (const double n = cfg_value(c, "nozzle_diameter"); n > 0.)
        l.nozzle_diameter = n;
    l.support_top_z_distance = cfg_value(c, "support_top_z_distance");
    return l;
}

Limits limits_from_print(const Print &print) { return limits_from_config(print.full_print_config()); }

// ------------------------------------------------------------------------------------------------
// Flow
// ------------------------------------------------------------------------------------------------

FlowReport check_flow(const MoveAnalysis &analysis, const Limits &limits, double tolerance, size_t max_locations)
{
    FlowReport rep;
    rep.tolerance         = tolerance;
    rep.verdict.source    = "G-code moves: commanded feed rate x mm3/mm (MoveVertex::volumetric_rate) per extrusion move, vs filament_max_volumetric_speed";
    rep.verdict.exactness = "exact for the commanded rate of each G-code move; mm3/mm is measured from E, so filament flow ratio scales it; "
                            "actual (planner cruise) rate is <= commanded";
    const GCodeProcessorResult *res = analysis.result();
    if (res == nullptr) {
        rep.verdict.verdict = "not_applicable";
        return rep;
    }
    struct Acc
    {
        FlowFilament ff;
        double       time = 0.;
        std::map<int, std::pair<double, Location>> per_layer_max;
        std::set<int> layers;
        std::map<std::string, double> by_role;
    };
    std::map<int, Acc> acc;
    for_each_move(*res, [&](size_t i, float fx, float fy, float fz, const GCodeProcessorResult::MoveVertex &m) {
        if (m.type != EMoveType::Extrude || m.extrusion_role == erCustom)
            return;
        const int    f    = m.extruder_id;
        Acc         &a    = acc[f];
        const double flow = m.volumetric_rate();
        const double len  = std::sqrt(sq(m.position.x() - fx) + sq(m.position.y() - fy) + sq(m.position.z() - fz));
        const double t    = analysis.move_time(i);
        a.time += t;
        const Location loc{int(m.layer_id), m.position.z(), m.position.x(), m.position.y(), analysis.object_at(i), role_key(m.extrusion_role)};
        if (flow > a.ff.max_flow) {
            a.ff.max_flow = flow;
            a.ff.max_at   = loc;
        }
        a.ff.max_actual_flow = std::max(a.ff.max_actual_flow, double(m.actual_volumetric_rate()));
        auto &pl = a.per_layer_max[int(m.layer_id)];
        if (flow > pl.first)
            pl = {flow, loc};
        const double limit = limits.get(limits.max_volumetric_speed, f);
        if (limit > 0.) {
            if (flow >= (1. - tolerance) * limit) {
                a.ff.at_limit_length_mm += len;
                a.ff.at_limit_time_s += t;
                a.layers.insert(int(m.layer_id));
                a.by_role[role_key(m.extrusion_role)] += len;
            }
            if (flow > (1. + tolerance) * limit) {
                a.ff.over_limit_length_mm += len;
                a.ff.over_limit_time_s += t;
            }
        }
    });
    for (auto &kv : acc) {
        Acc &a        = kv.second;
        FlowFilament &ff = a.ff;
        ff.filament   = kv.first;
        ff.limit      = limits.get(limits.max_volumetric_speed, kv.first);
        ff.ratio      = ff.limit > 0. ? ff.max_flow / ff.limit : 0.;
        ff.share_at_limit = a.time > 0. ? ff.at_limit_time_s / a.time : 0.;
        ff.layers_at_limit = int(a.layers.size());
        if (!a.layers.empty()) {
            ff.first_layer_at_limit = *a.layers.begin();
            ff.last_layer_at_limit  = *a.layers.rbegin();
        }
        for (const auto &r : a.by_role)
            ff.at_limit_length_by_role.emplace_back(r.first, r.second);
        std::vector<std::pair<double, Location>> layers_max;
        for (const auto &pl : a.per_layer_max)
            layers_max.push_back(pl.second);
        std::sort(layers_max.begin(), layers_max.end(), [](const auto &x, const auto &y) { return x.first > y.first; });
        for (size_t i = 0; i < layers_max.size() && i < max_locations; ++i)
            ff.worst.push_back(layers_max[i].second);
        if (ff.limit <= 0.)
            rep.verdict.raise("ok", "filament " + std::to_string(ff.filament + 1) + ": no max volumetric speed configured");
        else if (ff.over_limit_length_mm > 0.)
            rep.verdict.raise("problem", "filament " + std::to_string(ff.filament + 1) + " exceeds its max volumetric speed (" +
                                             std::to_string(ff.max_flow) + " > " + std::to_string(ff.limit) + " mm3/s)");
        else if (ff.at_limit_length_mm > 0.)
            rep.verdict.raise("warning", "filament " + std::to_string(ff.filament + 1) + " is limited by its max volumetric speed on " +
                                             std::to_string(ff.layers_at_limit) + " layers (" + std::to_string(int(std::lround(ff.share_at_limit * 100.))) +
                                             "% of its extrusion time)");
        rep.filaments.push_back(ff);
    }
    if (rep.verdict.message.empty())
        rep.verdict.message = "flow below every filament's limit";
    return rep;
}

// ------------------------------------------------------------------------------------------------
// Speeds
// ------------------------------------------------------------------------------------------------

SpeedReport check_speeds(const MoveAnalysis &analysis, const Limits &limits, double tolerance)
{
    SpeedReport rep;
    rep.verdict.source    = "G-code moves: commanded feed rate (F), planner cruise speed, acceleration in effect; configured speeds from the global config";
    rep.verdict.exactness = "exact per move; configured values are global (per-object/modifier overrides and per-nozzle variants other than the "
                            "first are not considered), overhang walls are compared with max(outer, inner wall speed)";
    rep.initial_layer_speed       = limits.initial_layer_speed;
    rep.travel_speed              = limits.travel_speed;
    rep.machine_max_speed_x       = limits.machine_max_speed_x;
    rep.machine_max_speed_y       = limits.machine_max_speed_y;
    rep.machine_max_acceleration  = limits.machine_max_acceleration_extruding;
    const GCodeProcessorResult *res = analysis.result();
    if (res == nullptr) {
        rep.verdict.verdict = "not_applicable";
        return rep;
    }
    struct Acc
    {
        SpeedRole sr;
        double    length = 0., cmd_time = 0.;
        bool      used = false;
    };
    std::array<Acc, erCount> acc;
    for_each_move(*res, [&](size_t i, float fx, float fy, float fz, const GCodeProcessorResult::MoveVertex &m) {
        const double dx = m.position.x() - fx, dy = m.position.y() - fy;
        const double len = std::sqrt(sq(dx) + sq(dy) + sq(m.position.z() - fz));
        if (len > 1e-6 && m.feedrate > 0.f) {
            rep.max_speed_x = std::max(rep.max_speed_x, std::abs(dx) / len * m.feedrate);
            rep.max_speed_y = std::max(rep.max_speed_y, std::abs(dy) / len * m.feedrate);
        }
        if (m.type == EMoveType::Travel) {
            rep.travel_max_speed = std::max(rep.travel_max_speed, double(m.feedrate));
            return;
        }
        if (m.type != EMoveType::Extrude || m.extrusion_role == erCustom)
            return;
        rep.max_acceleration = std::max(rep.max_acceleration, double(m.acceleration));
        if (m.layer_id == 0)
            rep.first_layer_max_speed = std::max(rep.first_layer_max_speed, double(m.feedrate));
        Acc &a = acc[m.extrusion_role];
        a.used = true;
        a.length += len;
        if (m.feedrate > 0.f)
            a.cmd_time += len / m.feedrate;
        if (m.feedrate > a.sr.max_speed) {
            a.sr.max_speed    = m.feedrate;
            a.sr.max_speed_at = {int(m.layer_id), m.position.z(), m.position.x(), m.position.y(), analysis.object_at(i), role_key(m.extrusion_role)};
        }
        a.sr.max_actual_speed = std::max(a.sr.max_actual_speed, double(m.actual_feedrate));
        a.sr.max_acceleration = std::max(a.sr.max_acceleration, double(m.acceleration));
        auto it = limits.role_speed.find(role_key(m.extrusion_role));
        if (it != limits.role_speed.end() && it->second > 0.) {
            if (m.layer_id > 0 && m.feedrate > it->second * (1. + tolerance) + 0.01)
                a.sr.over_configured_length_mm += len;
            if (m.feedrate < 0.5 * it->second)
                a.sr.below_half_length_mm += len;
        }
    });
    for (size_t r = 0; r < size_t(erCount); ++r) {
        Acc &a = acc[r];
        if (!a.used)
            continue;
        a.sr.role       = role_key(ExtrusionRole(r));
        a.sr.mean_speed = a.cmd_time > 0. ? a.length / a.cmd_time : 0.;
        auto it         = limits.role_speed.find(a.sr.role);
        a.sr.configured_speed = it == limits.role_speed.end() ? 0. : it->second;
        auto ia         = limits.role_acceleration.find(a.sr.role);
        a.sr.configured_acceleration = ia == limits.role_acceleration.end() ? limits.default_acceleration : ia->second;
        if (a.sr.over_configured_length_mm > 0.)
            rep.verdict.raise("warning", a.sr.role + " printed faster than configured over " + std::to_string(a.sr.over_configured_length_mm) + " mm");
        rep.roles.push_back(a.sr);
    }
    if (limits.machine_max_acceleration_extruding > 0. && rep.max_acceleration > limits.machine_max_acceleration_extruding * (1. + tolerance))
        rep.verdict.raise("warning", "acceleration above the machine limit");
    if (limits.machine_max_speed_x > 0. && rep.max_speed_x > limits.machine_max_speed_x * (1. + tolerance))
        rep.verdict.raise("warning", "X speed above the machine limit (the firmware will cap it)");
    if (limits.machine_max_speed_y > 0. && rep.max_speed_y > limits.machine_max_speed_y * (1. + tolerance))
        rep.verdict.raise("warning", "Y speed above the machine limit (the firmware will cap it)");
    if (rep.verdict.message.empty())
        rep.verdict.message = "speeds within the configured values";
    return rep;
}

// ------------------------------------------------------------------------------------------------
// Cooling
// ------------------------------------------------------------------------------------------------

CoolingReport check_cooling(const MoveAnalysis &analysis, const Limits &limits)
{
    CoolingReport rep;
    rep.verdict.source    = "per-layer sum of the G-code processor's move times vs slow_down_layer_time / fan_cooling_layer_time of the "
                            "filaments printed on the layer";
    rep.verdict.exactness = "layer time is the processor's estimate (acceleration aware), which is what the time display shows; the cooling "
                            "slow-down itself works on the G-code generator's own simpler estimate, so a layer within a few % of the "
                            "threshold may be reported either way";
    const GCodeProcessorResult *res = analysis.result();
    if (res == nullptr) {
        rep.verdict.verdict = "not_applicable";
        return rep;
    }
    std::vector<double> min_speed(analysis.layers().size(), std::numeric_limits<double>::max());
    for (const LayerRow &row : analysis.layers()) {
        if (row.length_mm <= 0.)
            continue;
        for (size_t i = row.first_move; i <= row.last_move && i < res->moves.size(); ++i) {
            const auto &m = res->moves[i];
            if (m.type == EMoveType::Extrude && !m.internal_only && m.extrusion_role != erWipeTower && m.extrusion_role != erCustom && m.feedrate > 0.f)
                min_speed[row.index] = std::min(min_speed[row.index], double(m.feedrate));
        }
    }
    double min_time = std::numeric_limits<double>::max();
    for (const LayerRow &row : analysis.layers()) {
        if (row.length_mm <= 0.)
            continue;
        if (row.time_s < min_time) {
            min_time                 = row.time_s;
            rep.min_layer_time_layer = row.index;
        }
        CoolingLayer cl;
        cl.layer  = row.index;
        cl.z      = row.z;
        cl.time_s = row.time_s;
        cl.fan_max = row.fan_max;
        cl.min_speed = min_speed[row.index] == std::numeric_limits<double>::max() ? 0. : min_speed[row.index];
        for (const LayerFilamentStats &f : row.filaments) {
            const bool enabled = limits.slow_down_for_layer_cooling.empty() ||
                                 (size_t(f.filament) < limits.slow_down_for_layer_cooling.size() ? limits.slow_down_for_layer_cooling[f.filament] :
                                                                                                   limits.slow_down_for_layer_cooling.front());
            if (enabled)
                cl.slow_down_layer_time = std::max(cl.slow_down_layer_time, limits.get(limits.slow_down_layer_time, f.filament));
            cl.fan_cooling_layer_time = std::max(cl.fan_cooling_layer_time, limits.get(limits.fan_cooling_layer_time, f.filament));
            cl.slow_down_min_speed    = std::max(cl.slow_down_min_speed, limits.get(limits.slow_down_min_speed, f.filament));
        }
        cl.below_slow_down_time = cl.slow_down_layer_time > 0. && row.time_s < cl.slow_down_layer_time;
        cl.at_min_speed         = cl.slow_down_min_speed > 0. && cl.min_speed <= cl.slow_down_min_speed + 0.5;
        const bool below_fan    = cl.fan_cooling_layer_time > 0. && row.time_s < cl.fan_cooling_layer_time;
        if (cl.below_slow_down_time)
            ++rep.layers_below_slow_down_time;
        if (below_fan)
            ++rep.layers_below_fan_time;
        if (cl.below_slow_down_time || below_fan)
            rep.layers.push_back(cl);
    }
    rep.min_layer_time_s = rep.min_layer_time_layer >= 0 ? min_time : 0.;
    if (rep.layers_below_slow_down_time > 0)
        rep.verdict.raise("warning", std::to_string(rep.layers_below_slow_down_time) +
                                         " layers print faster than the minimum layer time even after the slow-down (shortest " +
                                         std::to_string(rep.min_layer_time_s) + " s at layer " + std::to_string(rep.min_layer_time_layer) + ")");
    else
        rep.verdict.message = "every layer takes at least the minimum layer time";
    return rep;
}

// ------------------------------------------------------------------------------------------------
// Supports
// ------------------------------------------------------------------------------------------------

namespace {

// Visit extrusion moves of the given roles with their clipped length/volume inside a 3D box.
template<class Pred>
BoxQuery box_query(const MoveAnalysis &analysis, const Box3 &box, Pred &&role_ok)
{
    BoxQuery q;
    const GCodeProcessorResult *res = analysis.result();
    if (res == nullptr)
        return q;
    std::set<int> layers;
    std::map<std::string, double> by_role;
    std::map<int, double> by_fil;
    for_each_move(*res, [&](size_t i, float fx, float fy, float fz, const GCodeProcessorResult::MoveVertex &m) {
        if (m.type != EMoveType::Extrude || !role_ok(m.extrusion_role) || !z_in(m.position.z(), box))
            return;
        double t0, t1;
        const double frac = clip_fraction(fx, fy, m.position.x(), m.position.y(), box.xy, t0, t1);
        if (frac <= 0.)
            return;
        const double len = std::sqrt(sq(m.position.x() - fx) + sq(m.position.y() - fy) + sq(m.position.z() - fz)) * frac;
        q.length_mm += len;
        q.volume_mm3 += double(m.delta_extruder) * filament_area(*res, m.extruder_id) * frac;
        q.time_s += analysis.move_time(i) * frac;
        layers.insert(int(m.layer_id));
        by_role[role_key(m.extrusion_role)] += len;
        by_fil[m.extruder_id] += len;
        q.box.merge(fx + t0 * (m.position.x() - fx), fy + t0 * (m.position.y() - fy));
        q.box.merge(fx + t1 * (m.position.x() - fx), fy + t1 * (m.position.y() - fy));
    });
    q.layers = int(layers.size());
    if (!layers.empty()) {
        q.first_layer = *layers.begin();
        q.last_layer  = *layers.rbegin();
    }
    for (const auto &kv : by_role) q.length_by_role.emplace_back(kv.first, kv.second);
    for (const auto &kv : by_fil) q.length_by_filament.emplace_back(kv.first, kv.second);
    return q;
}

std::vector<Seg> collect_segments(const MoveAnalysis &analysis, const std::function<bool(ExtrusionRole)> &pred)
{
    std::vector<Seg> segs;
    const GCodeProcessorResult *res = analysis.result();
    if (res == nullptr)
        return segs;
    for_each_move(*res, [&](size_t i, float fx, float fy, float fz, const GCodeProcessorResult::MoveVertex &m) {
        if (m.type != EMoveType::Extrude || !pred(m.extrusion_role))
            return;
        Seg s;
        s.layer = int(m.layer_id);
        s.z     = m.position.z();
        s.box.merge(fx, fy);
        s.box.merge(m.position.x(), m.position.y());
        s.length = std::sqrt(sq(m.position.x() - fx) + sq(m.position.y() - fy) + sq(m.position.z() - fz));
        s.volume = double(m.delta_extruder) * filament_area(*res, m.extruder_id);
        s.object = analysis.object_at(i);
        segs.push_back(s);
    });
    return segs;
}

} // namespace

BoxQuery support_in_box(const MoveAnalysis &analysis, const Box3 &box) { return box_query(analysis, box, [](ExtrusionRole r) { return is_support_role(r); }); }

BoxQuery extrusions_in_box(const MoveAnalysis &analysis, const Box3 &box, const std::vector<std::string> &roles)
{
    std::set<ExtrusionRole> wanted;
    for (const std::string &k : roles)
        wanted.insert(role_from_key(k));
    return box_query(analysis, box, [&wanted](ExtrusionRole r) { return r != erCustom && (wanted.empty() || wanted.count(r) > 0); });
}

SupportReport check_supports(const MoveAnalysis &analysis, const Print *print, size_t max_clusters)
{
    SupportReport rep;
    rep.verdict.source    = "G-code moves of the support, support interface and support transition features; with a Print, support layers "
                            "(covered area of support_fills) and object layers (lslices) for the contact area";
    rep.verdict.exactness = "lengths/volumes exact; contact area = model area not resting on the layer below that lies over the support "
                            "footprint (extrusion width-covered area) of the support layer(s) just below, within the top z distance";
    rep.slicer_support_mm3 = analysis.summary().support_mm3;
    const GCodeProcessorResult *res = analysis.result();
    if (res != nullptr) {
        std::map<int, SupportLayerRow> rows;
        for_each_move(*res, [&](size_t i, float fx, float fy, float fz, const GCodeProcessorResult::MoveVertex &m) {
            if (m.type != EMoveType::Extrude || !is_support_role(m.extrusion_role))
                return;
            const double len = std::sqrt(sq(m.position.x() - fx) + sq(m.position.y() - fy) + sq(m.position.z() - fz));
            const double vol = double(m.delta_extruder) * filament_area(*res, m.extruder_id);
            rep.length_mm += len;
            rep.volume_mm3 += vol;
            rep.time_s += analysis.move_time(i);
            if (m.extrusion_role == erSupportMaterialInterface)
                rep.interface_volume_mm3 += vol;
            SupportLayerRow &row = rows[int(m.layer_id)];
            row.layer = int(m.layer_id);
            row.z     = std::max(row.z, double(m.position.z()));
            row.length_mm += len;
            row.volume_mm3 += vol;
            row.box.merge(fx, fy);
            row.box.merge(m.position.x(), m.position.y());
            rep.box.merge(row.box);
        });
        for (auto &kv : rows)
            rep.layers.push_back(kv.second);
        rep.layers_with_support = int(rep.layers.size());
        if (!rep.layers.empty()) {
            rep.first_layer = rep.layers.front().layer;
            rep.last_layer  = rep.layers.back().layer;
        }
        rep.clusters = cluster_segments(collect_segments(analysis, [](ExtrusionRole r) { return is_support_role(r); }), 2., max_clusters);
    }
    if (print != nullptr) {
        for (size_t oi = 0; oi < print->objects().size(); ++oi) {
            const PrintObject *po = print->objects()[oi];
            SupportObject so;
            so.object_index = int(oi);
            so.name         = object_name(*po);
            const auto fps  = support_footprints(*po);
            for (const auto &f : fps)
                if (!f.second.empty()) {
                    ++so.support_layers;
                    so.support_area_mm2 += area_mm2(f.second);
                }
            const Point shift = po->instances().empty() ? Point(0, 0) : po->instances().front().shift;
            const double gap_extra = cfg_value(print->full_print_config(), "support_top_z_distance");
            std::vector<double> contact(po->layers().size(), 0.);
            std::vector<BoundingBox> contact_bb(po->layers().size());
            tbb::parallel_for(tbb::blocked_range<size_t>(0, po->layers().size()), [&](const tbb::blocked_range<size_t> &range) {
                for (size_t li = range.begin(); li < range.end(); ++li) {
                    const Layer *layer = po->layers()[li];
                    const double gap   = gap_extra + layer->height + 0.05;
                    ExPolygons   sup   = support_below(fps, layer->bottom_z(), gap);
                    if (sup.empty())
                        continue;
                    ExPolygons overhang = layer->lower_layer ? diff_ex(layer->lslices, layer->lower_layer->lslices) : layer->lslices;
                    ExPolygons c        = intersection_ex(overhang, sup);
                    contact[li]         = area_mm2(c);
                    if (!c.empty())
                        contact_bb[li] = get_extents(c);
                }
            });
            BoundingBox all;
            for (size_t li = 0; li < contact.size(); ++li) {
                if (contact[li] <= 1e-6)
                    continue;
                so.contact_area_mm2 += contact[li];
                if (so.first_contact_layer < 0)
                    so.first_contact_layer = int(li);
                so.last_contact_layer = int(li);
                all.merge(contact_bb[li]);
            }
            so.contact_box = to_box(all, shift);
            rep.objects.push_back(so);
        }
    }
    if (rep.volume_mm3 > 0.)
        rep.verdict.message = "support on " + std::to_string(rep.layers_with_support) + " layers, " + std::to_string(rep.volume_mm3) + " mm3";
    else
        rep.verdict.message = "no support printed";
    return rep;
}

// ------------------------------------------------------------------------------------------------
// Overhangs and bridges
// ------------------------------------------------------------------------------------------------

OverhangReport check_overhangs(const MoveAnalysis &analysis, const Print *print, size_t max_clusters)
{
    OverhangReport rep;
    rep.verdict.source    = "G-code moves of the bridge, internal bridge and overhang wall features; with a Print, every wall path sampled "
                            "every 0.5 mm against the islands of the layer below";
    rep.verdict.exactness = "lengths exact; longest bridge = longest single bridge move (bridges are straight lines between anchors); "
                            "overhang band = share of the line width outside the layer below at the sample, the same measure the "
                            "overhang speed bands (overhang_1_4..4_4_speed) use, recomputed here because the slicer does not store it";
    const GCodeProcessorResult *res = analysis.result();
    if (res != nullptr) {
        ExtrusionRole prev = erNone;
        std::set<int> bl, ol;
        for_each_move(*res, [&](size_t i, float fx, float fy, float fz, const GCodeProcessorResult::MoveVertex &m) {
            const ExtrusionRole r = m.type == EMoveType::Extrude ? m.extrusion_role : erNone;
            if (m.type != EMoveType::Extrude) {
                if (m.type == EMoveType::Travel)
                    prev = erNone;
                return;
            }
            const double len = std::sqrt(sq(m.position.x() - fx) + sq(m.position.y() - fy) + sq(m.position.z() - fz));
            const double vol = double(m.delta_extruder) * filament_area(*res, m.extruder_id);
            if (r == erBridgeInfill) {
                rep.bridge_length_mm += len;
                rep.bridge_volume_mm3 += vol;
                bl.insert(int(m.layer_id));
                if (prev != r) ++rep.bridge_runs;
                if (len > rep.longest_bridge_mm) {
                    rep.longest_bridge_mm = len;
                    rep.longest_bridge_at = {int(m.layer_id), m.position.z(), 0.5 * (fx + m.position.x()), 0.5 * (fy + m.position.y()),
                                             analysis.object_at(i), role_key(r)};
                }
            } else if (r == erInternalBridgeInfill) {
                rep.internal_bridge_length_mm += len;
            } else if (r == erOverhangPerimeter) {
                rep.overhang_wall_length_mm += len;
                rep.overhang_wall_volume_mm3 += vol;
                ol.insert(int(m.layer_id));
                if (prev != r) ++rep.overhang_wall_runs;
            }
            prev = r;
        });
        rep.layers_with_bridge        = int(bl.size());
        rep.layers_with_overhang_wall = int(ol.size());
        rep.bridge_clusters        = cluster_segments(collect_segments(analysis, [](ExtrusionRole r) { return r == erBridgeInfill; }), 2., max_clusters);
        rep.overhang_wall_clusters = cluster_segments(collect_segments(analysis, [](ExtrusionRole r) { return r == erOverhangPerimeter; }), 2., max_clusters);
    }
    if (print != nullptr) {
        static const char *band_names[] = {"0", "0-25", "25-50", "50-75", "75-100", "100"};
        std::array<double, 6>   band_len{};
        std::array<double, 6>   band_best{};
        std::array<Location, 6> band_loc;
        for (size_t oi = 0; oi < print->objects().size(); ++oi) {
            const PrintObject *po = print->objects()[oi];
            const Point shift = po->instances().empty() ? Point(0, 0) : po->instances().front().shift;
            const int   label = (po->instances().empty() || po->instances().front().model_instance == nullptr) ?
                                    -1 : int(po->instances().front().model_instance->get_labeled_id());
            struct LayerBands { std::array<double, 6> len{}; std::array<double, 6> best{}; std::array<Location, 6> loc; };
            std::vector<LayerBands> lb(po->layers().size());
            tbb::parallel_for(tbb::blocked_range<size_t>(1, std::max<size_t>(1, po->layers().size())), [&](const tbb::blocked_range<size_t> &range) {
                for (size_t li = range.begin(); li < range.end(); ++li) {
                    const Layer *layer = po->layers()[li];
                    if (layer->lower_layer == nullptr || layer->lower_layer->lslices.empty())
                        continue;
                    AABBTreeLines::LinesDistancer<Linef> lower(to_unscaled_linesf(layer->lower_layer->lslices));
                    LayerBands &out = lb[li];
                    for (const LayerRegion *region : layer->regions()) {
                        visit_paths(region->perimeters, [&](const ExtrusionPath &path, int) {
                            if (!is_perimeter(path.role()))
                                return;
                            const double w = path.width > 0.f ? double(path.width) : 0.45;
                            const auto  &pts = path.polyline.points;
                            for (size_t k = 1; k < pts.size(); ++k) {
                                const Vec2d a  = unscale(Point(pts[k - 1].x(), pts[k - 1].y()));
                                const Vec2d b  = unscale(Point(pts[k].x(), pts[k].y()));
                                const double L = (b - a).norm();
                                if (L <= 0.)
                                    continue;
                                const int    n    = std::max(1, int(std::ceil(L / 0.5)));
                                const double step = L / n;
                                for (int j = 0; j < n; ++j) {
                                    const Vec2d  p    = a + (b - a) * ((j + 0.5) / n);
                                    const double d    = lower.distance_from_lines<true>(p);
                                    const double frac = std::clamp((d + 0.5 * w) / w, 0., 1.);
                                    const int    band = frac <= 0. ? 0 : frac >= 1. ? 5 : 1 + std::min(3, int(frac * 4.));
                                    out.len[band] += step;
                                    if (frac >= out.best[band]) {
                                        out.best[band] = frac;
                                        out.loc[band]  = {int(li), layer->print_z, p.x() + unscale<double>(shift.x()), p.y() + unscale<double>(shift.y()),
                                                          label, role_key(path.role())};
                                    }
                                }
                            }
                        });
                    }
                }
            });
            for (const LayerBands &l : lb)
                for (size_t b = 0; b < 6; ++b) {
                    band_len[b] += l.len[b];
                    if (l.len[b] > 0. && l.best[b] >= band_best[b]) {
                        band_best[b] = l.best[b];
                        band_loc[b]  = l.loc[b];
                    }
                }
        }
        rep.bands_computed = true;
        for (size_t b = 0; b < 6; ++b)
            rep.wall_bands.push_back({band_names[b], band_len[b], band_loc[b]});
    }
    if (rep.bridge_length_mm > 0. || rep.overhang_wall_length_mm > 0.)
        rep.verdict.message = std::to_string(rep.bridge_runs) + " bridge runs (longest " + std::to_string(rep.longest_bridge_mm) + " mm), " +
                              std::to_string(rep.overhang_wall_length_mm) + " mm of overhang wall";
    else
        rep.verdict.message = "no bridges or overhang walls";
    return rep;
}

// ------------------------------------------------------------------------------------------------
// Unsupported regions
// ------------------------------------------------------------------------------------------------

UnsupportedReport check_unsupported(const Print &print, const UnsupportedOptions &opt)
{
    UnsupportedReport rep;
    const Limits lim = limits_from_print(print);
    rep.tolerance_mm = opt.tolerance_mm > 0. ? opt.tolerance_mm : 0.5 * lim.nozzle_diameter;
    rep.verdict.source = "Print layers: each layer's islands (Layer::lslices) against the layer below's islands plus the support footprint "
                         "(area covered by support extrusions of support layers just below), grown by the tolerance";
    rep.verdict.exactness = "per-layer polygon boolean, exact for the sliced geometry; floating = an island with less than min_area of its area "
                            "over the grown support footprint; max_distance is the largest distance of a region vertex from the supported "
                            "area (a lower bound of the true cantilever length, which can lie inside the region). Bridges between two "
                            "anchors are reported as unsupported overhang with their half span as distance.";
    const double tol = scale_(rep.tolerance_mm);
    const double min_area = opt.min_area_mm2 / (SCALING_FACTOR * SCALING_FACTOR);
    for (size_t oi = 0; oi < print.objects().size(); ++oi) {
        const PrintObject *po = print.objects()[oi];
        const Point shift = po->instances().empty() ? Point(0, 0) : po->instances().front().shift;
        const auto  fps   = support_footprints(*po);
        std::vector<std::vector<UnsupportedRegion>> per_layer(po->layers().size());
        tbb::parallel_for(tbb::blocked_range<size_t>(1, std::max<size_t>(1, po->layers().size())), [&](const tbb::blocked_range<size_t> &range) {
            for (size_t li = range.begin(); li < range.end(); ++li) {
                const Layer *layer = po->layers()[li];
                const double gap   = opt.support_gap_mm > 0. ? opt.support_gap_mm : lim.support_top_z_distance + layer->height + 0.05;
                ExPolygons below = layer->lower_layer ? layer->lower_layer->lslices : ExPolygons();
                append(below, support_below(fps, layer->bottom_z(), gap));
                const ExPolygons grown = offset_ex(below, float(tol));
                std::unique_ptr<AABBTreeLines::LinesDistancer<Linef>> dist;
                for (const ExPolygon &island : layer->lslices) {
                    const double a = island.area();
                    if (a < min_area)
                        continue;
                    const ExPolygons supported = intersection_ex(island, grown);
                    if (area(supported) < min_area) {
                        UnsupportedRegion r;
                        r.object_index = int(oi);
                        r.layer        = int(li);
                        r.z            = layer->print_z;
                        r.area_mm2     = area_mm2(island);
                        r.box          = to_box(get_extents(island), shift);
                        r.cx           = r.box.center_x();
                        r.cy           = r.box.center_y();
                        r.floating     = true;
                        per_layer[li].push_back(r);
                        continue;
                    }
                    for (const ExPolygon &piece : diff_ex(island, grown)) {
                        if (piece.area() < min_area)
                            continue;
                        if (!dist)
                            dist = std::make_unique<AABBTreeLines::LinesDistancer<Linef>>(to_unscaled_linesf(grown));
                        double maxd = 0.;
                        for (const Point &p : piece.contour.points)
                            maxd = std::max(maxd, dist->distance_from_lines<false>(unscale(p)));
                        maxd += rep.tolerance_mm;
                        if (maxd < opt.report_distance_mm)
                            continue;
                        UnsupportedRegion r;
                        r.object_index    = int(oi);
                        r.layer           = int(li);
                        r.z               = layer->print_z;
                        r.area_mm2        = area_mm2(piece);
                        r.box             = to_box(get_extents(piece), shift);
                        r.cx              = r.box.center_x();
                        r.cy              = r.box.center_y();
                        r.max_distance_mm = maxd;
                        per_layer[li].push_back(r);
                    }
                }
            }
        });
        for (auto &v : per_layer)
            for (auto &r : v) {
                if (r.floating) {
                    ++rep.floating_islands;
                    rep.floating_area_mm2 += r.area_mm2;
                } else {
                    rep.unsupported_area_mm2 += r.area_mm2;
                    rep.max_distance_mm = std::max(rep.max_distance_mm, r.max_distance_mm);
                }
                rep.regions.push_back(r);
            }
    }
    std::stable_sort(rep.regions.begin(), rep.regions.end(), [](const UnsupportedRegion &a, const UnsupportedRegion &b) {
        if (a.floating != b.floating) return a.floating;
        return a.area_mm2 > b.area_mm2;
    });
    if (rep.regions.size() > opt.max_regions)
        rep.regions.resize(opt.max_regions);
    if (rep.floating_islands > 0)
        rep.verdict.raise("problem", std::to_string(rep.floating_islands) + " islands start in mid-air (" + std::to_string(rep.floating_area_mm2) + " mm2)");
    if (rep.unsupported_area_mm2 > 0.)
        rep.verdict.raise(rep.max_distance_mm > 5. ? "warning" : "ok",
                          std::to_string(rep.unsupported_area_mm2) + " mm2 overhang without support (farthest " + std::to_string(rep.max_distance_mm) + " mm)");
    if (rep.verdict.message.empty())
        rep.verdict.message = "everything rests on the layer below or on support";
    return rep;
}

// ------------------------------------------------------------------------------------------------
// First layer
// ------------------------------------------------------------------------------------------------

FirstLayerReport check_first_layer(const Print &print, const MoveAnalysis *analysis, double aspect_warning)
{
    FirstLayerReport rep;
    rep.verdict.source    = "Print: first object layer islands (lslices), the first support layer when there is a raft; G-code moves for "
                            "brim/skirt; object height from the last layer's print_z";
    rep.verdict.exactness = "areas exact for the sliced first layer (before elephant-foot compensation is not distinguished); aspect "
                            "ratio and slenderness are indicators, not a prediction";
    rep.first_layer_height = print.config().initial_layer_print_height.value;
    struct BrimPiece { double x, y, length; };
    std::vector<BrimPiece> brim_pts; // midpoint and length of every brim move
    if (analysis != nullptr && analysis->result() != nullptr) {
        rep.first_layer_time_s = analysis->summary().first_layer_time_s;
        for_each_move(*analysis->result(), [&](size_t i, float fx, float fy, float fz, const GCodeProcessorResult::MoveVertex &m) {
            if (m.type != EMoveType::Extrude)
                return;
            const double len = std::sqrt(sq(m.position.x() - fx) + sq(m.position.y() - fy) + sq(m.position.z() - fz));
            if (m.extrusion_role == erBrim) {
                rep.brim_length_mm += len;
                rep.brim_box.merge(fx, fy);
                rep.brim_box.merge(m.position.x(), m.position.y());
                brim_pts.push_back({0.5 * (fx + m.position.x()), 0.5 * (fy + m.position.y()), len});
            } else if (m.extrusion_role == erSkirt) {
                rep.skirt_length_mm += len;
                rep.skirt_box.merge(fx, fy);
                rep.skirt_box.merge(m.position.x(), m.position.y());
            }
        });
        rep.has_brim  = rep.brim_length_mm > 0.;
        rep.has_skirt = rep.skirt_length_mm > 0.;
    }
    for (size_t oi = 0; oi < print.objects().size(); ++oi) {
        const PrintObject *po = print.objects()[oi];
        if (po->layers().empty())
            continue;
        const Layer *first = po->layers().front();
        const int    raft  = po->config().raft_layers.value;
        for (size_t k = 0; k < po->instances().size(); ++k) {
            FirstLayerObject fo;
            fo.object_index = int(oi);
            fo.name         = object_name(*po);
            fo.instance     = int(k);
            fo.raft_layers  = raft;
            fo.raft         = raft > 0 && !po->support_layers().empty();
            fo.object_first_layer_area_mm2 = area_mm2(first->lslices);
            fo.islands      = int(first->lslices.size());
            const Point shift = po->instances()[k].shift;
            if (fo.raft) {
                Polygons covered;
                po->support_layers().front()->support_fills.polygons_covered_by_width(covered, float(scale_(0.01)));
                const ExPolygons raft_area = union_ex(covered);
                fo.contact_area_mm2 = area_mm2(raft_area);
                fo.footprint        = to_box(get_extents(raft_area), shift);
            } else {
                fo.contact_area_mm2 = fo.object_first_layer_area_mm2;
                fo.footprint        = to_box(get_extents(first->lslices), shift);
            }
            fo.height_mm    = po->layers().back()->print_z;
            const double mn = std::min(fo.footprint.size_x(), fo.footprint.size_y());
            fo.aspect_ratio = mn > 0. ? fo.height_mm / mn : 0.;
            fo.slenderness  = fo.contact_area_mm2 > 0. ? fo.height_mm / std::sqrt(fo.contact_area_mm2) : 0.;
            const Box2 near = fo.footprint.inflated(20.);
            for (const BrimPiece &b : brim_pts)
                if (near.contains(b.x, b.y))
                    fo.brim_length_mm += b.length;
            if (fo.raft) rep.has_raft = true;
            if (fo.aspect_ratio > aspect_warning && fo.brim_length_mm <= 0. && !fo.raft)
                rep.verdict.raise("warning", fo.name + ": tall and thin (height/width " + std::to_string(fo.aspect_ratio) + ") without brim or raft");
            rep.objects.push_back(fo);
        }
    }
    if (rep.verdict.message.empty())
        rep.verdict.message = "no tall-thin object without brim or raft";
    return rep;
}

// ------------------------------------------------------------------------------------------------
// Seams
// ------------------------------------------------------------------------------------------------

SeamReport check_seams(const MoveAnalysis &analysis, double align_distance_mm, size_t max_samples)
{
    SeamReport rep;
    rep.from_seam_moves   = analysis.seams_from_seam_moves();
    rep.verdict.source    = rep.from_seam_moves ? "Seam markers of the G-code processor (a closed outer wall: midpoint of its start and end)" :
                                                  "start points of outer wall runs (the result has no Seam markers)";
    rep.verdict.exactness = "positions exact to the G-code; angle measured around the XY box centre of the object's outer walls; holes "
                            "and separate islands add their own seams";
    rep.count = int(analysis.seams().size());
    const GCodeProcessorResult *res = analysis.result();
    std::map<int, Box2> boxes;
    if (res != nullptr)
        for_each_move(*res, [&](size_t i, float fx, float fy, float, const GCodeProcessorResult::MoveVertex &m) {
            if (m.type == EMoveType::Extrude && m.extrusion_role == erExternalPerimeter) {
                boxes[analysis.object_at(i)].merge(fx, fy);
                boxes[analysis.object_at(i)].merge(m.position.x(), m.position.y());
            }
        });
    std::map<int, std::vector<const MoveAnalysis::Seam *>> by_object;
    for (const auto &s : analysis.seams())
        by_object[s.object].push_back(&s);
    for (auto &kv : by_object) {
        SeamObject so;
        so.object = kv.first;
        so.count  = int(kv.second.size());
        so.box    = boxes[kv.first];
        if (!so.box.defined())
            for (const auto *s : kv.second) so.box.merge(s->x, s->y);
        so.cx = so.box.center_x();
        so.cy = so.box.center_y();
        double sx = 0., sy = 0., sumx = 0., sumy = 0.;
        int    quad[4] = {0, 0, 0, 0}; // right, rear, left, front
        so.min_y = std::numeric_limits<double>::max();
        so.max_y = std::numeric_limits<double>::lowest();
        for (const auto *s : kv.second) {
            const double a = std::atan2(s->y - so.cy, s->x - so.cx);
            sx += std::cos(a);
            sy += std::sin(a);
            double deg = a * 180. / kPi;
            if (deg < 0.) deg += 360.;
            const int q = int(std::floor(std::fmod(deg + 45., 360.) / 90.));
            quad[std::clamp(q, 0, 3)]++;
            so.min_y = std::min(so.min_y, double(s->y));
            so.max_y = std::max(so.max_y, double(s->y));
            sumx += s->x;
            sumy += s->y;
        }
        const double n = double(so.count);
        so.mean_x = sumx / n;
        so.mean_y = sumy / n;
        so.concentration  = std::sqrt(sq(sx) + sq(sy)) / n;
        double mean       = std::atan2(sy, sx) * 180. / kPi;
        so.mean_angle_deg = mean < 0. ? mean + 360. : mean;
        so.angular_spread_deg = so.concentration > 0. ? std::sqrt(-2. * std::log(std::min(1., so.concentration))) * 180. / kPi : 180.;
        so.right_share = quad[0] / n;
        so.rear_share  = quad[1] / n;
        so.left_share  = quad[2] / n;
        so.front_share = quad[3] / n;
        // Steps: each seam to the nearest seam of the previous layer that has seams.
        std::map<int, std::vector<const MoveAnalysis::Seam *>> by_layer;
        for (const auto *s : kv.second)
            by_layer[s->layer].push_back(s);
        std::vector<double> steps;
        const std::vector<const MoveAnalysis::Seam *> *prev = nullptr;
        for (auto &lk : by_layer) {
            if (prev != nullptr)
                for (const auto *s : lk.second) {
                    double best = std::numeric_limits<double>::max();
                    for (const auto *p : *prev)
                        best = std::min(best, std::sqrt(sq(s->x - p->x) + sq(s->y - p->y)));
                    steps.push_back(best);
                }
            prev = &lk.second;
        }
        if (!steps.empty()) {
            std::vector<double> sorted = steps;
            std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
            so.median_step_mm = sorted[sorted.size() / 2];
            so.aligned_share  = double(std::count_if(steps.begin(), steps.end(), [align_distance_mm](double d) { return d <= align_distance_mm; })) /
                               double(steps.size());
        }
        so.pattern = so.aligned_share >= 0.8 ? "aligned" : (so.aligned_share <= 0.3 ? "scattered" : "mixed");
        // Samples spread over the list.
        const size_t step = std::max<size_t>(1, kv.second.size() / std::max<size_t>(1, max_samples));
        for (size_t i = 0; i < kv.second.size() && so.samples.size() < max_samples; i += step) {
            const auto *s = kv.second[i];
            so.samples.push_back({s->layer, s->z, s->x, s->y, s->object, "outer_wall"});
        }
        rep.objects.push_back(so);
    }
    if (rep.count == 0)
        rep.verdict.raise("not_applicable", "no seams found");
    else {
        std::string msg;
        for (const SeamObject &so : rep.objects)
            msg += (msg.empty() ? "" : "; ") + std::string("object ") + std::to_string(so.object) + ": " + so.pattern + ", mean angle " +
                   std::to_string(int(std::lround(so.mean_angle_deg))) + " deg, rear share " + std::to_string(int(std::lround(so.rear_share * 100.))) + "%";
        rep.verdict.message = msg;
    }
    return rep;
}

// ------------------------------------------------------------------------------------------------
// Thin features
// ------------------------------------------------------------------------------------------------

ThinFeatureReport check_thin_features(const MoveAnalysis &analysis, const Print *print, double narrow_width_mm, double min_area_mm2, size_t max_items)
{
    ThinFeatureReport rep;
    rep.verdict.source    = "G-code moves of the gap fill feature and wall moves narrower than the threshold (width as the processor "
                            "derives it from E, height and length); with a Print, layer islands minus the area covered by the layer's "
                            "extrusions (polygons_covered_by_width)";
    rep.verdict.exactness = "gap fill amounts exact; move widths are reconstructed and approximate; uncovered area is exact for the "
                            "sliced geometry but includes regions intentionally left open (e.g. gaps below the minimum gap-fill length)";
    Limits lim;
    if (print != nullptr)
        lim = limits_from_print(*print);
    rep.narrow_width_mm = narrow_width_mm > 0. ? narrow_width_mm : 0.75 * lim.nozzle_diameter;
    const GCodeProcessorResult *res = analysis.result();
    if (res != nullptr) {
        std::set<int> gl;
        for_each_move(*res, [&](size_t i, float fx, float fy, float fz, const GCodeProcessorResult::MoveVertex &m) {
            if (m.type != EMoveType::Extrude)
                return;
            const double len = std::sqrt(sq(m.position.x() - fx) + sq(m.position.y() - fy) + sq(m.position.z() - fz));
            if (m.extrusion_role == erGapFill) {
                rep.gap_fill_length_mm += len;
                rep.gap_fill_volume_mm3 += double(m.delta_extruder) * filament_area(*res, m.extruder_id);
                gl.insert(int(m.layer_id));
            }
            if (is_perimeter(m.extrusion_role) && m.width > 0.f && m.width < rep.narrow_width_mm && len > 0.) {
                rep.narrow_length_mm += len;
                if (rep.narrow_examples.size() < max_items)
                    rep.narrow_examples.push_back({int(m.layer_id), m.position.z(), m.position.x(), m.position.y(), analysis.object_at(i), role_key(m.extrusion_role)});
            }
        });
        rep.layers_with_gap_fill = int(gl.size());
        rep.gap_fill_clusters = cluster_segments(collect_segments(analysis, [](ExtrusionRole r) { return r == erGapFill; }), 2., max_items);
    }
    if (print != nullptr) {
        rep.uncovered_computed = true;
        const double min_area = min_area_mm2 / (SCALING_FACTOR * SCALING_FACTOR);
        for (size_t oi = 0; oi < print->objects().size(); ++oi) {
            const PrintObject *po = print->objects()[oi];
            const Point shift = po->instances().empty() ? Point(0, 0) : po->instances().front().shift;
            std::vector<std::vector<UnsupportedRegion>> per_layer(po->layers().size());
            tbb::parallel_for(tbb::blocked_range<size_t>(0, po->layers().size()), [&](const tbb::blocked_range<size_t> &range) {
                for (size_t li = range.begin(); li < range.end(); ++li) {
                    const Layer *layer = po->layers()[li];
                    Polygons covered;
                    for (const LayerRegion *region : layer->regions()) {
                        region->perimeters.polygons_covered_by_width(covered, float(scale_(0.01)));
                        region->fills.polygons_covered_by_width(covered, float(scale_(0.01)));
                        region->thin_fills.polygons_covered_by_width(covered, float(scale_(0.01)));
                        // Areas handed to infill count as covered (sparse infill leaves gaps by design).
                        append(covered, to_polygons(region->fill_expolygons));
                    }
                    // Grow slightly so the rounding between neighbouring lines does not count.
                    const ExPolygons cov = offset_ex(union_ex(covered), float(scale_(0.05)));
                    for (const ExPolygon &piece : diff_ex(layer->lslices, cov)) {
                        if (piece.area() < min_area)
                            continue;
                        UnsupportedRegion r;
                        r.object_index = int(oi);
                        r.layer        = int(li);
                        r.z            = layer->print_z;
                        r.area_mm2     = area_mm2(piece);
                        r.box          = to_box(get_extents(piece), shift);
                        r.cx           = r.box.center_x();
                        r.cy           = r.box.center_y();
                        per_layer[li].push_back(r);
                    }
                }
            });
            for (auto &v : per_layer)
                for (auto &r : v) {
                    rep.uncovered_area_mm2 += r.area_mm2;
                    rep.uncovered.push_back(r);
                }
        }
        std::sort(rep.uncovered.begin(), rep.uncovered.end(), [](const UnsupportedRegion &a, const UnsupportedRegion &b) { return a.area_mm2 > b.area_mm2; });
        if (rep.uncovered.size() > max_items)
            rep.uncovered.resize(max_items);
        if (rep.uncovered_area_mm2 > 0.)
            rep.verdict.raise("warning", std::to_string(rep.uncovered_area_mm2) + " mm2 of the sliced layers is not covered by any extrusion");
    }
    if (rep.gap_fill_length_mm > 0.)
        rep.verdict.raise("ok", std::to_string(rep.gap_fill_length_mm) + " mm gap fill on " + std::to_string(rep.layers_with_gap_fill) + " layers");
    if (rep.verdict.message.empty())
        rep.verdict.message = "no thin features detected";
    return rep;
}

// ------------------------------------------------------------------------------------------------
// Multi-material
// ------------------------------------------------------------------------------------------------

MultiMaterialReport check_multi_material(const MoveAnalysis &analysis, const Print *print)
{
    MultiMaterialReport rep;
    rep.verdict.source    = "Tool_change moves and prime tower extrusions of the G-code; slicer's flush_per_filament and "
                            "filament_change_count_map; with a Print, its wipe tower data";
    rep.verdict.exactness = "counts and volumes exact; tower volume per transition = prime tower extrusion between the change and the next "
                            "non-tower extrusion (includes the tower's sparse layer part printed in that block)";
    const GCodeProcessorResult *res = analysis.result();
    const Summary &s = analysis.summary();
    rep.tower_volume_mm3 = s.wipe_tower_mm3;
    rep.flush_mm3        = s.flush_mm3;
    rep.wipe_tower_box   = s.wipe_tower_box;
    rep.has_wipe_tower   = s.wipe_tower_box.defined();
    if (res == nullptr) {
        rep.verdict.verdict = "not_applicable";
        return rep;
    }
    std::map<int, int> per_layer;
    for (const auto &tc : analysis.tool_changes()) {
        if (tc.from < 0)
            continue;
        Transition t;
        t.layer = tc.layer;
        t.z     = tc.z;
        t.from  = tc.from;
        t.to    = tc.to;
        for (size_t i = tc.move + 1; i < res->moves.size(); ++i) {
            const auto &m = res->moves[i];
            if (m.internal_only || m.type != EMoveType::Extrude)
                continue;
            if (m.extrusion_role != erWipeTower)
                break;
            t.tower_volume_mm3 += double(m.delta_extruder) * filament_area(*res, m.extruder_id);
        }
        rep.transitions.push_back(t);
        per_layer[t.layer]++;
    }
    rep.tool_changes = int(rep.transitions.size());
    for (const auto &kv : per_layer) {
        rep.changes_per_layer.emplace_back(kv.first, kv.second);
        rep.max_changes_per_layer = std::max(rep.max_changes_per_layer, kv.second);
    }
    std::map<int, std::vector<int>> fil_layers;
    for (const LayerRow &row : analysis.layers())
        for (const LayerFilamentStats &f : row.filaments)
            if (f.volume_mm3 > 0.)
                fil_layers[f.filament].push_back(row.index);
    for (auto &kv : fil_layers) {
        FilamentRange fr;
        fr.filament    = kv.first;
        fr.layers      = int(kv.second.size());
        fr.first_layer = kv.second.front();
        fr.last_layer  = kv.second.back();
        int a = kv.second.front(), b = a;
        for (size_t i = 1; i < kv.second.size(); ++i) {
            if (kv.second[i] == b + 1) b = kv.second[i];
            else { fr.ranges.emplace_back(a, b); a = b = kv.second[i]; }
        }
        fr.ranges.emplace_back(a, b);
        rep.filaments.push_back(fr);
    }
    for (const auto &kv : res->filament_change_count_map)
        rep.transition_counts.push_back({kv.first, kv.second});
    for (const auto &kv : res->print_statistics.flush_per_filament)
        rep.flush_per_filament.emplace_back(int(kv.first), kv.second);
    for (const LayerRow &row : analysis.layers())
        for (const LayerRoleStats &r : row.roles)
            if (r.role == "prime_tower")
                rep.wipe_tower_height = std::max(rep.wipe_tower_height, row.z);
    if (print != nullptr && rep.has_wipe_tower) {
        const WipeTowerData &wt = print->wipe_tower_data();
        rep.tower_width      = wt.width;
        rep.tower_depth      = wt.depth;
        rep.tower_brim_width = wt.brim_width;
    }
    rep.verdict.message = std::to_string(rep.tool_changes) + " filament changes, " + std::to_string(rep.filaments.size()) + " filaments, " +
                          std::to_string(rep.tower_volume_mm3) + " mm3 prime tower, " + std::to_string(rep.flush_mm3) + " mm3 flush";
    return rep;
}

// ------------------------------------------------------------------------------------------------
// Cross-sections
// ------------------------------------------------------------------------------------------------

SectionFromMoves section_from_moves(const MoveAnalysis &analysis, int layer, const Box2 &box)
{
    SectionFromMoves sec;
    sec.layer = layer;
    sec.query = box;
    sec.area_mm2 = box.area();
    const GCodeProcessorResult *res = analysis.result();
    if (res == nullptr || layer < 0 || size_t(layer) >= analysis.layers().size())
        return sec;
    const LayerRow &row = analysis.layers()[layer];
    sec.z = row.z;
    if (row.length_mm <= 0.)
        return sec;
    std::array<SectionRole, erCount> roles;
    std::array<double, erCount>      wsum{};
    std::map<int, double>            fil;
    // Position before the first move of the layer.
    size_t start = row.first_move;
    Vec3f  last  = res->moves[start].position;
    for (size_t i = start; i > 0; --i)
        if (!res->moves[i - 1].internal_only && res->moves[i - 1].type != EMoveType::Seam) {
            last = res->moves[i - 1].position;
            break;
        }
    for (size_t i = start; i <= row.last_move && i < res->moves.size(); ++i) {
        const auto &m = res->moves[i];
        if (m.internal_only || m.type == EMoveType::Seam)
            continue;
        const Vec3f from = last;
        last             = m.position;
        if (m.type != EMoveType::Extrude || m.extrusion_role == erCustom)
            continue;
        const double frac = clip_fraction(from.x(), from.y(), m.position.x(), m.position.y(), box);
        if (frac <= 0.)
            continue;
        const double len = (m.position - from).cast<double>().norm() * frac;
        SectionRole &r = roles[m.extrusion_role];
        r.length_mm += len;
        r.volume_mm3 += double(m.delta_extruder) * filament_area(*res, m.extruder_id) * frac;
        wsum[m.extrusion_role] += len * m.width;
        fil[m.extruder_id] += len;
        sec.extruded_area_mm2 += len * m.width;
        if (m.extrusion_role == erInternalInfill)
            sec.infill_area_mm2 += len * m.width;
    }
    for (size_t r = 0; r < size_t(erCount); ++r)
        if (roles[r].length_mm > 0.) {
            roles[r].role       = role_key(ExtrusionRole(r));
            roles[r].mean_width = wsum[r] / roles[r].length_mm;
            sec.roles.push_back(roles[r]);
        }
    for (const auto &kv : fil)
        sec.filaments.emplace_back(kv.first, kv.second);
    return sec;
}

SectionFromPrint section_from_print(const Print &print, double z, const Box2 &box)
{
    SectionFromPrint sec;
    sec.query = box;
    sec.z     = z;
    std::array<SectionRole, erCount> roles;
    std::array<double, erCount>      wsum{};
    std::set<int> insets;
    double wall_len = 0., wall_w = 0., layer_h = 0., wall_area = 0.;
    for (size_t oi = 0; oi < print.objects().size(); ++oi) {
        const PrintObject *po = print.objects()[oi];
        const Layer *layer = nullptr;
        double best = std::numeric_limits<double>::max();
        for (const Layer *l : po->layers()) {
            const double d = std::abs(l->print_z - z);
            if (d < best && d <= 0.5 * l->height + EPSILON) { best = d; layer = l; }
        }
        if (layer == nullptr)
            continue;
        for (const PrintInstance &inst : po->instances()) {
            const Point shift = inst.shift;
            // Query box in the object's frame (unscaled), for segment clipping.
            Box2 qb;
            if (box.defined())
                qb = Box2(box.min_x - unscale<double>(shift.x()), box.min_y - unscale<double>(shift.y()), box.max_x - unscale<double>(shift.x()),
                          box.max_y - unscale<double>(shift.y()));
            const ExPolygons islands = box.defined() ? intersection_ex(layer->lslices, Polygons{box_polygon(box, shift)}) : layer->lslices;
            if (islands.empty())
                continue;
            if (sec.layer < 0) {
                sec.layer = int(layer->id());
                sec.z     = layer->print_z;
            }
            layer_h = layer->height;
            sec.island_area_mm2 += area_mm2(islands);
            for (const ExPolygon &isl : layer->lslices)
                if (!box.defined() || !intersection_ex(isl, Polygons{box_polygon(box, shift)}).empty())
                    ++sec.islands;
            for (const LayerRegion *region : layer->regions()) {
                ExPolygons rs = to_expolygons(region->slices.surfaces);
                if (box.defined())
                    rs = intersection_ex(rs, Polygons{box_polygon(box, shift)});
                const double ra = area_mm2(rs);
                if (ra > 1e-6) {
                    SectionRegion sr;
                    sr.object_index   = int(oi);
                    sr.region_id      = region->region().print_object_region_id();
                    const PrintRegionConfig &rc = region->region().config();
                    sr.infill_pattern = rc.option("sparse_infill_pattern") ? rc.option("sparse_infill_pattern")->serialize() : std::string();
                    sr.infill_density = rc.sparse_infill_density.value;
                    sr.wall_loops     = rc.wall_loops.value;
                    sr.slice_area_mm2 = ra;
                    bool merged = false;
                    for (SectionRegion &e : sec.regions)
                        if (e.object_index == sr.object_index && e.region_id == sr.region_id) { e.slice_area_mm2 += ra; merged = true; }
                    if (!merged)
                        sec.regions.push_back(sr);
                }
                auto add_path = [&](const ExtrusionPath &path, int inset) {
                    const auto &pts = path.polyline.points;
                    double len = 0.;
                    for (size_t k = 1; k < pts.size(); ++k) {
                        const double x0 = unscale<double>(pts[k - 1].x()), y0 = unscale<double>(pts[k - 1].y());
                        const double x1 = unscale<double>(pts[k].x()), y1 = unscale<double>(pts[k].y());
                        len += std::sqrt(sq(x1 - x0) + sq(y1 - y0)) * clip_fraction(x0, y0, x1, y1, qb);
                    }
                    if (len <= 0.)
                        return;
                    const ExtrusionRole r = path.role();
                    roles[r].length_mm += len;
                    roles[r].volume_mm3 += len * std::max(0., path.mm3_per_mm);
                    wsum[r] += len * path.width;
                    if (is_perimeter(r)) {
                        if (inset >= 0)
                            insets.insert(inset);
                        wall_len += len;
                        wall_w += len * path.width;
                        wall_area += len * path.width;
                    } else if (r == erInternalInfill) {
                        sec.sparse_infill_area_mm2 += len * path.width;
                    } else if (r == erSolidInfill || r == erTopSolidInfill || r == erBottomSurface || is_bridge_role(r)) {
                        sec.solid_infill_area_mm2 += len * path.width;
                    }
                };
                visit_paths(region->perimeters, add_path);
                visit_paths(region->fills, add_path);
                visit_paths(region->thin_fills, add_path);
            }
        }
    }
    for (size_t r = 0; r < size_t(erCount); ++r)
        if (roles[r].length_mm > 0.) {
            roles[r].role       = role_key(ExtrusionRole(r));
            roles[r].mean_width = wsum[r] / roles[r].length_mm;
            sec.roles.push_back(roles[r]);
        }
    sec.perimeter_count = int(insets.size());
    sec.wall_width_mm   = wall_len > 0. ? wall_w / wall_len : 0.;
    if (sec.perimeter_count > 0) {
        const double spacing  = sec.wall_width_mm - layer_h * (1. - 0.25 * kPi);
        sec.wall_thickness_mm = (sec.perimeter_count - 1) * spacing + sec.wall_width_mm;
    }
    const double open = sec.island_area_mm2 - wall_area - sec.solid_infill_area_mm2;
    sec.sparse_infill_fill_ratio = open > 1e-6 ? sec.sparse_infill_area_mm2 / open : 0.;
    return sec;
}

// ------------------------------------------------------------------------------------------------
// Diff
// ------------------------------------------------------------------------------------------------

std::vector<std::pair<std::string, double>> flatten(const Summary &s)
{
    std::vector<std::pair<std::string, double>> out;
    auto add = [&out](const std::string &k, double v) { out.emplace_back(k, v); };
    add("time_s", s.time_s);
    add("silent_time_s", s.silent_time_s);
    add("first_layer_time_s", s.first_layer_time_s);
    add("extrusion_time_s", s.extrusion_time_s);
    add("travel_time_s", s.travel_time_s);
    add("total_mm3", s.total_mm3);
    add("model_mm3", s.model_mm3);
    add("support_mm3", s.support_mm3);
    add("wipe_tower_mm3", s.wipe_tower_mm3);
    add("flush_mm3", s.flush_mm3);
    add("total_length_mm", s.total_length_mm);
    add("total_weight_g", s.total_weight_g);
    add("total_cost", s.total_cost);
    add("layer_count", double(s.layer_count));
    add("first_layer_height", s.first_layer_height);
    add("layer_height", s.layer_height);
    add("max_z", s.max_z);
    add("tool_changes", s.tool_changes);
    add("filament_changes", s.filament_changes);
    add("travel_distance_mm", s.travel_distance_mm);
    add("travel_moves", s.travel_moves);
    add("retractions", s.retractions);
    add("seams", s.seams);
    for (const RoleStats &r : s.roles) {
        const std::string p = "roles." + r.role + ".";
        add(p + "time_s", r.time_s);
        add(p + "length_mm", r.length_mm);
        add(p + "volume_mm3", r.volume_mm3);
        add(p + "weight_g", r.weight_g);
        add(p + "max_speed", r.max_speed);
        add(p + "mean_speed", r.mean_speed);
        add(p + "max_flow", r.max_flow);
    }
    for (const FilamentStats &f : s.filaments) {
        const std::string p = "filaments." + std::to_string(f.filament) + ".";
        add(p + "total_mm3", f.total_mm3);
        add(p + "weight_g", f.weight_g);
        add(p + "length_mm", f.length_mm);
        add(p + "cost", f.cost);
        add(p + "flush_mm3", f.flush_mm3);
        add(p + "support_mm3", f.support_mm3);
    }
    for (const ObjectStats &o : s.objects) {
        const std::string p = "objects." + std::to_string(o.label_id) + ".";
        add(p + "time_s", o.time_s);
        add(p + "volume_mm3", o.volume_mm3);
        add(p + "support_mm3", o.support_mm3);
    }
    return out;
}

std::vector<DiffEntry> diff(const Summary &before, const Summary &after, double rel_tolerance, double abs_tolerance)
{
    std::map<std::string, double> a, b;
    for (const auto &kv : flatten(before)) a[kv.first] = kv.second;
    for (const auto &kv : flatten(after)) b[kv.first] = kv.second;
    std::set<std::string> keys;
    for (const auto &kv : a) keys.insert(kv.first);
    for (const auto &kv : b) keys.insert(kv.first);
    std::vector<DiffEntry> out;
    for (const std::string &k : keys) {
        DiffEntry e;
        e.key      = k;
        auto ia    = a.find(k);
        auto ib    = b.find(k);
        e.removed  = ib == b.end();
        e.added    = ia == a.end();
        e.before   = e.added ? 0. : ia->second;
        e.after    = e.removed ? 0. : ib->second;
        e.delta    = e.after - e.before;
        e.relative = std::abs(e.before) > 0. ? e.delta / std::abs(e.before) : 0.;
        if (std::abs(e.delta) <= abs_tolerance)
            continue;
        if (e.before != 0. && std::abs(e.relative) <= rel_tolerance)
            continue;
        out.push_back(e);
    }
    std::stable_sort(out.begin(), out.end(), [](const DiffEntry &x, const DiffEntry &y) {
        const double rx = x.before == 0. ? std::numeric_limits<double>::max() : std::abs(x.relative);
        const double ry = y.before == 0. ? std::numeric_limits<double>::max() : std::abs(y.relative);
        return rx > ry;
    });
    return out;
}

} } // namespace Slic3r::SliceAnalysis
