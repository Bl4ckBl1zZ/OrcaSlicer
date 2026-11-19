#include "PaintStats.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <numeric>
#include <set>
#include <unordered_map>
#include <vector>

#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleSelector.hpp"

#include "../../McpError.hpp"

namespace Slic3r { namespace GUI { namespace Mcp { namespace Paint {

namespace {

double r3(double v) { return std::round(v * 1000.) / 1000.; }
double r6(double v) { return std::round(v * 1e6) / 1e6; }

json vec_json(const Vec3d& v) { return json::array({ r3(v.x()), r3(v.y()), r3(v.z()) }); }

} // namespace

json box_json(const BoundingBoxf3& box)
{
    if (!box.defined)
        return nullptr;
    json out = json::object();
    out["min"] = vec_json(box.min);
    out["max"] = vec_json(box.max);
    return out;
}

bool parse_channel(const std::string& name, FacetPaint::Channel& out)
{
    if (name == "support") out = FacetPaint::Channel::Support;
    else if (name == "seam") out = FacetPaint::Channel::Seam;
    else if (name == "color") out = FacetPaint::Channel::Color;
    else if (name == "fuzzy_skin") out = FacetPaint::Channel::FuzzySkin;
    else return false;
    return true;
}

const char* channel_name(FacetPaint::Channel channel)
{
    switch (channel) {
    case FacetPaint::Channel::Support: return "support";
    case FacetPaint::Channel::Seam: return "seam";
    case FacetPaint::Channel::Color: return "color";
    case FacetPaint::Channel::FuzzySkin: return "fuzzy_skin";
    }
    return "support";
}

std::string state_name(FacetPaint::Channel channel, EnforcerBlockerType state)
{
    if (state == EnforcerBlockerType::NONE)
        return "none";
    switch (channel) {
    case FacetPaint::Channel::Support:
    case FacetPaint::Channel::Seam: return state == EnforcerBlockerType::ENFORCER ? "enforce" : state == EnforcerBlockerType::BLOCKER ? "block" : "state_" + std::to_string(int(state));
    case FacetPaint::Channel::FuzzySkin: return state == EnforcerBlockerType::FUZZY_SKIN ? "fuzzy" : "state_" + std::to_string(int(state));
    case FacetPaint::Channel::Color: return "filament_" + std::to_string(int(state));
    }
    return "state_" + std::to_string(int(state));
}

EnforcerBlockerType parse_state(FacetPaint::Channel channel, const json& value, const char* what, int filament_count)
{
    const std::string ch = channel_name(channel);
    if (value.is_string()) {
        const std::string s = value.get<std::string>();
        if (s == "none")
            return EnforcerBlockerType::NONE;
        if ((channel == FacetPaint::Channel::Support || channel == FacetPaint::Channel::Seam) && (s == "enforce" || s == "block"))
            return s == "enforce" ? EnforcerBlockerType::ENFORCER : EnforcerBlockerType::BLOCKER;
        if (channel == FacetPaint::Channel::FuzzySkin && s == "fuzzy")
            return EnforcerBlockerType::FUZZY_SKIN;
    } else if (value.is_object() && value.contains("filament") && value["filament"].is_number_integer()) {
        if (channel != FacetPaint::Channel::Color)
            fail("invalid_argument", std::string("`") + what + "`: {\"filament\": n} is a state of the color channel, not of " + ch + ".",
                 ch == "fuzzy_skin" ? "fuzzy_skin takes \"fuzzy\" or \"none\"." : "support and seam take \"enforce\", \"block\" or \"none\".");
        const int n = value["filament"].get<int>();
        if (n < 1 || n > int(EnforcerBlockerType::ExtruderMax))
            fail("invalid_argument", std::string("`") + what + ".filament` must be within 1.." + std::to_string(int(EnforcerBlockerType::ExtruderMax)) + " (got " + std::to_string(n) + ").");
        if (n > filament_count)
            fail("invalid_argument",
                 std::string("`") + what + ".filament`: there is no filament slot " + std::to_string(n) + "; the project has " + std::to_string(filament_count) + ".",
                 "Add filament slots first (filament_edit with action \"add\", or the + button of the Filament list in the sidebar), then paint. "
                 "filament_list shows the slots.",
                 json::object({ { "filament_count", filament_count }, { "requested", n } }));
        return EnforcerBlockerType(n);
    }
    std::string accepted = channel == FacetPaint::Channel::Color ? "{\"filament\": n} or \"none\"" :
                           channel == FacetPaint::Channel::FuzzySkin ? "\"fuzzy\" or \"none\"" : "\"enforce\", \"block\" or \"none\"";
    fail("invalid_argument", std::string("`") + what + "` = " + value.dump() + " is not a state of the " + ch + " channel. Accepted: " + accepted + ".");
}

json states_json(FacetPaint::Channel channel, const FacetPaint::Stats& stats)
{
    json out = json::object();
    for (const FacetPaint::StateStats& s : stats.states) {
        json e = json::object();
        e["area_mm2"] = r3(s.area);
        e["fraction"] = r6(s.fraction);
        e["triangles"] = s.triangles;
        e["box"] = box_json(s.bbox_world); // FacetPaint's world frame is the plate frame here
        e["box_object"] = box_json(s.bbox_object);
        out[state_name(channel, s.state)] = std::move(e);
    }
    return out;
}

namespace {

struct Leaf
{
    Vec3f               a, b, c;
    EnforcerBlockerType state;
    int                 facet;
};

std::vector<Leaf> painted_leaves(const PaintTarget& target, FacetPaint::Channel channel)
{
    std::vector<Leaf> leaves;
    const FacetsAnnotation& annotation = FacetPaint::annotation(*target.volume, channel);
    if (annotation.empty())
        return leaves;
    TriangleSelector selector(target.volume->mesh());
    selector.deserialize(annotation.get_data(), false, FacetPaint::max_state(channel));
    selector.for_each_leaf([&leaves](const Vec3f& a, const Vec3f& b, const Vec3f& c, EnforcerBlockerType state, int facet) {
        if (state != EnforcerBlockerType::NONE)
            leaves.push_back({ a, b, c, state, facet });
    });
    return leaves;
}

struct VertexKey
{
    float x, y, z;
    bool operator==(const VertexKey& o) const { return std::memcmp(this, &o, sizeof(VertexKey)) == 0; }
};
struct VertexHash
{
    size_t operator()(const VertexKey& k) const
    {
        uint32_t v[3];
        std::memcpy(v, &k, sizeof(v));
        size_t h = v[0];
        h = h * 1000003u ^ v[1];
        h = h * 1000003u ^ v[2];
        return h;
    }
};

size_t find_root(std::vector<size_t>& parent, size_t i)
{
    while (parent[i] != i) {
        parent[i] = parent[parent[i]];
        i = parent[i];
    }
    return i;
}

} // namespace

json components_json(const PaintTarget& target, FacetPaint::Channel channel, size_t limit, size_t& total)
{
    total = 0;
    const std::vector<Leaf> leaves = painted_leaves(target, channel);
    json out = json::array();
    if (leaves.empty())
        return out;

    // Union-find over leaves of the same state sharing a vertex (split children share their
    // vertices exactly, neighbouring facets share the mesh's vertices).
    std::vector<size_t> parent(leaves.size());
    std::iota(parent.begin(), parent.end(), size_t(0));
    std::map<EnforcerBlockerType, std::unordered_map<VertexKey, size_t, VertexHash>> by_state;
    for (size_t i = 0; i < leaves.size(); ++i) {
        auto& table = by_state[leaves[i].state];
        for (const Vec3f* p : { &leaves[i].a, &leaves[i].b, &leaves[i].c }) {
            const VertexKey key { p->x(), p->y(), p->z() };
            auto [it, inserted] = table.emplace(key, i);
            if (!inserted) {
                const size_t ra = find_root(parent, i), rb = find_root(parent, it->second);
                if (ra != rb)
                    parent[ra] = rb;
            }
        }
    }

    // T-junctions: a leaf next to a finer-split neighbour shares no vertex with the leaves along
    // their common edge, only that edge. The finer leaves' corners on it are midpoints of it
    // (TriangleSelector splits at 0.5f * (a + b)), so follow the midpoints of every leaf edge.
    for (size_t i = 0; i < leaves.size(); ++i) {
        const auto& table = by_state[leaves[i].state];
        std::function<void(const Vec3f&, const Vec3f&, int)> follow = [&](const Vec3f& p, const Vec3f& q, int depth) {
            const Vec3f m = 0.5f * (p + q);
            auto it = table.find(VertexKey { m.x(), m.y(), m.z() });
            if (it == table.end())
                return;
            const size_t ra = find_root(parent, i), rb = find_root(parent, it->second);
            if (ra != rb)
                parent[ra] = rb;
            if (depth < 20) {
                follow(p, m, depth + 1);
                follow(m, q, depth + 1);
            }
        };
        const Leaf& l = leaves[i];
        follow(l.a, l.b, 0);
        follow(l.b, l.c, 0);
        follow(l.c, l.a, 0);
    }

    const Transform3d world = target.instance_plate() * target.volume->get_matrix();
    const Transform3d object = target.volume->get_matrix();
    struct Region
    {
        EnforcerBlockerType state { EnforcerBlockerType::NONE };
        double              area { 0. };
        size_t              triangles { 0 };
        BoundingBoxf3       box, box_object;
        Vec3d               centroid { Vec3d::Zero() };
        Vec3d               normal { Vec3d::Zero() };
        std::set<int>       facets;
    };
    std::unordered_map<size_t, Region> regions;
    for (size_t i = 0; i < leaves.size(); ++i) {
        Region&     r = regions[find_root(parent, i)];
        const Leaf& l = leaves[i];
        const Vec3d a = world * l.a.cast<double>(), b = world * l.b.cast<double>(), c = world * l.c.cast<double>();
        const Vec3d cross = (b - a).cross(c - a);
        const double area = 0.5 * cross.norm();
        r.state = l.state;
        r.area += area;
        ++r.triangles;
        r.centroid += area * (a + b + c) / 3.;
        r.normal += 0.5 * cross; // area-weighted
        for (const Vec3d& p : { a, b, c })
            r.box.merge(p);
        for (const Vec3f* p : { &l.a, &l.b, &l.c })
            r.box_object.merge(Vec3d(object * p->cast<double>()));
        if (r.facets.size() < 64)
            r.facets.insert(l.facet);
    }
    std::vector<Region*> sorted;
    for (auto& [root, region] : regions)
        sorted.push_back(&region);
    std::sort(sorted.begin(), sorted.end(), [](const Region* x, const Region* y) { return x->area > y->area; });
    total = sorted.size();
    for (size_t i = 0; i < sorted.size() && i < limit; ++i) {
        const Region& r = *sorted[i];
        json e = json::object();
        e["state"] = state_name(channel, r.state);
        e["area_mm2"] = r3(r.area);
        e["triangles"] = r.triangles;
        e["box"] = box_json(r.box);
        e["box_object"] = box_json(r.box_object);
        e["centroid"] = vec_json(r.area > 0. ? Vec3d(r.centroid / r.area) : r.box.center());
        const double n = r.normal.norm();
        e["mean_normal"] = n > 1e-12 ? vec_json(r.normal / n) : json(nullptr);
        // How flat the region is: |sum of area vectors| / area = 1 for a plane, ~0 for a closed shell.
        e["flatness"] = r.area > 0. ? r3(n / r.area) : 0.;
        out.push_back(std::move(e));
    }
    return out;
}

json facets_json(const PaintTarget& target, FacetPaint::Channel channel, size_t limit)
{
    std::map<EnforcerBlockerType, std::set<int>> per_state;
    for (const Leaf& l : painted_leaves(target, channel))
        per_state[l.state].insert(l.facet);
    json out = json::object();
    for (const auto& [state, ids] : per_state) {
        json e = json::object();
        e["count"] = ids.size();
        json list = json::array();
        for (int id : ids) {
            if (list.size() >= limit)
                break;
            list.push_back(id);
        }
        e["ids"] = std::move(list);
        e["truncated"] = ids.size() > limit;
        out[state_name(channel, state)] = std::move(e);
    }
    return out;
}

}}}} // namespace Slic3r::GUI::Mcp::Paint
