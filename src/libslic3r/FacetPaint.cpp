#include "FacetPaint.hpp"

#include "AABBTreeIndirect.hpp"
#include "Exception.hpp"
#include "Geometry.hpp"
#include "Model.hpp"
#include "TriangleMesh.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <queue>
#include <utility>

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

namespace Slic3r {
namespace FacetPaint {

using Overlap = TriangleSelector::RegionOverlap;

Transform3d Transforms::frame_from_mesh(Frame frame) const
{
    switch (frame) {
    case Frame::Mesh: return Transform3d::Identity();
    case Frame::Object: return this->object_from_mesh();
    case Frame::World:
    default: return this->world_from_mesh();
    }
}

// ------------------------------------------------------------------------------------------------
// Selector construction
// ------------------------------------------------------------------------------------------------

static void require(bool condition, const char *message)
{
    if (! condition)
        throw InvalidArgument(std::string("FacetPaint: ") + message);
}

static Vec3d unit(const Vec3d &v, const char *what)
{
    const double n = v.norm();
    require(std::isfinite(n) && n > 1e-12, what);
    return v / n;
}

static bool finite(const Vec3d &v) { return std::isfinite(v.x()) && std::isfinite(v.y()) && std::isfinite(v.z()); }

static Selector make(Selector::Type type, Frame frame)
{
    Selector s;
    s.m_type  = type;
    s.m_frame = frame;
    return s;
}

Selector Selector::all() { return make(Type::All, Frame::Mesh); }

Selector Selector::facets(std::vector<int> ids)
{
    Selector s = make(Type::Facets, Frame::Mesh);
    s.m_ids    = std::move(ids);
    return s;
}

Selector Selector::normal(const Vec3d &direction, double max_angle_deg, Frame frame)
{
    require(std::isfinite(max_angle_deg) && max_angle_deg >= 0. && max_angle_deg <= 180., "normal: the angle must be within 0..180 degrees");
    Selector s = make(Type::Normal, frame);
    s.m_v1     = unit(direction, "normal: the direction must not be zero");
    s.m_a      = max_angle_deg;
    return s;
}

Selector Selector::overhang(double threshold_deg, const Vec3d &up, Frame frame)
{
    require(std::isfinite(threshold_deg) && threshold_deg >= 0. && threshold_deg <= 90., "overhang: the threshold must be within 0..90 degrees");
    return normal(-unit(up, "overhang: the up direction must not be zero"), threshold_deg, frame);
}

Selector Selector::halfspace(const Vec3d &point, const Vec3d &normal, Frame frame)
{
    require(finite(point), "halfspace: the point must be finite");
    Selector s = make(Type::HalfSpace, frame);
    s.m_v1     = unit(normal, "halfspace: the normal must not be zero");
    s.m_a      = s.m_v1.dot(point);
    return s;
}

Selector Selector::slab(const Vec3d &normal, double min, double max, Frame frame)
{
    require(std::isfinite(min) && std::isfinite(max) && min <= max, "slab: min must not exceed max");
    const double n = normal.norm();
    require(std::isfinite(n) && n > 1e-12, "slab: the normal must not be zero");
    Selector s = make(Type::Slab, frame);
    s.m_v1     = normal / n;
    // The planes are p . normal = min / max for the normal as given; keep them where they are.
    s.m_a = min / n;
    s.m_b = max / n;
    return s;
}

Selector Selector::box(const Vec3d &min, const Vec3d &max, Frame frame)
{
    require(finite(min) && finite(max) && (min.array() <= max.array()).all(), "box: min must not exceed max");
    return oriented_box(0.5 * (min + max), Matrix3d::Identity(), 0.5 * (max - min), frame);
}

Selector Selector::oriented_box(const Vec3d &center, const Matrix3d &rotation, const Vec3d &half_size, Frame frame)
{
    require(finite(center) && finite(half_size) && (half_size.array() >= 0.).all(), "box: the size must not be negative");
    Selector s = make(Type::Box, frame);
    s.m_v1     = center;
    s.m_v2     = half_size;
    // Orthonormalize, so that a slightly inexact rotation still describes a box.
    Matrix3d r;
    r.col(0) = unit(rotation.col(0), "box: degenerate rotation");
    r.col(1) = unit(rotation.col(1) - rotation.col(1).dot(r.col(0)) * r.col(0), "box: degenerate rotation");
    r.col(2) = r.col(0).cross(r.col(1));
    s.m_rotation = r;
    return s;
}

Selector Selector::sphere(const Vec3d &center, double radius, Frame frame)
{
    require(finite(center) && std::isfinite(radius) && radius > 0., "sphere: the radius must be positive");
    Selector s = make(Type::Sphere, frame);
    s.m_v1     = center;
    s.m_a      = radius;
    return s;
}

Selector Selector::cylinder(const Vec3d &base, const Vec3d &axis, double height, double radius, Frame frame)
{
    require(finite(base) && std::isfinite(height) && height > 0. && std::isfinite(radius) && radius > 0.,
            "cylinder: the height and radius must be positive");
    Selector s = make(Type::Cylinder, frame);
    s.m_v1     = base;
    s.m_v2     = unit(axis, "cylinder: the axis must not be zero");
    s.m_a      = height;
    s.m_b      = radius;
    return s;
}

Selector Selector::capsule(const Vec3d &a, const Vec3d &b, double radius, Frame frame)
{
    require(finite(a) && finite(b) && std::isfinite(radius) && radius > 0., "capsule: the radius must be positive");
    Selector s = make(Type::Capsule, frame);
    s.m_v1     = a;
    s.m_v2     = b;
    s.m_a      = radius;
    return s;
}

Selector Selector::stroke(std::vector<Seed> path, double radius, bool connected_only)
{
    require(! path.empty(), "stroke: the path is empty");
    require(std::isfinite(radius) && radius > 0., "stroke: the radius must be positive");
    Selector s = make(Type::Stroke, Frame::World);
    s.m_seeds  = std::move(path);
    s.m_a      = radius;
    s.m_flag   = connected_only;
    return s;
}

Selector Selector::connected(const Seed &seed, double angle_deg)
{
    require(std::isfinite(angle_deg) && angle_deg >= 0. && angle_deg <= 180., "connected: the angle must be within 0..180 degrees");
    Selector s = make(Type::Connected, Frame::World);
    s.m_seeds  = {seed};
    s.m_a      = angle_deg;
    return s;
}

Selector Selector::same_state(const Seed &seed, double angle_deg)
{
    require(std::isfinite(angle_deg) && angle_deg <= 180., "same_state: the angle must not exceed 180 degrees");
    Selector s = make(Type::SameState, Frame::World);
    s.m_seeds  = {seed};
    s.m_a      = angle_deg;
    return s;
}

static Selector combine(Selector::Type type, Selector lhs, Selector rhs)
{
    Selector s = make(type, Frame::Mesh);
    for (Selector *operand : {&lhs, &rhs}) {
        if (operand->m_type == type)
            for (Selector &child : operand->m_children)
                s.m_children.emplace_back(std::move(child));
        else
            s.m_children.emplace_back(std::move(*operand));
    }
    return s;
}

Selector operator&&(Selector lhs, Selector rhs) { return combine(Selector::Type::And, std::move(lhs), std::move(rhs)); }
Selector operator||(Selector lhs, Selector rhs) { return combine(Selector::Type::Or, std::move(lhs), std::move(rhs)); }
Selector operator!(Selector operand)
{
    if (operand.m_type == Selector::Type::Not)
        return std::move(operand.m_children.front());
    Selector s = make(Selector::Type::Not, Frame::Mesh);
    s.m_children.emplace_back(std::move(operand));
    return s;
}

const StateStats *Stats::find(EnforcerBlockerType state) const
{
    for (const StateStats &s : states)
        if (s.state == state)
            return &s;
    return nullptr;
}

// ------------------------------------------------------------------------------------------------
// Geometry
// ------------------------------------------------------------------------------------------------

namespace {

using Tri = std::array<Vec3d, 3>;

Vec3d centroid(const Tri &t) { return (t[0] + t[1] + t[2]) / 3.; }

// Radius of a ball around the centroid that holds the triangle.
double bounding_radius(const Tri &t, const Vec3d &c)
{
    return std::sqrt(std::max({(t[0] - c).squaredNorm(), (t[1] - c).squaredNorm(), (t[2] - c).squaredNorm()}));
}

// Closest point on a triangle, Ericson, Real-Time Collision Detection, 5.1.5.
Vec3d closest_point_on_triangle(const Vec3d &p, const Vec3d &a, const Vec3d &b, const Vec3d &c)
{
    const Vec3d ab = b - a, ac = c - a, ap = p - a;
    const double d1 = ab.dot(ap), d2 = ac.dot(ap);
    if (d1 <= 0. && d2 <= 0.)
        return a;
    const Vec3d  bp = p - b;
    const double d3 = ab.dot(bp), d4 = ac.dot(bp);
    if (d3 >= 0. && d4 <= d3)
        return b;
    const double vc = d1 * d4 - d3 * d2;
    if (vc <= 0. && d1 >= 0. && d3 <= 0.)
        return a + (d1 / (d1 - d3)) * ab;
    const Vec3d  cp = p - c;
    const double d5 = ab.dot(cp), d6 = ac.dot(cp);
    if (d6 >= 0. && d5 <= d6)
        return c;
    const double vb = d5 * d2 - d1 * d6;
    if (vb <= 0. && d2 >= 0. && d6 <= 0.)
        return a + (d2 / (d2 - d6)) * ac;
    const double va = d3 * d6 - d5 * d4;
    if (va <= 0. && (d4 - d3) >= 0. && (d5 - d6) >= 0.)
        return b + ((d4 - d3) / ((d4 - d3) + (d5 - d6))) * (c - b);
    const double denom = va + vb + vc;
    if (std::abs(denom) < 1e-300)
        return a; // Degenerate triangle.
    const double v = vb / denom, w = vc / denom;
    return a + ab * v + ac * w;
}

double distance_to_segment(const Vec3d &p, const Vec3d &a, const Vec3d &b)
{
    const Vec3d  ab = b - a;
    const double l2 = ab.squaredNorm();
    const double t  = l2 > 0. ? std::clamp((p - a).dot(ab) / l2, 0., 1.) : 0.;
    return (a + t * ab - p).norm();
}

// Separating axis test of a triangle against the box [-h, h], Akenine-Moeller.
bool triangle_overlaps_box(const Vec3d &h, const Vec3d &v0, const Vec3d &v1, const Vec3d &v2)
{
    const std::array<Vec3d, 3> e = {v1 - v0, v2 - v1, v0 - v2};
    for (int i = 0; i < 3; ++i)
        for (const Vec3d &edge : e) {
            const Vec3d  axis = Vec3d::Unit(i).cross(edge);
            const double p0 = axis.dot(v0), p1 = axis.dot(v1), p2 = axis.dot(v2);
            const double r  = h.dot(axis.cwiseAbs());
            if (std::min({p0, p1, p2}) > r || std::max({p0, p1, p2}) < -r)
                return false;
        }
    for (int i = 0; i < 3; ++i)
        if (std::min({v0(i), v1(i), v2(i)}) > h(i) || std::max({v0(i), v1(i), v2(i)}) < -h(i))
            return false;
    const Vec3d n = e[0].cross(e[1]);
    return std::abs(n.dot(v0)) <= h.dot(n.cwiseAbs());
}

// A solid in the frame of its selector.
class Shape
{
public:
    virtual ~Shape() = default;
    virtual bool    contains(const Vec3d &p) const = 0;
    virtual Overlap classify(const Tri &t) const = 0;
};

// A convex solid: inside if all corners are; outside if a lower bound of the distance from the
// centroid exceeds the triangle's bounding radius, otherwise partial.
class ConvexShape : public Shape
{
public:
    Overlap classify(const Tri &t) const override
    {
        const int inside = int(this->contains(t[0])) + int(this->contains(t[1])) + int(this->contains(t[2]));
        if (inside == 3)
            return Overlap::Inside;
        if (inside > 0)
            return Overlap::Partial;
        return this->separated(t) ? Overlap::Outside : Overlap::Partial;
    }

protected:
    // Exact or conservative (true only if the triangle is certainly outside).
    virtual bool separated(const Tri &t) const
    {
        const Vec3d c = centroid(t);
        return this->distance_lower_bound(c) > bounding_radius(t, c);
    }
    virtual double distance_lower_bound(const Vec3d &p) const = 0;
};

class HalfSpaceShape : public Shape
{
public:
    HalfSpaceShape(const Vec3d &n, double d) : m_n(n), m_d(d) {}
    bool    contains(const Vec3d &p) const override { return m_n.dot(p) >= m_d; }
    Overlap classify(const Tri &t) const override
    {
        const int inside = int(this->contains(t[0])) + int(this->contains(t[1])) + int(this->contains(t[2]));
        return inside == 3 ? Overlap::Inside : inside == 0 ? Overlap::Outside : Overlap::Partial;
    }

private:
    Vec3d  m_n;
    double m_d;
};

class SlabShape : public Shape
{
public:
    SlabShape(const Vec3d &n, double min, double max) : m_n(n), m_min(min), m_max(max) {}
    bool    contains(const Vec3d &p) const override { const double d = m_n.dot(p); return d >= m_min && d <= m_max; }
    Overlap classify(const Tri &t) const override
    {
        const std::array<double, 3> d = {m_n.dot(t[0]), m_n.dot(t[1]), m_n.dot(t[2])};
        const double lo = std::min({d[0], d[1], d[2]}), hi = std::max({d[0], d[1], d[2]});
        if (lo >= m_min && hi <= m_max)
            return Overlap::Inside;
        if (hi < m_min || lo > m_max)
            return Overlap::Outside;
        return Overlap::Partial;
    }

private:
    Vec3d  m_n;
    double m_min, m_max;
};

class BoxShape : public ConvexShape
{
public:
    BoxShape(const Vec3d &center, const Matrix3d &rotation, const Vec3d &half) : m_center(center), m_rot_t(rotation.transpose()), m_half(half) {}
    bool contains(const Vec3d &p) const override { return (this->local(p).cwiseAbs().array() <= m_half.array()).all(); }

protected:
    bool separated(const Tri &t) const override { return ! triangle_overlaps_box(m_half, this->local(t[0]), this->local(t[1]), this->local(t[2])); }
    double distance_lower_bound(const Vec3d &p) const override { return (this->local(p).cwiseAbs() - m_half).cwiseMax(0.).norm(); }

private:
    Vec3d local(const Vec3d &p) const { return m_rot_t * (p - m_center); }
    Vec3d    m_center;
    Matrix3d m_rot_t;
    Vec3d    m_half;
};

class SphereShape : public ConvexShape
{
public:
    SphereShape(const Vec3d &center, double radius) : m_center(center), m_radius(radius) {}
    bool contains(const Vec3d &p) const override { return (p - m_center).squaredNorm() <= m_radius * m_radius; }

protected:
    bool separated(const Tri &t) const override
    {
        return (closest_point_on_triangle(m_center, t[0], t[1], t[2]) - m_center).squaredNorm() > m_radius * m_radius;
    }
    double distance_lower_bound(const Vec3d &p) const override { return (p - m_center).norm() - m_radius; }

private:
    Vec3d  m_center;
    double m_radius;
};

class CapsuleShape : public ConvexShape
{
public:
    CapsuleShape(const Vec3d &a, const Vec3d &b, double radius) : m_a(a), m_b(b), m_radius(radius) {}
    bool contains(const Vec3d &p) const override { return distance_to_segment(p, m_a, m_b) <= m_radius; }

protected:
    double distance_lower_bound(const Vec3d &p) const override { return distance_to_segment(p, m_a, m_b) - m_radius; }

private:
    Vec3d  m_a, m_b;
    double m_radius;
};

class CylinderShape : public ConvexShape
{
public:
    CylinderShape(const Vec3d &base, const Vec3d &axis, double height, double radius)
        : m_base(base), m_axis(axis), m_height(height), m_radius(radius) {}
    bool contains(const Vec3d &p) const override
    {
        const Vec2d d = this->distances(p);
        return d.x() <= 0. && d.y() <= 0.;
    }

protected:
    bool separated(const Tri &t) const override
    {
        // Wholly below the base or above the top: exact.
        const std::array<double, 3> h = {m_axis.dot(t[0] - m_base), m_axis.dot(t[1] - m_base), m_axis.dot(t[2] - m_base)};
        if (std::max({h[0], h[1], h[2]}) < 0. || std::min({h[0], h[1], h[2]}) > m_height)
            return true;
        return ConvexShape::separated(t);
    }
    // Exact distance to a capped cylinder (outside).
    double distance_lower_bound(const Vec3d &p) const override
    {
        const Vec2d d = this->distances(p);
        return std::min(std::max(d.x(), d.y()), 0.) + d.cwiseMax(0.).norm();
    }

private:
    // (radial distance - radius, axial distance from the mid height - height / 2)
    Vec2d distances(const Vec3d &p) const
    {
        const Vec3d  v      = p - m_base;
        const double along  = m_axis.dot(v);
        const double radial = (v - along * m_axis).norm();
        return {radial - m_radius, std::abs(along - 0.5 * m_height) - 0.5 * m_height};
    }
    Vec3d  m_base, m_axis;
    double m_height, m_radius;
};

// ------------------------------------------------------------------------------------------------
// Compiled selector tree, evaluated in the mesh frame.
// ------------------------------------------------------------------------------------------------

class Node
{
public:
    virtual ~Node() = default;
    virtual Overlap classify(const std::array<Vec3f, 3> &t, int facet) const = 0;
    virtual bool    contains(const Vec3f &p, int facet) const = 0;
};
using NodePtr = std::unique_ptr<Node>;

class ConstNode : public Node
{
public:
    explicit ConstNode(bool value) : m_value(value) {}
    Overlap classify(const std::array<Vec3f, 3> &, int) const override { return m_value ? Overlap::Inside : Overlap::Outside; }
    bool    contains(const Vec3f &, int) const override { return m_value; }

private:
    bool m_value;
};

// Whole original facets.
class FacetMaskNode : public Node
{
public:
    explicit FacetMaskNode(std::vector<uint8_t> mask) : m_mask(std::move(mask)) {}
    Overlap classify(const std::array<Vec3f, 3> &, int facet) const override { return m_mask[facet] ? Overlap::Inside : Overlap::Outside; }
    bool    contains(const Vec3f &, int facet) const override { return m_mask[facet] != 0; }

private:
    std::vector<uint8_t> m_mask;
};

class ShapeNode : public Node
{
public:
    ShapeNode(const Transform3d &frame_from_mesh, std::unique_ptr<Shape> shape) : m_trafo(frame_from_mesh), m_shape(std::move(shape)) {}
    Overlap classify(const std::array<Vec3f, 3> &t, int) const override
    {
        // An affine map takes a triangle to a triangle, so classifying in the selector's frame is exact.
        return m_shape->classify({m_trafo * t[0].cast<double>(), m_trafo * t[1].cast<double>(), m_trafo * t[2].cast<double>()});
    }
    bool contains(const Vec3f &p, int) const override { return m_shape->contains(m_trafo * p.cast<double>()); }

private:
    Transform3d            m_trafo;
    std::unique_ptr<Shape> m_shape;
};

class AndNode : public Node
{
public:
    explicit AndNode(std::vector<NodePtr> children) : m_children(std::move(children)) {}
    Overlap classify(const std::array<Vec3f, 3> &t, int facet) const override
    {
        Overlap out = Overlap::Inside;
        for (const NodePtr &child : m_children)
            if (Overlap o = child->classify(t, facet); o == Overlap::Outside)
                return Overlap::Outside;
            else if (o == Overlap::Partial)
                out = Overlap::Partial;
        return out;
    }
    bool contains(const Vec3f &p, int facet) const override
    {
        return std::all_of(m_children.begin(), m_children.end(), [&](const NodePtr &c) { return c->contains(p, facet); });
    }

private:
    std::vector<NodePtr> m_children;
};

class OrNode : public Node
{
public:
    explicit OrNode(std::vector<NodePtr> children) : m_children(std::move(children)) {}
    Overlap classify(const std::array<Vec3f, 3> &t, int facet) const override
    {
        Overlap out = Overlap::Outside;
        for (const NodePtr &child : m_children)
            if (Overlap o = child->classify(t, facet); o == Overlap::Inside)
                return Overlap::Inside;
            else if (o == Overlap::Partial)
                out = Overlap::Partial;
        return out;
    }
    bool contains(const Vec3f &p, int facet) const override
    {
        return std::any_of(m_children.begin(), m_children.end(), [&](const NodePtr &c) { return c->contains(p, facet); });
    }

private:
    std::vector<NodePtr> m_children;
};

class NotNode : public Node
{
public:
    explicit NotNode(NodePtr child) : m_child(std::move(child)) {}
    Overlap classify(const std::array<Vec3f, 3> &t, int facet) const override
    {
        const Overlap o = m_child->classify(t, facet);
        return o == Overlap::Inside ? Overlap::Outside : o == Overlap::Outside ? Overlap::Inside : Overlap::Partial;
    }
    bool contains(const Vec3f &p, int facet) const override { return ! m_child->contains(p, facet); }

private:
    NodePtr m_child;
};

// The Region handed to TriangleSelector::select_region().
class RegionAdapter : public TriangleSelector::Region
{
public:
    RegionAdapter(const Node &node, const indexed_triangle_set &its, bool whole_facets) : m_node(node), m_its(its), m_whole_facets(whole_facets) {}

    Overlap classify(const Vec3f &a, const Vec3f &b, const Vec3f &c, int facet) const override
    {
        if (m_whole_facets)
            return m_node.contains(this->facet_centroid(facet), facet) ? Overlap::Inside : Overlap::Outside;
        return m_node.classify({a, b, c}, facet);
    }
    bool contains(const Vec3f &p, int facet) const override
    {
        return m_whole_facets ? m_node.contains(this->facet_centroid(facet), facet) : m_node.contains(p, facet);
    }

private:
    Vec3f facet_centroid(int facet) const
    {
        const stl_triangle_vertex_indices &f = m_its.indices[facet];
        return (m_its.vertices[f[0]] + m_its.vertices[f[1]] + m_its.vertices[f[2]]) / 3.f;
    }
    const Node                 &m_node;
    const indexed_triangle_set &m_its;
    bool                        m_whole_facets;
};

struct SurfacePoint
{
    int   facet;
    Vec3f point; // mesh frame
};

// Everything a selector needs to know about the mesh; the expensive parts are built on demand.
class Context
{
public:
    Context(const TriangleMesh &mesh, const Transforms &trafo) : m_mesh(mesh), m_trafo(trafo) {}

    const indexed_triangle_set &its() const { return m_mesh.its; }
    const Transforms           &trafo() const { return m_trafo; }
    size_t                      num_facets() const { return m_mesh.its.indices.size(); }

    const std::vector<Vec3i32> &neighbors()
    {
        if (m_neighbors.empty() && ! m_mesh.its.indices.empty())
            m_neighbors = its_face_neighbors(m_mesh.its);
        return m_neighbors;
    }

    // Outward facet normals in a frame (normalized; zero for a degenerate facet).
    std::vector<Vec3d> normals(Frame frame) const
    {
        const Matrix3d     n_mat = m_trafo.frame_from_mesh(frame).linear().inverse().transpose();
        std::vector<Vec3d> out(this->num_facets());
        tbb::parallel_for(tbb::blocked_range<size_t>(0, out.size()), [&](const tbb::blocked_range<size_t> &range) {
            for (size_t i = range.begin(); i < range.end(); ++i) {
                const Vec3d n = n_mat * its_face_normal(m_mesh.its, int(i)).cast<double>();
                const double l = n.norm();
                out[i]         = l > 0. && std::isfinite(l) ? Vec3d(n / l) : Vec3d::Zero();
            }
        });
        return out;
    }

    SurfacePoint resolve(const Seed &seed)
    {
        if (seed.type == Seed::Type::Facet) {
            require(seed.facet >= 0 && size_t(seed.facet) < this->num_facets(), "seed: facet index out of range");
            const stl_triangle_vertex_indices &f = m_mesh.its.indices[seed.facet];
            return {seed.facet, (m_mesh.its.vertices[f[0]] + m_mesh.its.vertices[f[1]] + m_mesh.its.vertices[f[2]]) / 3.f};
        }
        require(! m_mesh.its.indices.empty(), "seed: the mesh is empty");
        if (m_tree.nodes().empty())
            m_tree = AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(m_mesh.its.vertices, m_mesh.its.indices);
        const Transform3d mesh_from_frame = m_trafo.frame_from_mesh(seed.frame).inverse();
        if (seed.type == Seed::Type::Point) {
            require(finite(seed.point), "seed: the point must be finite");
            size_t      idx = 0;
            Vec3f       hit = Vec3f::Zero();
            const Vec3f p   = (mesh_from_frame * seed.point).cast<float>();
            const float d2  = AABBTreeIndirect::squared_distance_to_indexed_triangle_set(m_mesh.its.vertices, m_mesh.its.indices, m_tree, p, idx, hit);
            require(d2 >= 0.f, "seed: no closest point found");
            return {int(idx), hit};
        }
        require(finite(seed.point) && finite(seed.dir) && seed.dir.squaredNorm() > 0., "seed: invalid ray");
        const Vec3d     origin = mesh_from_frame * seed.point;
        const Vec3d     dir    = (mesh_from_frame.linear() * seed.dir).normalized();
        igl::Hit<float> hit;
        if (! AABBTreeIndirect::intersect_ray_first_hit(m_mesh.its.vertices, m_mesh.its.indices, m_tree, origin, dir, hit))
            throw InvalidArgument("FacetPaint: seed: the ray misses the mesh");
        const stl_triangle_vertex_indices &f = m_mesh.its.indices[hit.id];
        const Vec3f p = (1.f - hit.u - hit.v) * m_mesh.its.vertices[f[0]] + hit.u * m_mesh.its.vertices[f[1]] + hit.v * m_mesh.its.vertices[f[2]];
        return {hit.id, p};
    }

    // Facets reachable from start through neighbors accepted by the predicate.
    template<typename Accept> std::vector<uint8_t> flood(const std::vector<int> &start, Accept &&accept)
    {
        const std::vector<Vec3i32> &nb = this->neighbors();
        std::vector<uint8_t>        visited(this->num_facets(), 0);
        std::queue<int>             queue;
        for (int f : start)
            if (! visited[f]) {
                visited[f] = 1;
                queue.push(f);
            }
        while (! queue.empty()) {
            const int f = queue.front();
            queue.pop();
            for (int n : nb[f])
                if (n >= 0 && ! visited[n] && accept(f, n)) {
                    visited[n] = 1;
                    queue.push(n);
                }
        }
        return visited;
    }

private:
    const TriangleMesh            &m_mesh;
    Transforms                     m_trafo;
    std::vector<Vec3i32>           m_neighbors;
    AABBTreeIndirect::Tree3f       m_tree;
};

Overlap classify_facet(const Node &node, const indexed_triangle_set &its, int facet)
{
    const stl_triangle_vertex_indices &f = its.indices[facet];
    return node.classify({its.vertices[f[0]], its.vertices[f[1]], its.vertices[f[2]]}, facet);
}

NodePtr compile(const Selector &s, Context &ctx);

std::vector<NodePtr> compile_children(const Selector &s, Context &ctx)
{
    std::vector<NodePtr> out;
    for (const Selector &child : s.m_children)
        out.emplace_back(compile(child, ctx));
    return out;
}

NodePtr shape_node(const Selector &s, Context &ctx, std::unique_ptr<Shape> shape)
{
    const Transform3d t = ctx.trafo().frame_from_mesh(s.m_frame);
    require(std::abs(t.linear().determinant()) > 1e-18, "the transformation is singular");
    return std::make_unique<ShapeNode>(t, std::move(shape));
}

NodePtr compile(const Selector &s, Context &ctx)
{
    using Type = Selector::Type;
    switch (s.m_type) {
    case Type::All: return std::make_unique<ConstNode>(true);
    case Type::Facets: {
        std::vector<uint8_t> mask(ctx.num_facets(), 0);
        for (int id : s.m_ids) {
            require(id >= 0 && size_t(id) < mask.size(), "facets: index out of range");
            mask[id] = 1;
        }
        return std::make_unique<FacetMaskNode>(std::move(mask));
    }
    case Type::Normal: {
        const std::vector<Vec3d> normals = ctx.normals(s.m_frame);
        // A small tolerance keeps facets exactly at the limit (a top face at 0 degrees) selected.
        const double         limit = std::cos(Geometry::deg2rad(s.m_a)) - 1e-6;
        std::vector<uint8_t> mask(normals.size(), 0);
        for (size_t i = 0; i < normals.size(); ++i)
            mask[i] = normals[i].squaredNorm() > 0. && normals[i].dot(s.m_v1) >= limit;
        return std::make_unique<FacetMaskNode>(std::move(mask));
    }
    case Type::HalfSpace: return shape_node(s, ctx, std::make_unique<HalfSpaceShape>(s.m_v1, s.m_a));
    case Type::Slab: return shape_node(s, ctx, std::make_unique<SlabShape>(s.m_v1, s.m_a, s.m_b));
    case Type::Box: return shape_node(s, ctx, std::make_unique<BoxShape>(s.m_v1, s.m_rotation, s.m_v2));
    case Type::Sphere: return shape_node(s, ctx, std::make_unique<SphereShape>(s.m_v1, s.m_a));
    case Type::Cylinder: return shape_node(s, ctx, std::make_unique<CylinderShape>(s.m_v1, s.m_v2, s.m_a, s.m_b));
    case Type::Capsule: return shape_node(s, ctx, std::make_unique<CapsuleShape>(s.m_v1, s.m_v2, s.m_a));
    case Type::Stroke: {
        // The path snapped to the surface, as world points: the brush is a ball in world space.
        const Transform3d    world_from_mesh = ctx.trafo().world_from_mesh();
        std::vector<int>     start;
        std::vector<Vec3d>   points;
        for (const Seed &seed : s.m_seeds) {
            const SurfacePoint sp = ctx.resolve(seed);
            start.push_back(sp.facet);
            points.push_back(world_from_mesh * sp.point.cast<double>());
        }
        Selector world = make(Type::Stroke, Frame::World);
        std::vector<NodePtr> parts;
        if (points.size() == 1)
            parts.emplace_back(shape_node(world, ctx, std::make_unique<SphereShape>(points.front(), s.m_a)));
        for (size_t i = 1; i < points.size(); ++i)
            parts.emplace_back(shape_node(world, ctx, std::make_unique<CapsuleShape>(points[i - 1], points[i], s.m_a)));
        NodePtr region = std::make_unique<OrNode>(std::move(parts));
        if (! s.m_flag)
            return region;
        // Like the GUI brush (select_patch): only surface connected to the stroke through facets it touches.
        const indexed_triangle_set &its  = ctx.its();
        std::vector<uint8_t>        mask = ctx.flood(start, [&](int, int n) { return classify_facet(*region, its, n) != Overlap::Outside; });
        std::vector<NodePtr>        both;
        both.emplace_back(std::make_unique<FacetMaskNode>(std::move(mask)));
        both.emplace_back(std::move(region));
        return std::make_unique<AndNode>(std::move(both));
    }
    case Type::Connected: {
        // Smart fill, the rule of TriangleSelector::seed_fill_select_triangles() on world normals.
        const SurfacePoint       sp      = ctx.resolve(s.m_seeds.front());
        const std::vector<Vec3d> normals = ctx.normals(Frame::World);
        const double             limit   = std::cos(Geometry::deg2rad(s.m_a)) - EPSILON;
        return std::make_unique<FacetMaskNode>(ctx.flood({sp.facet}, [&](int f, int n) {
            return std::clamp(normals[f].dot(normals[n]), 0., 1.) >= limit;
        }));
    }
    case Type::SameState: throw InvalidArgument("FacetPaint: same_state may only be used at the top level or in a top-level AND");
    case Type::And: return std::make_unique<AndNode>(compile_children(s, ctx));
    case Type::Or: return std::make_unique<OrNode>(compile_children(s, ctx));
    case Type::Not:
        require(s.m_children.size() == 1, "not: needs one operand");
        return std::make_unique<NotNode>(compile(s.m_children.front(), ctx));
    }
    throw InvalidArgument("FacetPaint: unknown selector");
}

// Split off a bucket fill: top level, or one operand of a top-level AND.
std::pair<const Selector *, Selector> split_same_state(const Selector &s)
{
    if (s.m_type == Selector::Type::SameState)
        return {&s, Selector::all()};
    if (s.m_type == Selector::Type::And) {
        const Selector *bucket = nullptr;
        Selector        rest   = make(Selector::Type::And, Frame::Mesh);
        for (const Selector &child : s.m_children)
            if (child.m_type == Selector::Type::SameState) {
                require(bucket == nullptr, "only one same_state selector is allowed");
                bucket = &child;
            } else
                rest.m_children.push_back(child);
        if (bucket != nullptr)
            return {bucket, std::move(rest)};
    }
    return {nullptr, s};
}

double triangle_area(const Vec3d &a, const Vec3d &b, const Vec3d &c) { return 0.5 * (b - a).cross(c - a).norm(); }

} // namespace

// ------------------------------------------------------------------------------------------------
// Statistics
// ------------------------------------------------------------------------------------------------

Stats statistics(const TriangleSelector &selector, const TriangleMesh &mesh, const Transforms &trafo)
{
    const Transform3d world_from_mesh  = trafo.world_from_mesh();
    const Transform3d object_from_mesh = trafo.object_from_mesh();

    Stats stats;
    stats.original_facets = mesh.its.indices.size();
    stats.live_triangles  = selector.num_live_triangles();
    for (const stl_triangle_vertex_indices &f : mesh.its.indices)
        stats.total_area += triangle_area(world_from_mesh * mesh.its.vertices[f[0]].cast<double>(), world_from_mesh * mesh.its.vertices[f[1]].cast<double>(),
                                          world_from_mesh * mesh.its.vertices[f[2]].cast<double>());

    std::vector<StateStats> per_state(size_t(EnforcerBlockerType::ExtruderMax) + 1);
    selector.for_each_leaf([&](const Vec3f &a, const Vec3f &b, const Vec3f &c, EnforcerBlockerType state, int) {
        ++stats.leaf_triangles;
        if (state == EnforcerBlockerType::NONE || size_t(state) >= per_state.size())
            return;
        StateStats &s = per_state[size_t(state)];
        s.state       = state;
        ++s.triangles;
        const Vec3d wa = world_from_mesh * a.cast<double>(), wb = world_from_mesh * b.cast<double>(), wc = world_from_mesh * c.cast<double>();
        s.area += triangle_area(wa, wb, wc);
        for (const Vec3d &p : {wa, wb, wc})
            s.bbox_world.merge(p);
        for (const Vec3f *p : {&a, &b, &c})
            s.bbox_object.merge(Vec3d(object_from_mesh * p->cast<double>()));
    });
    for (StateStats &s : per_state)
        if (s.triangles > 0) {
            s.fraction = stats.total_area > 0. ? s.area / stats.total_area : 0.;
            stats.states.push_back(s);
        }
    return stats;
}

Stats statistics(const TriangleMesh &mesh, const TriangleSelector::TriangleSplittingData &data, const Transforms &trafo, EnforcerBlockerType max_state)
{
    TriangleSelector selector(mesh);
    selector.deserialize(data, false, max_state);
    return statistics(selector, mesh, trafo);
}

// ------------------------------------------------------------------------------------------------
// Apply
// ------------------------------------------------------------------------------------------------

Result apply(const TriangleMesh &mesh, const TriangleSelector::TriangleSplittingData &existing, const Transforms &trafo, const Request &request)
{
    require(request.state <= request.max_state, "the state is not accepted by this channel");
    require(! request.only_where || *request.only_where <= request.max_state, "only_where: the state is not accepted by this channel");
    require(std::isfinite(request.resolution) && request.resolution > 0., "the resolution must be positive");

    Result result;
    result.data = existing;

    TriangleSelector selector(mesh);
    // The constructor has reset the selector already.
    selector.deserialize(existing, false, request.max_state);
    result.before = statistics(selector, mesh, trafo);

    Context ctx(mesh, trafo);
    auto [bucket, rest] = split_same_state(request.selector);
    const NodePtr       root = compile(rest, ctx);
    const RegionAdapter region(*root, mesh.its, request.mode == Mode::WholeFacets);

    // Original facets the selection touches, found in parallel; only those are walked.
    std::vector<uint8_t> touched(mesh.its.indices.size(), 0);
    tbb::parallel_for(tbb::blocked_range<size_t>(0, touched.size()), [&](const tbb::blocked_range<size_t> &range) {
        for (size_t i = range.begin(); i < range.end(); ++i) {
            const stl_triangle_vertex_indices &f = mesh.its.indices[i];
            touched[i] = region.classify(mesh.its.vertices[f[0]], mesh.its.vertices[f[1]], mesh.its.vertices[f[2]], int(i)) != Overlap::Outside;
        }
    });
    std::vector<int> facets;
    for (size_t i = 0; i < touched.size(); ++i)
        if (touched[i])
            facets.push_back(int(i));
    result.facets_selected = facets.size();

    // Bucket fill seed, resolved once (it does not depend on the paint).
    std::optional<SurfacePoint> bucket_seed;
    if (bucket != nullptr)
        bucket_seed = ctx.resolve(bucket->m_seeds.front());

    TriangleSelector::RegionParams params;
    params.edge_trafo    = trafo.world_from_mesh().cast<float>();
    params.only_state    = request.only_where;
    params.max_triangles = request.max_new_triangles == 0 ? 0 : result.before.live_triangles + request.max_new_triangles;

    double resolution = request.resolution;
    for (int attempt = 0;; ++attempt) {
        if (attempt > 0)
            selector.deserialize(existing, true, request.max_state);
        if (request.clear_first)
            selector.reset();
        if (bucket_seed) {
            Vec3f hit = bucket_seed->point;
            // The hit may sit on an edge of a sub-triangle; nudge it into the facet if needed.
            const stl_triangle_vertex_indices &f = mesh.its.indices[bucket_seed->facet];
            const Vec3f c = (mesh.its.vertices[f[0]] + mesh.its.vertices[f[1]] + mesh.its.vertices[f[2]]) / 3.f;
            for (int i = 0; i < 8 && selector.select_unsplit_triangle(hit, bucket_seed->facet) < 0; ++i)
                hit += 0.25f * (c - hit);
            require(selector.select_unsplit_triangle(hit, bucket_seed->facet) >= 0, "same_state: the seed is not on the surface");
            selector.bucket_fill_select_triangles(hit, bucket_seed->facet, TriangleSelector::ClippingPlane(), float(bucket->m_a), true, true);
            params.only_seed_fill_selected = true;
        }
        params.edge_limit  = float(resolution);
        const bool fits    = selector.select_region(region, request.state, params, &facets);
        if (bucket_seed)
            selector.seed_fill_unselect_all_triangles();
        if (fits)
            break;
        if (! request.coarsen_to_fit) {
            result.status  = Status::TooFine;
            result.message = "the resolution would add more than " + std::to_string(request.max_new_triangles) + " triangles";
            result.after   = result.before;
            result.resolution_used = request.resolution;
            return result;
        }
        resolution *= 2.;
    }
    result.resolution_used = resolution;
    if (resolution != request.resolution)
        result.message = "coarsened to fit the triangle limit";

    result.data    = selector.serialize();
    result.changed = result.data != existing;
    result.after   = statistics(selector, mesh, trafo);
    result.triangles_added = result.after.live_triangles > result.before.live_triangles ? result.after.live_triangles - result.before.live_triangles : 0;
    return result;
}

// ------------------------------------------------------------------------------------------------
// ModelVolume
// ------------------------------------------------------------------------------------------------

FacetsAnnotation &annotation(ModelVolume &volume, Channel channel)
{
    switch (channel) {
    case Channel::Support: return volume.supported_facets;
    case Channel::Seam: return volume.seam_facets;
    case Channel::Color: return volume.mmu_segmentation_facets;
    case Channel::FuzzySkin:
    default: return volume.fuzzy_skin_facets;
    }
}

const FacetsAnnotation &annotation(const ModelVolume &volume, Channel channel) { return annotation(const_cast<ModelVolume &>(volume), channel); }

EnforcerBlockerType max_state(Channel channel)
{
    switch (channel) {
    case Channel::Support:
    case Channel::Seam: return EnforcerBlockerType::BLOCKER;
    case Channel::FuzzySkin: return EnforcerBlockerType::FUZZY_SKIN;
    case Channel::Color:
    default: return EnforcerBlockerType::ExtruderMax;
    }
}

Result apply_to_volume(ModelVolume &volume, const Transform3d &instance_matrix, Channel channel, Request request)
{
    require(volume.is_model_part(), "only model parts can be painted");
    request.max_state = max_state(channel);
    Transforms trafo;
    trafo.volume   = volume.get_matrix();
    trafo.instance = instance_matrix;
    FacetsAnnotation &facets = annotation(volume, channel);
    Result            result = apply(volume.mesh(), facets.get_data(), trafo, request);
    if (result.status == Status::Ok && result.changed) {
        TriangleSelector::TriangleSplittingData data = result.data;
        facets.set_data(std::move(data));
    }
    return result;
}

} // namespace FacetPaint
} // namespace Slic3r
