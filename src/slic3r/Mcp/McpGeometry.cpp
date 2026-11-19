#include "McpGeometry.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace Slic3r { namespace GUI { namespace Mcp {

namespace Geometry {

namespace {

const double DEG_TO_RAD = 3.14159265358979323846 / 180.;

// Union-find over facet indices.
struct Groups
{
    explicit Groups(size_t count) : parent(count) { std::iota(parent.begin(), parent.end(), size_t(0)); }

    size_t find(size_t i)
    {
        while (parent[i] != i) {
            parent[i] = parent[parent[i]];
            i = parent[i];
        }
        return i;
    }

    void join(size_t a, size_t b)
    {
        a = find(a);
        b = find(b);
        if (a != b)
            parent[b] = a;
    }

    std::vector<size_t> parent;
};

} // namespace

RayHit ray_mesh(const indexed_triangle_set& its, const Transform3d& world, const Vec3d& origin, const Vec3d& direction)
{
    // Work in the mesh's own frame; the ray parameter is the same in both frames because the
    // direction is transformed without renormalising.
    const Transform3d inverse = world.inverse();
    const Vec3d       o = inverse * origin;
    const Vec3d       d = inverse.linear() * direction;

    RayHit best;
    double best_t = std::numeric_limits<double>::max();
    for (size_t f = 0; f < its.indices.size(); ++f) {
        const Vec3d a = its.vertices[its.indices[f][0]].cast<double>();
        const Vec3d b = its.vertices[its.indices[f][1]].cast<double>();
        const Vec3d c = its.vertices[its.indices[f][2]].cast<double>();
        // Moeller-Trumbore.
        const Vec3d  e1 = b - a;
        const Vec3d  e2 = c - a;
        const Vec3d  p = d.cross(e2);
        const double det = e1.dot(p);
        if (std::abs(det) < 1e-12)
            continue;
        const double inv = 1. / det;
        const Vec3d  s = o - a;
        const double u = s.dot(p) * inv;
        if (u < -1e-9 || u > 1. + 1e-9)
            continue;
        const Vec3d  q = s.cross(e1);
        const double v = d.dot(q) * inv;
        if (v < -1e-9 || u + v > 1. + 1e-9)
            continue;
        const double t = e2.dot(q) * inv;
        if (t <= 1e-9 || t >= best_t)
            continue;
        best_t = t;
        best.hit = true;
        best.facet = static_cast<int>(f);
        best.normal = e1.cross(e2);
    }
    if (best.hit) {
        best.distance = best_t * direction.norm();
        best.point = origin + best_t * direction;
        // Normals transform with the inverse transpose.
        best.normal = (world.linear().inverse().transpose() * best.normal).normalized();
    }
    return best;
}

double surface_area(const indexed_triangle_set& its, const Transform3d& world)
{
    double area = 0.;
    for (const auto& facet : its.indices) {
        const Vec3d a = world * its.vertices[facet[0]].cast<double>();
        const Vec3d b = world * its.vertices[facet[1]].cast<double>();
        const Vec3d c = world * its.vertices[facet[2]].cast<double>();
        area += 0.5 * (b - a).cross(c - a).norm();
    }
    return area;
}

OverhangReport overhangs(const indexed_triangle_set& its, const Transform3d& world, double threshold_deg, double bed_z)
{
    OverhangReport report;
    report.lowest_z = std::numeric_limits<double>::max();
    const double limit = std::cos(std::max(0., std::min(90., threshold_deg)) * DEG_TO_RAD);

    std::vector<Vec3d> vertices;
    vertices.reserve(its.vertices.size());
    for (const auto& vertex : its.vertices)
        vertices.push_back(world * vertex.cast<double>());
    // A mirrored placement flips the winding, and with it the computed normals.
    const double winding = world.linear().determinant() < 0. ? -1. : 1.;

    std::vector<char>   is_overhang(its.indices.size(), 0);
    std::vector<double> areas(its.indices.size(), 0.);
    for (size_t f = 0; f < its.indices.size(); ++f) {
        const Vec3d& a = vertices[its.indices[f][0]];
        const Vec3d& b = vertices[its.indices[f][1]];
        const Vec3d& c = vertices[its.indices[f][2]];
        const Vec3d  cross = winding * (b - a).cross(c - a);
        const double twice_area = cross.norm();
        if (twice_area < 1e-12)
            continue;
        // Within the threshold of straight down?
        if (-cross.z() / twice_area <= limit)
            continue;
        if (a.z() < bed_z + 0.05 && b.z() < bed_z + 0.05 && c.z() < bed_z + 0.05)
            continue; // resting on the bed
        is_overhang[f] = 1;
        areas[f] = 0.5 * twice_area;
    }

    // Group overhanging facets that share a vertex.
    Groups              groups(its.indices.size());
    std::vector<size_t> first_facet_of_vertex(its.vertices.size(), std::numeric_limits<size_t>::max());
    for (size_t f = 0; f < its.indices.size(); ++f) {
        if (!is_overhang[f])
            continue;
        for (int k = 0; k < 3; ++k) {
            size_t& first = first_facet_of_vertex[static_cast<size_t>(its.indices[f][k])];
            if (first == std::numeric_limits<size_t>::max())
                first = f;
            else
                groups.join(first, f);
        }
    }
    std::vector<size_t> cluster_of(its.indices.size(), std::numeric_limits<size_t>::max());
    for (size_t f = 0; f < its.indices.size(); ++f) {
        if (!is_overhang[f])
            continue;
        const size_t root = groups.find(f);
        if (cluster_of[root] == std::numeric_limits<size_t>::max()) {
            cluster_of[root] = report.clusters.size();
            report.clusters.emplace_back();
        }
        OverhangCluster& cluster = report.clusters[cluster_of[root]];
        cluster.area += areas[f];
        ++cluster.facets;
        for (int k = 0; k < 3; ++k) {
            const Vec3d& vertex = vertices[static_cast<size_t>(its.indices[f][k])];
            cluster.box.merge(vertex);
            report.lowest_z = std::min(report.lowest_z, vertex.z());
        }
        report.area += areas[f];
        ++report.facets;
    }
    std::sort(report.clusters.begin(), report.clusters.end(), [](const OverhangCluster& a, const OverhangCluster& b) { return a.area > b.area; });
    if (report.facets == 0)
        report.lowest_z = 0.;
    return report;
}

std::vector<FlatFace> flat_faces(const TriangleMesh& mesh, double min_area_ratio, size_t max_faces)
{
    std::vector<FlatFace> faces;
    if (mesh.empty())
        return faces;
    const TriangleMesh          hull = mesh.convex_hull_3d();
    const indexed_triangle_set& its = hull.its;
    // Merge hull triangles that lie in one plane: same normal within about a degree.
    for (const auto& facet : its.indices) {
        const Vec3d  a = its.vertices[facet[0]].cast<double>();
        const Vec3d  b = its.vertices[facet[1]].cast<double>();
        const Vec3d  c = its.vertices[facet[2]].cast<double>();
        const Vec3d  cross = (b - a).cross(c - a);
        const double twice_area = cross.norm();
        if (twice_area < 1e-9)
            continue;
        const Vec3d normal = cross / twice_area;
        bool        merged = false;
        for (FlatFace& face : faces)
            if (face.normal.dot(normal) > 0.9998) {
                face.area += 0.5 * twice_area;
                merged = true;
                break;
            }
        if (!merged)
            faces.push_back(FlatFace { normal, 0.5 * twice_area });
    }
    std::sort(faces.begin(), faces.end(), [](const FlatFace& a, const FlatFace& b) { return a.area > b.area; });
    if (!faces.empty()) {
        const double smallest = faces.front().area * min_area_ratio;
        faces.erase(std::remove_if(faces.begin(), faces.end(), [smallest](const FlatFace& face) { return face.area < smallest; }), faces.end());
    }
    if (faces.size() > max_faces)
        faces.resize(max_faces);
    return faces;
}

} // namespace Geometry

}}} // namespace Slic3r::GUI::Mcp
