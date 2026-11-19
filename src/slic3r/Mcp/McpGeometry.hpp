#ifndef slic3r_Mcp_Geometry_hpp_
#define slic3r_Mcp_Geometry_hpp_

// Geometry queries on triangle meshes: ray casting, overhang detection, flat faces for laying an
// object down, surface area. Plain libslic3r data in, plain data out: no wx, no GUI, unit-testable.

#include <cstddef>
#include <vector>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/TriangleMesh.hpp"

namespace Slic3r { namespace GUI { namespace Mcp {

namespace Geometry {

struct RayHit
{
    bool   hit { false };
    double distance { 0. }; // along the ray, in the units of `direction` (world mm when it is normalised)
    int    facet { -1 };    // index into its.indices
    Vec3d  point { Vec3d::Zero() };  // world
    Vec3d  normal { Vec3d::Zero() }; // world, unit length, facing the ray origin's side of the facet as the mesh winds it
};

// Nearest intersection of the ray (world coordinates) with the mesh placed by `world`.
RayHit ray_mesh(const indexed_triangle_set& its, const Transform3d& world, const Vec3d& origin, const Vec3d& direction);

// Surface area of the mesh as placed by `world`, mm2.
double surface_area(const indexed_triangle_set& its, const Transform3d& world);

struct OverhangCluster
{
    BoundingBoxf3 box;        // world
    double        area { 0. };
    size_t        facets { 0 };
};

struct OverhangReport
{
    double                       area { 0. };
    size_t                       facets { 0 };
    double                       lowest_z { 0. }; // lowest point of any overhanging facet, world
    std::vector<OverhangCluster> clusters;        // connected groups of overhanging facets, largest first
};

// Facets whose normal is within `threshold_deg` of straight down - the test the support painting
// gizmo applies ("highlight overhangs by angle"). Facets lying on the bed (every vertex within
// 0.05 mm of `bed_z`) are not overhangs.
OverhangReport overhangs(const indexed_triangle_set& its, const Transform3d& world, double threshold_deg, double bed_z);

struct FlatFace
{
    Vec3d  normal { Vec3d::Zero() }; // outward, unit length, in the frame of the mesh given
    double area { 0. };
};

// The planes of the convex hull, largest first: the faces an object can be laid on, as the
// "Lay on face" gizmo offers them. Faces under `min_area_ratio` of the largest are dropped.
std::vector<FlatFace> flat_faces(const TriangleMesh& mesh, double min_area_ratio = 0.02, size_t max_faces = 20);

} // namespace Geometry

}}} // namespace Slic3r::GUI::Mcp

#endif
