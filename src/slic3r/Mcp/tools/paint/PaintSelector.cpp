#include "PaintSelector.hpp"

#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <set>

#include "libslic3r/AABBMesh.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "../../McpError.hpp"
#include "../../McpIds.hpp"

namespace Slic3r { namespace GUI { namespace Mcp { namespace Paint {

// ---------------------------------------------------------------------------------------------
// Frames and targets
// ---------------------------------------------------------------------------------------------

FrameKind frame_from(const json& value, const std::string& path, FrameKind fallback)
{
    if (value.is_null())
        return fallback;
    if (value.is_string()) {
        const std::string s = value.get<std::string>();
        if (s == "plate")
            return FrameKind::Plate;
        if (s == "world")
            return FrameKind::World;
        if (s == "object")
            return FrameKind::Object;
    }
    fail("invalid_argument", "`" + path + "` must be \"plate\", \"world\" or \"object\" (got " + value.dump() + ").");
}

const char* frame_name(FrameKind frame)
{
    switch (frame) {
    case FrameKind::Plate: return "plate";
    case FrameKind::World: return "world";
    case FrameKind::Object: return "object";
    }
    return "plate";
}

Transform3d PaintTarget::instance_plate() const
{
    Transform3d out = instance_world;
    out.pretranslate(-plate_origin);
    return out;
}

FacetPaint::Transforms PaintTarget::transforms() const
{
    FacetPaint::Transforms t;
    t.volume = volume->get_matrix();
    t.instance = instance_plate();
    return t;
}

std::string PaintTarget::id() const { return Ids::of(*volume); }

// ---------------------------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------------------------

namespace {

[[noreturn]] void bad(const std::string& path, const std::string& what, const std::string& hint = {})
{
    fail("invalid_argument", "`" + path + "` " + what,
         hint.empty() ? std::string("See the selector grammar in the paint_apply description; every selector is an object with exactly one key.")
                      : hint);
}

std::string child(const std::string& path, const std::string& key) { return path + "." + key; }
std::string index(const std::string& path, size_t i) { return path + "[" + std::to_string(i) + "]"; }

const json& member(const json& obj, const char* key, const std::string& path)
{
    auto it = obj.find(key);
    if (it == obj.end() || it->is_null())
        bad(path, std::string("needs `") + key + "`.");
    return *it;
}

bool has(const json& obj, const char* key) { return obj.contains(key) && !obj[key].is_null(); }

void only_keys(const json& obj, const std::string& path, std::initializer_list<const char*> allowed)
{
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        bool ok = false;
        for (const char* key : allowed)
            ok = ok || it.key() == key;
        if (!ok) {
            std::string list;
            for (const char* key : allowed)
                list += (list.empty() ? "" : ", ") + std::string(key);
            bad(path, "has an unknown member `" + it.key() + "`. Accepted: " + list + ".");
        }
    }
}

double number(const json& value, const std::string& path)
{
    if (!value.is_number())
        bad(path, "must be a number (got " + value.dump() + ").");
    const double v = value.get<double>();
    if (!std::isfinite(v))
        bad(path, "must be finite.");
    return v;
}

double number(const json& obj, const char* key, const std::string& path)
{
    return number(member(obj, key, path), child(path, key));
}

double positive(const json& obj, const char* key, const std::string& path)
{
    const double v = number(obj, key, path);
    if (!(v > 0.))
        bad(child(path, key), "must be > 0 (got " + std::to_string(v) + ").");
    return v;
}

Vec3d vec3(const json& value, const std::string& path)
{
    if (!value.is_array() || value.size() != 3)
        bad(path, "must be [x, y, z] (got " + value.dump() + ").");
    return Vec3d(number(value[0], index(path, 0)), number(value[1], index(path, 1)), number(value[2], index(path, 2)));
}

Vec3d vec3(const json& obj, const char* key, const std::string& path) { return vec3(member(obj, key, path), child(path, key)); }

Vec3d direction(const json& obj, const char* key, const std::string& path)
{
    const Vec3d v = vec3(obj, key, path);
    if (v.norm() < 1e-12)
        bad(child(path, key), "must not be the zero vector.");
    return v;
}

FrameKind frame_of(const json& obj, const std::string& path)
{
    return obj.contains("frame") ? frame_from(obj["frame"], child(path, "frame")) : FrameKind::Plate;
}

const json& object_member(const json& value, const std::string& path)
{
    if (!value.is_object())
        bad(path, "must be an object (got " + value.dump() + ").");
    return value;
}

SeedSpec parse_seed(const json& value, const std::string& path, FrameKind default_frame, bool allow_bare_point)
{
    SeedSpec seed;
    seed.path = path;
    seed.frame = default_frame;
    if (allow_bare_point && value.is_array()) {
        seed.type = SeedSpec::Type::Point;
        seed.point = vec3(value, path);
        return seed;
    }
    if (!value.is_object())
        bad(path, "must be a seed: {\"point\": [x,y,z]}, {\"ray\": {\"origin\", \"direction\"}}, {\"facet\": n} or {\"pixel\": [u,v], \"camera\": {...}} (got " +
                      value.dump() + ").");
    if (has(value, "point")) {
        only_keys(value, path, { "point", "frame" });
        seed.type = SeedSpec::Type::Point;
        seed.point = vec3(value, "point", path);
        seed.frame = value.contains("frame") ? frame_from(value["frame"], child(path, "frame")) : default_frame;
    } else if (has(value, "ray")) {
        only_keys(value, path, { "ray", "frame" });
        const json& ray = object_member(value["ray"], child(path, "ray"));
        only_keys(ray, child(path, "ray"), { "origin", "direction" });
        seed.type = SeedSpec::Type::Ray;
        seed.point = vec3(ray, "origin", child(path, "ray"));
        seed.dir = direction(ray, "direction", child(path, "ray")).normalized();
        seed.frame = value.contains("frame") ? frame_from(value["frame"], child(path, "frame")) : default_frame;
    } else if (has(value, "facet")) {
        only_keys(value, path, { "facet", "part" });
        const json& f = value["facet"];
        if (!f.is_number_integer() || f.get<long long>() < 0)
            bad(child(path, "facet"), "must be a facet index >= 0 (got " + f.dump() + ").");
        seed.type = SeedSpec::Type::Facet;
        seed.facet = f.get<int>();
        if (has(value, "part")) {
            if (!value["part"].is_string())
                bad(child(path, "part"), "must be a part id v<N>.");
            seed.part = value["part"].get<std::string>();
        }
    } else if (has(value, "pixel")) {
        only_keys(value, path, { "pixel", "camera" });
        const json& px = value["pixel"];
        if (!px.is_array() || px.size() != 2)
            bad(child(path, "pixel"), "must be [u, v] (got " + px.dump() + ").");
        seed.type = SeedSpec::Type::Pixel;
        seed.pixel = Vec2d(number(px[0], index(child(path, "pixel"), 0)), number(px[1], index(child(path, "pixel"), 1)));
        const json& camera = member(value, "camera", path);
        pixel_ray(camera, seed.pixel, child(path, "camera"), seed.point, seed.dir);
        seed.frame = FrameKind::World;
    } else
        bad(path, "must have one of `point`, `ray`, `facet` or `pixel` (got " + value.dump() + ").");
    return seed;
}

void parse_into(SelectorSpec& out, const json& value, const std::string& path, int depth, bool top_level, bool top_and);

SelectorSpec parse_node(const json& value, const std::string& path, int depth, bool top_level, bool top_and)
{
    SelectorSpec out;
    parse_into(out, value, path, depth, top_level, top_and);
    return out;
}

void parse_into(SelectorSpec& out, const json& value, const std::string& path, int depth, bool top_level, bool top_and)
{
    using K = SelectorSpec::Kind;
    if (depth > 32)
        bad(path, "is nested too deeply (more than 32 levels).");
    if (!value.is_object())
        bad(path, "must be a selector object such as {\"all\": true} or {\"box\": {...}} (got " + value.dump() + ").");
    if (value.size() != 1) {
        std::string keys;
        for (auto it = value.begin(); it != value.end(); ++it)
            keys += (keys.empty() ? "" : ", ") + it.key();
        bad(path, "must have exactly one key (got " + std::to_string(value.size()) + (keys.empty() ? "" : ": " + keys) + ").",
            "Combine several conditions with {\"and\": [...]} or {\"or\": [...]}; put `frame` inside the leaf, e.g. {\"box\": {\"min\":..., \"max\":..., \"frame\": \"object\"}}.");
    }
    const std::string key = value.begin().key();
    const json&       v = value.begin().value();
    const std::string p = child(path, key);
    out.path = p;

    if (key == "all") {
        if (!v.is_boolean() || !v.get<bool>())
            bad(p, "must be true.");
        out.kind = K::All;
    } else if (key == "facets") {
        out.kind = K::Facets;
        const json* ids = &v;
        if (v.is_object()) {
            only_keys(v, p, { "ids", "part" });
            ids = &member(v, "ids", p);
            if (has(v, "part")) {
                if (!v["part"].is_string())
                    bad(child(p, "part"), "must be a part id v<N>.");
                out.part = v["part"].get<std::string>();
            }
        }
        if (!ids->is_array() || ids->empty())
            bad(p, "must be a non-empty array of facet indices, or {\"ids\": [...], \"part\": \"v<N>\"}.");
        for (size_t i = 0; i < ids->size(); ++i) {
            const json& id = (*ids)[i];
            if (!id.is_number_integer() || id.get<long long>() < 0 || id.get<long long>() > 2147483647LL)
                bad(index(p, i), "must be a facet index >= 0 (got " + id.dump() + ").");
            out.ids.push_back(id.get<int>());
        }
    } else if (key == "normal") {
        object_member(v, p);
        only_keys(v, p, { "direction", "max_angle_deg", "frame" });
        out.kind = K::Normal;
        out.a = direction(v, "direction", p);
        out.x = number(v, "max_angle_deg", p);
        if (out.x < 0. || out.x > 180.)
            bad(child(p, "max_angle_deg"), "must be within 0..180.");
        out.frame = frame_of(v, p);
    } else if (key == "overhang") {
        object_member(v, p);
        only_keys(v, p, { "threshold_deg", "up" });
        out.kind = K::Overhang;
        out.x = number(v, "threshold_deg", p);
        if (out.x < 0. || out.x > 90.)
            bad(child(p, "threshold_deg"), "must be within 0..90.");
        out.a = has(v, "up") ? direction(v, "up", p) : Vec3d::UnitZ();
        out.frame = FrameKind::Plate;
    } else if (key == "z_range") {
        object_member(v, p);
        only_keys(v, p, { "min", "max", "frame" });
        out.kind = K::ZRange;
        out.x = number(v, "min", p);
        out.y = number(v, "max", p);
        if (out.x > out.y)
            bad(p, "needs min <= max (got " + std::to_string(out.x) + " > " + std::to_string(out.y) + ").");
        out.frame = frame_of(v, p);
    } else if (key == "slab") {
        object_member(v, p);
        only_keys(v, p, { "normal", "min", "max", "frame" });
        out.kind = K::Slab;
        out.a = direction(v, "normal", p).normalized();
        out.x = number(v, "min", p);
        out.y = number(v, "max", p);
        if (out.x > out.y)
            bad(p, "needs min <= max.");
        out.frame = frame_of(v, p);
    } else if (key == "halfspace") {
        object_member(v, p);
        only_keys(v, p, { "point", "normal", "frame" });
        out.kind = K::HalfSpace;
        out.a = vec3(v, "point", p);
        out.b = direction(v, "normal", p);
        out.frame = frame_of(v, p);
    } else if (key == "box") {
        object_member(v, p);
        out.frame = frame_of(v, p);
        if (has(v, "center") || has(v, "size")) {
            only_keys(v, p, { "center", "size", "rotation_deg", "frame" });
            out.kind = K::OrientedBox;
            out.a = vec3(v, "center", p);
            out.b = vec3(v, "size", p);
            if ((out.b.array() < 0.).any())
                bad(child(p, "size"), "must not be negative.");
            const Vec3d r = has(v, "rotation_deg") ? Vec3d(vec3(v, "rotation_deg", p) * (M_PI / 180.)) : Vec3d(Vec3d::Zero());
            // As object rotations: about X, then Y, then Z (Geometry::rotation_transform).
            out.rotation = (Eigen::AngleAxisd(r.z(), Vec3d::UnitZ()) * Eigen::AngleAxisd(r.y(), Vec3d::UnitY()) *
                            Eigen::AngleAxisd(r.x(), Vec3d::UnitX()))
                               .toRotationMatrix();
        } else {
            only_keys(v, p, { "min", "max", "frame" });
            out.kind = K::Box;
            out.a = vec3(v, "min", p);
            out.b = vec3(v, "max", p);
            if ((out.a.array() > out.b.array()).any())
                bad(p, "needs min <= max in every coordinate.");
        }
    } else if (key == "sphere") {
        object_member(v, p);
        only_keys(v, p, { "center", "radius", "frame" });
        out.kind = K::Sphere;
        out.a = vec3(v, "center", p);
        out.x = positive(v, "radius", p);
        out.frame = frame_of(v, p);
    } else if (key == "cylinder") {
        object_member(v, p);
        only_keys(v, p, { "base", "axis", "radius", "height", "frame" });
        out.kind = K::Cylinder;
        out.a = vec3(v, "base", p);
        out.b = direction(v, "axis", p);
        out.x = positive(v, "radius", p);
        out.y = has(v, "height") ? positive(v, "height", p) : out.b.norm();
        out.frame = frame_of(v, p);
    } else if (key == "capsule") {
        object_member(v, p);
        only_keys(v, p, { "a", "b", "radius", "frame" });
        out.kind = K::Capsule;
        out.a = vec3(v, "a", p);
        out.b = vec3(v, "b", p);
        out.x = positive(v, "radius", p);
        out.frame = frame_of(v, p);
    } else if (key == "stroke") {
        object_member(v, p);
        only_keys(v, p, { "path", "radius", "frame", "connected_only" });
        out.kind = K::Stroke;
        out.frame = frame_of(v, p);
        out.x = positive(v, "radius", p);
        out.flag = has(v, "connected_only") ? v["connected_only"].get<bool>() : true;
        const json& points = member(v, "path", p);
        if (!points.is_array() || points.empty())
            bad(child(p, "path"), "must be a non-empty array of points [x,y,z] or seeds.");
        for (size_t i = 0; i < points.size(); ++i)
            out.seeds.push_back(parse_seed(points[i], index(child(p, "path"), i), out.frame, true));
    } else if (key == "connected" || key == "same_state") {
        object_member(v, p);
        only_keys(v, p, { "seed", "angle_deg" });
        out.kind = key == "connected" ? K::Connected : K::SameState;
        out.x = key == "connected" ? 30. : -1.;
        if (has(v, "angle_deg")) {
            out.x = number(v, "angle_deg", p);
            if (out.x < 0. || out.x > 180.)
                bad(child(p, "angle_deg"), "must be within 0..180.");
        }
        out.seeds.push_back(parse_seed(member(v, "seed", p), child(p, "seed"), FrameKind::Plate, false));
        if (key == "same_state" && !top_level && !top_and)
            bad(p, "(bucket fill) may only be the whole selector or an operand of a top-level \"and\".",
                "Write {\"and\": [{\"same_state\": ...}, <other conditions>]}.");
    } else if (key == "and" || key == "or") {
        out.kind = key == "and" ? K::And : K::Or;
        if (!v.is_array() || v.empty())
            bad(p, "must be a non-empty array of selectors.");
        for (size_t i = 0; i < v.size(); ++i)
            out.children.push_back(parse_node(v[i], index(p, i), depth + 1, false, top_level && key == "and"));
    } else if (key == "not") {
        out.kind = K::Not;
        out.children.push_back(parse_node(v, p, depth + 1, false, false));
    } else {
        bad(path, "has an unknown selector `" + key + "`.",
            "Selectors: all, facets, normal, overhang, z_range, slab, halfspace, box, sphere, cylinder, capsule, stroke, connected, "
            "same_state, and, or, not.");
    }
}

void collect_seeds(SelectorSpec& spec, std::vector<SeedSpec*>& out)
{
    for (SeedSpec& s : spec.seeds)
        out.push_back(&s);
    for (SelectorSpec& c : spec.children)
        collect_seeds(c, out);
}

void collect_seeds(const SelectorSpec& spec, std::vector<const SeedSpec*>& out)
{
    for (const SeedSpec& s : spec.seeds)
        out.push_back(&s);
    for (const SelectorSpec& c : spec.children)
        collect_seeds(c, out);
}

// Matrix from [[r0],[r1],[r2],[r3]] or 16 numbers, row-major.
bool read_matrix(const json& value, Matrix4d& out)
{
    if (!value.is_array())
        return false;
    if (value.size() == 4) {
        for (int r = 0; r < 4; ++r) {
            if (!value[r].is_array() || value[r].size() != 4)
                return false;
            for (int c = 0; c < 4; ++c) {
                if (!value[r][c].is_number())
                    return false;
                out(r, c) = value[r][c].get<double>();
            }
        }
        return true;
    }
    if (value.size() == 16) {
        for (int i = 0; i < 16; ++i) {
            if (!value[i].is_number())
                return false;
            out(i / 4, i % 4) = value[i].get<double>();
        }
        return true;
    }
    return false;
}

Vec3d face_normal(const indexed_triangle_set& its, int facet)
{
    const auto& f = its.indices[static_cast<size_t>(facet)];
    const Vec3d a = its.vertices[f[0]].cast<double>(), b = its.vertices[f[1]].cast<double>(), c = its.vertices[f[2]].cast<double>();
    return (b - a).cross(c - a).normalized();
}

Vec3d transform_normal(const Transform3d& m, const Vec3d& n)
{
    return (m.linear().inverse().transpose() * n).normalized();
}

} // namespace

SelectorSpec parse_selector(const json& value, const std::string& path)
{
    return parse_node(value, path, 0, true, false);
}

void pixel_ray(const json& camera, const Vec2d& pixel, const std::string& path, Vec3d& origin, Vec3d& direction)
{
    if (!camera.is_object())
        bad(path, "must be the `camera` object a screenshot returned.", "Pass back the whole `camera` member of the screenshot result unchanged.");
    Matrix4d view, projection;
    if (!camera.contains("view_matrix") || !read_matrix(camera["view_matrix"], view) || !camera.contains("projection_matrix") ||
        !read_matrix(camera["projection_matrix"], projection))
        bad(path, "needs `view_matrix` and `projection_matrix` (4x4, row-major) as a screenshot returns them.",
            "Pass back the whole `camera` member of the screenshot result unchanged.");
    if (!camera.contains("width") || !camera.contains("height") || !camera["width"].is_number() || !camera["height"].is_number())
        bad(path, "needs `width` and `height` (the picture size in pixels).", "Pass back the whole `camera` member of the screenshot result unchanged.");
    const double width = camera["width"].get<double>(), height = camera["height"].get<double>();
    if (!(width > 0.) || !(height > 0.))
        bad(path, "has a non-positive width or height.");
    if (pixel.x() < 0. || pixel.y() < 0. || pixel.x() > width || pixel.y() > height)
        fail("invalid_argument",
             "The pixel [" + std::to_string(pixel.x()) + ", " + std::to_string(pixel.y()) + "] is outside the picture (" + std::to_string(int(width)) + " x " +
                 std::to_string(int(height)) + ").",
             "Pixels are (u right, v down) from the top-left corner of the screenshot the camera belongs to.");
    // Same convention as the screenshot camera (McpCamera.cpp): no half-pixel offset.
    const double   ndc_x = 2. * pixel.x() / width - 1.;
    const double   ndc_y = 1. - 2. * pixel.y() / height;
    const Matrix4d pv = projection * view;
    if (std::abs(pv.determinant()) < 1e-300)
        bad(path, "has a singular view-projection matrix.");
    const Matrix4d  inverse = pv.inverse();
    Eigen::Vector4d near_point = inverse * Eigen::Vector4d(ndc_x, ndc_y, -1., 1.);
    Eigen::Vector4d far_point = inverse * Eigen::Vector4d(ndc_x, ndc_y, 1., 1.);
    near_point /= near_point.w();
    far_point /= far_point.w();
    origin = near_point.head<3>();
    direction = (far_point.head<3>() - near_point.head<3>()).normalized();
}

bool has_seeds(const SelectorSpec& spec)
{
    std::vector<const SeedSpec*> seeds;
    collect_seeds(spec, seeds);
    return !seeds.empty();
}

// ---------------------------------------------------------------------------------------------
// Seeds
// ---------------------------------------------------------------------------------------------

namespace {

// Seed position / direction in the plate frame of a target.
Vec3d to_plate_point(const PaintTarget& t, FrameKind frame, const Vec3d& p)
{
    switch (frame) {
    case FrameKind::Plate: return p;
    case FrameKind::World: return p - t.plate_origin;
    case FrameKind::Object: return t.instance_plate() * p;
    }
    return p;
}

Vec3d to_plate_dir(const PaintTarget& t, FrameKind frame, const Vec3d& d)
{
    return frame == FrameKind::Object ? Vec3d((t.instance_plate().linear() * d).normalized()) : d;
}

} // namespace

void resolve_seeds(SelectorSpec& spec, const std::vector<PaintTarget>& targets)
{
    // Facet selectors restricted to a part: the part must be one of the targets.
    std::function<void(const SelectorSpec&)> check_parts = [&](const SelectorSpec& s) {
        if (s.kind == SelectorSpec::Kind::Facets && !s.part.empty()) {
            bool found = false;
            for (const PaintTarget& t : targets)
                found = found || t.id() == s.part;
            if (!found)
                bad(child(s.path, "part"), "names " + s.part + ", which is not one of the painted parts.");
        }
        for (const SelectorSpec& c : s.children)
            check_parts(c);
    };
    check_parts(spec);

    std::vector<SeedSpec*> seeds;
    collect_seeds(spec, seeds);
    if (seeds.empty())
        return;

    std::map<const ModelVolume*, std::unique_ptr<AABBMesh>> trees;
    auto tree = [&trees](const ModelVolume* v) -> const AABBMesh& {
        auto& slot = trees[v];
        if (!slot)
            slot = std::make_unique<AABBMesh>(v->mesh().its);
        return *slot;
    };

    for (SeedSpec* seed : seeds) {
        std::optional<SeedHit> best;
        if (seed->type == SeedSpec::Type::Facet) {
            size_t which = targets.size();
            if (!seed->part.empty()) {
                for (size_t i = 0; i < targets.size(); ++i)
                    if (targets[i].id() == seed->part)
                        which = i;
                if (which == targets.size())
                    bad(child(seed->path, "part"), "names " + seed->part + ", which is not one of the painted parts.");
            } else {
                std::set<const ModelVolume*> volumes;
                for (const PaintTarget& t : targets)
                    volumes.insert(t.volume);
                if (volumes.size() != 1)
                    bad(seed->path, "is a facet index, but " + std::to_string(volumes.size()) + " parts are painted.",
                        "Add \"part\": \"v<N>\" to the seed, or target only that part.");
                which = 0;
            }
            const PaintTarget& t = targets[which];
            const indexed_triangle_set& its = t.volume->mesh().its;
            if (seed->facet >= static_cast<int>(its.indices.size()))
                bad(child(seed->path, "facet"), "is out of range: " + t.id() + " has " + std::to_string(its.indices.size()) + " facets.");
            const auto& f = its.indices[static_cast<size_t>(seed->facet)];
            const Vec3d c = (its.vertices[f[0]] + its.vertices[f[1]] + its.vertices[f[2]]).cast<double>() / 3.;
            const Transform3d m = t.instance_plate() * t.volume->get_matrix();
            SeedHit hit;
            hit.target = which;
            hit.facet = seed->facet;
            hit.point = m * c;
            hit.normal = transform_normal(m, face_normal(its, seed->facet));
            best = hit;
        } else if (seed->type == SeedSpec::Type::Point) {
            for (size_t i = 0; i < targets.size(); ++i) {
                const PaintTarget& t = targets[i];
                const Transform3d m = t.instance_plate() * t.volume->get_matrix();
                const Vec3d p = to_plate_point(t, seed->frame, seed->point);
                int facet = -1;
                Vec3d closest;
                tree(t.volume).squared_distance(m.inverse() * p, facet, closest);
                if (facet < 0)
                    continue;
                const Vec3d cp = m * closest;
                const double d = (cp - p).norm();
                if (!best || d < best->distance) {
                    SeedHit hit;
                    hit.target = i;
                    hit.facet = facet;
                    hit.point = cp;
                    hit.normal = transform_normal(m, face_normal(t.volume->mesh().its, facet));
                    hit.distance = d;
                    best = hit;
                }
            }
        } else {
            // Ray or pixel. A world ray (pixel, or frame world) is tested against every instance
            // of the object, since paint is shared by all instances and the picture shows them all.
            for (size_t i = 0; i < targets.size(); ++i) {
                const PaintTarget& t = targets[i];
                const Transform3d to_target = t.instance_plate() * t.volume->get_matrix();
                std::vector<Transform3d> placements; // plate frame of the target, per instance
                if (seed->frame == FrameKind::Object)
                    placements.push_back(to_target);
                else
                    for (const ModelInstance* inst : t.object->instances) {
                        Transform3d m = inst->get_matrix() * t.volume->get_matrix();
                        m.pretranslate(-t.plate_origin);
                        placements.push_back(m);
                    }
                const Vec3d o = to_plate_point(t, seed->frame, seed->point);
                const Vec3d d = to_plate_dir(t, seed->frame, seed->dir);
                for (const Transform3d& m : placements) {
                    const Transform3d inv = m.inverse();
                    const Vec3d       om = inv * o;
                    const Vec3d       dm = (inv.linear() * d).normalized();
                    const AABBMesh::hit_result h = tree(t.volume).query_ray_hit(om, dm);
                    if (!h.is_hit())
                        continue;
                    const Vec3d  hp = m * h.position();
                    const double dist = (hp - o).norm();
                    if (!best || dist < best->distance) {
                        SeedHit hit;
                        hit.target = i;
                        hit.facet = h.face();
                        // Where that facet point is for the target's own instance (the frame FacetPaint uses).
                        hit.point = to_target * h.position();
                        hit.normal = transform_normal(to_target, face_normal(t.volume->mesh().its, h.face()));
                        hit.distance = dist;
                        best = hit;
                    }
                }
            }
            if (!best)
                fail("seed_missed",
                     "`" + seed->path + "`: the " + (seed->type == SeedSpec::Type::Pixel ? std::string("pixel's ray") : std::string("ray")) +
                         " does not hit any painted part.",
                     seed->type == SeedSpec::Type::Pixel ?
                         "Check the pixel against the screenshot the camera came from (u right, v down from the top-left), and that the object at "
                         "that pixel is one of the targets." :
                         "Aim the ray at the part (origin outside, direction towards it) or use a point seed.");
        }
        if (!best)
            bad(seed->path, "could not be placed on any painted part.");
        seed->hit = best;
    }
}

json seeds_json(const SelectorSpec& spec, const std::vector<PaintTarget>& targets)
{
    std::vector<const SeedSpec*> seeds;
    collect_seeds(spec, seeds);
    json out = json::array();
    auto r = [](double v) { return std::round(v * 1e4) / 1e4; };
    for (const SeedSpec* s : seeds) {
        json e = json::object();
        e["path"] = s->path;
        e["type"] = s->type == SeedSpec::Type::Point ? "point" : s->type == SeedSpec::Type::Ray ? "ray" : s->type == SeedSpec::Type::Facet ? "facet" : "pixel";
        if (s->hit) {
            const PaintTarget& t = targets[s->hit->target];
            e["part"] = t.id();
            e["facet"] = s->hit->facet;
            e["point"] = json::array({ r(s->hit->point.x()), r(s->hit->point.y()), r(s->hit->point.z()) });
            e["normal"] = json::array({ r(s->hit->normal.x()), r(s->hit->normal.y()), r(s->hit->normal.z()) });
            if (s->type == SeedSpec::Type::Point)
                e["snap_distance_mm"] = r(s->hit->distance);
        }
        out.push_back(std::move(e));
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// Compile
// ---------------------------------------------------------------------------------------------

namespace {

using FacetPaint::Selector;

FacetPaint::Frame fp_frame(FrameKind f) { return f == FrameKind::Object ? FacetPaint::Frame::Object : FacetPaint::Frame::World; }

// A position of a leaf in the frame FacetPaint will see (plate is FacetPaint's world).
Vec3d leaf_point(const PaintTarget& t, FrameKind f, const Vec3d& p) { return f == FrameKind::World ? Vec3d(p - t.plate_origin) : p; }

Selector compile_node(const SelectorSpec& s, const std::vector<PaintTarget>& targets, size_t ti)
{
    using K = SelectorSpec::Kind;
    const PaintTarget&      t = targets[ti];
    const FacetPaint::Frame fr = fp_frame(s.frame);
    auto                    P = [&](const Vec3d& p) { return leaf_point(t, s.frame, p); };
    auto seed_here = [&](const SeedSpec& seed) { return seed.hit && seed.hit->target == ti; };
    // The seed's own facet is known: aim a short ray at the hit point from just outside that facet,
    // nudged towards its centroid, so a hit on an edge or corner cannot snap to a neighbour.
    auto fp_seed = [&](const SeedSpec& seed) {
        if (seed.type == SeedSpec::Type::Facet)
            return FacetPaint::Seed::on_facet(seed.hit->facet);
        const indexed_triangle_set& its = t.volume->mesh().its;
        const auto&       f = its.indices[static_cast<size_t>(seed.hit->facet)];
        const Transform3d m = t.instance_plate() * t.volume->get_matrix();
        const Vec3d       centroid = m * ((its.vertices[f[0]] + its.vertices[f[1]] + its.vertices[f[2]]).cast<double>() / 3.);
        const Vec3d       n = seed.hit->normal;
        const Vec3d       p = seed.hit->point + 0.01 * (centroid - seed.hit->point);
        return FacetPaint::Seed::along_ray(p + 0.05 * n, -n, FacetPaint::Frame::World);
    };
    switch (s.kind) {
    case K::All: return Selector::all();
    case K::Facets: {
        if (!s.part.empty() && s.part != t.id())
            return Selector::none();
        const size_t n = t.volume->mesh().its.indices.size();
        for (size_t i = 0; i < s.ids.size(); ++i)
            if (static_cast<size_t>(s.ids[i]) >= n)
                bad(index(s.path, i), "= " + std::to_string(s.ids[i]) + " is out of range: " + t.id() + " has " + std::to_string(n) + " facets.",
                    "Facet ids are indices into one part's triangles; with several parts, use {\"facets\": {\"ids\": [...], \"part\": \"v<N>\"}}.");
        return Selector::facets(s.ids);
    }
    case K::Normal: return Selector::normal(s.a, s.x, fr);
    case K::Overhang: return Selector::overhang(s.x, s.a, FacetPaint::Frame::World);
    case K::ZRange: {
        // A horizontal band in its frame: shift by the plate origin for "world".
        const double dz = s.frame == FrameKind::World ? t.plate_origin.z() : 0.;
        return Selector::slab(Vec3d::UnitZ(), s.x - dz, s.y - dz, fr);
    }
    case K::Slab: {
        const double d = s.frame == FrameKind::World ? s.a.dot(t.plate_origin) : 0.;
        return Selector::slab(s.a, s.x - d, s.y - d, fr);
    }
    case K::HalfSpace: return Selector::halfspace(P(s.a), s.b, fr);
    case K::Box: return Selector::box(P(s.a), P(s.b), fr);
    case K::OrientedBox: return Selector::oriented_box(P(s.a), s.rotation, s.b * 0.5, fr);
    case K::Sphere: return Selector::sphere(P(s.a), s.x, fr);
    case K::Cylinder: return Selector::cylinder(P(s.a), s.b, s.y, s.x, fr);
    case K::Capsule: return Selector::capsule(P(s.a), P(s.b), s.x, fr);
    case K::Stroke: {
        std::vector<FacetPaint::Seed> path;
        for (const SeedSpec& seed : s.seeds)
            if (seed_here(seed))
                path.push_back(fp_seed(seed));
        if (path.empty())
            return Selector::none();
        return Selector::stroke(std::move(path), s.x, s.flag);
    }
    case K::Connected:
        if (!seed_here(s.seeds.front()))
            return Selector::none();
        return Selector::connected(fp_seed(s.seeds.front()), s.x);
    case K::SameState:
        if (!seed_here(s.seeds.front()))
            return Selector::none();
        return Selector::same_state(fp_seed(s.seeds.front()), s.x);
    case K::And:
    case K::Or: {
        Selector out = compile_node(s.children.front(), targets, ti);
        for (size_t i = 1; i < s.children.size(); ++i)
            out = s.kind == K::And ? (std::move(out) && compile_node(s.children[i], targets, ti)) : (std::move(out) || compile_node(s.children[i], targets, ti));
        return out;
    }
    case K::Not: return !compile_node(s.children.front(), targets, ti);
    }
    return Selector::all();
}

} // namespace

FacetPaint::Selector compile(const SelectorSpec& spec, const std::vector<PaintTarget>& targets, size_t target)
{
    try {
        return compile_node(spec, targets, target);
    } catch (const Slic3r::InvalidArgument& e) {
        fail("invalid_argument", std::string("`selector`: ") + e.what() + ".");
    }
}

// ---------------------------------------------------------------------------------------------
// Schema
// ---------------------------------------------------------------------------------------------

const char* selector_defs_json()
{
    return R"JSON({
  "Selector": {"type": "object",
    "description": "What to paint: an object with exactly ONE key. Leaves: all, facets, normal, overhang, z_range, slab, halfspace, box, sphere, cylinder, capsule, stroke, connected (smart fill), same_state (bucket fill). Combine with and / or / not. Positions take `frame` (plate | world | object, default plate).",
    "properties": {
      "all": {"const": true, "description": "Every facet."},
      "facets": {"description": "Original facets by index (as mesh_query/paint_get report them). With several parts: {\"ids\": [...], \"part\": \"v<N>\"}.",
                 "oneOf": [{"type": "array", "items": {"type": "integer", "minimum": 0}, "minItems": 1},
                           {"type": "object", "properties": {"ids": {"type": "array", "items": {"type": "integer", "minimum": 0}, "minItems": 1}, "part": {"$ref": "#/$defs/Id"}},
                            "required": ["ids"], "additionalProperties": false}]},
      "normal": {"type": "object", "description": "Facets whose outward normal is within max_angle_deg of direction ([0,0,1], 0 = faces pointing straight up).",
                 "properties": {"direction": {"$ref": "#/$defs/Vec3"}, "max_angle_deg": {"type": "number", "minimum": 0, "maximum": 180}, "frame": {"$ref": "#/$defs/Frame"}},
                 "required": ["direction", "max_angle_deg"], "additionalProperties": false},
      "overhang": {"type": "object", "description": "Downward-facing facets whose normal is within threshold_deg of straight down - the support gizmo's 'by angle' rule (threshold measured from the horizontal, as support_threshold_angle). Whole facets.",
                   "properties": {"threshold_deg": {"type": "number", "minimum": 0, "maximum": 90}, "up": {"$ref": "#/$defs/Vec3", "description": "Default [0,0,1]."}},
                   "required": ["threshold_deg"], "additionalProperties": false},
      "z_range": {"type": "object", "description": "Horizontal band min <= z <= max (sharp boundaries).",
                  "properties": {"min": {"type": "number"}, "max": {"type": "number"}, "frame": {"$ref": "#/$defs/Frame"}}, "required": ["min", "max"], "additionalProperties": false},
      "slab": {"type": "object", "description": "Between two parallel planes: min <= p . unit(normal) <= max.",
               "properties": {"normal": {"$ref": "#/$defs/Vec3"}, "min": {"type": "number"}, "max": {"type": "number"}, "frame": {"$ref": "#/$defs/Frame"}},
               "required": ["normal", "min", "max"], "additionalProperties": false},
      "halfspace": {"type": "object", "description": "The side of the plane through point that normal points to.",
                    "properties": {"point": {"$ref": "#/$defs/Vec3"}, "normal": {"$ref": "#/$defs/Vec3"}, "frame": {"$ref": "#/$defs/Frame"}},
                    "required": ["point", "normal"], "additionalProperties": false},
      "box": {"type": "object", "description": "Axis-aligned {min, max}, or oriented {center, size, rotation_deg [x,y,z]}.",
              "properties": {"min": {"$ref": "#/$defs/Vec3"}, "max": {"$ref": "#/$defs/Vec3"}, "center": {"$ref": "#/$defs/Vec3"}, "size": {"$ref": "#/$defs/Vec3"},
                             "rotation_deg": {"$ref": "#/$defs/Vec3"}, "frame": {"$ref": "#/$defs/Frame"}}, "additionalProperties": false},
      "sphere": {"type": "object", "properties": {"center": {"$ref": "#/$defs/Vec3"}, "radius": {"type": "number", "exclusiveMinimum": 0}, "frame": {"$ref": "#/$defs/Frame"}},
                 "required": ["center", "radius"], "additionalProperties": false},
      "cylinder": {"type": "object", "description": "Finite cylinder from base along axis; height defaults to the axis length.",
                   "properties": {"base": {"$ref": "#/$defs/Vec3"}, "axis": {"$ref": "#/$defs/Vec3"}, "radius": {"type": "number", "exclusiveMinimum": 0},
                                  "height": {"type": "number", "exclusiveMinimum": 0}, "frame": {"$ref": "#/$defs/Frame"}},
                   "required": ["base", "axis", "radius"], "additionalProperties": false},
      "capsule": {"type": "object", "description": "Points within radius of the segment a-b.",
                  "properties": {"a": {"$ref": "#/$defs/Vec3"}, "b": {"$ref": "#/$defs/Vec3"}, "radius": {"type": "number", "exclusiveMinimum": 0}, "frame": {"$ref": "#/$defs/Frame"}},
                  "required": ["a", "b", "radius"], "additionalProperties": false},
      "stroke": {"type": "object", "description": "A brush stroke: a ball of radius dragged along path (points [x,y,z] snapped to the surface, or seeds). connected_only (default true) keeps it on the surface the stroke touches, like the GUI brush.",
                 "properties": {"path": {"type": "array", "minItems": 1, "items": {"anyOf": [{"$ref": "#/$defs/Vec3"}, {"$ref": "#/$defs/Seed"}]}},
                                "radius": {"type": "number", "exclusiveMinimum": 0}, "frame": {"$ref": "#/$defs/Frame"}, "connected_only": {"type": "boolean", "default": true}},
                 "required": ["path", "radius"], "additionalProperties": false},
      "connected": {"type": "object", "description": "Smart fill: the smooth surface around the seed, stopping at edges sharper than angle_deg (default 30). Whole facets.",
                    "properties": {"seed": {"$ref": "#/$defs/Seed"}, "angle_deg": {"type": "number", "minimum": 0, "maximum": 180}}, "required": ["seed"], "additionalProperties": false},
      "same_state": {"type": "object", "description": "Bucket fill: the connected region that currently has the seed's paint state. Only as the whole selector or an operand of a top-level and.",
                     "properties": {"seed": {"$ref": "#/$defs/Seed"}, "angle_deg": {"type": "number", "minimum": 0, "maximum": 180, "description": "Also stop at sharper edges (default: no edge detection)."}},
                     "required": ["seed"], "additionalProperties": false},
      "and": {"type": "array", "items": {"$ref": "#/$defs/Selector"}, "minItems": 1},
      "or": {"type": "array", "items": {"$ref": "#/$defs/Selector"}, "minItems": 1},
      "not": {"$ref": "#/$defs/Selector"}
    },
    "additionalProperties": false},
  "Seed": {"type": "object",
    "description": "A point on the surface: {\"point\": [x,y,z], \"frame\"?} (snapped to the closest surface point), {\"ray\": {\"origin\", \"direction\"}, \"frame\"?} (first hit), {\"facet\": n, \"part\"?: \"v<N>\"} (facet centroid), or {\"pixel\": [u,v], \"camera\": <the camera object a screenshot returned>} (first hit of the ray through that pixel).",
    "properties": {"point": {"$ref": "#/$defs/Vec3"}, "frame": {"$ref": "#/$defs/Frame"},
                   "ray": {"type": "object", "properties": {"origin": {"$ref": "#/$defs/Vec3"}, "direction": {"$ref": "#/$defs/Vec3"}}, "required": ["origin", "direction"], "additionalProperties": false},
                   "facet": {"type": "integer", "minimum": 0}, "part": {"$ref": "#/$defs/Id"},
                   "pixel": {"$ref": "#/$defs/Vec2"}, "camera": {"type": "object"}},
    "additionalProperties": false}
})JSON";
}

}}}} // namespace Slic3r::GUI::Mcp::Paint
