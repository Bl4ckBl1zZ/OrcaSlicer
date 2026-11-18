#ifndef slic3r_FacetPaint_hpp_
#define slic3r_FacetPaint_hpp_

// Painting of mesh facets by geometric rules instead of a mouse brush: support enforcers and
// blockers, seam enforcers and blockers, filament colour and fuzzy skin.
//
// A request names a selector (what to paint), a state (how to paint it) and a resolution. The
// result is the same TriangleSplittingData the painting gizmos store in a FacetsAnnotation, so it
// renders, saves to 3MF and slices exactly like hand-painted data. Region boundaries cut through
// large triangles: triangles straddling a boundary are subdivided down to the resolution.
//
// Frames: paint data lives in the mesh frame of a ModelVolume. Positions and directions given to a
// selector are in one of three frames:
//   Mesh   - the volume's own vertex coordinates,
//   Object - after the volume matrix (ModelVolume::get_matrix()),
//   World  - after the instance matrix too (ModelInstance::get_matrix() * ModelVolume::get_matrix()).
// Lengths (resolution, statistics) are measured in the world frame.

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "BoundingBox.hpp"
#include "Point.hpp"
#include "TriangleSelector.hpp"

namespace Slic3r {

class ModelVolume;
class FacetsAnnotation;

namespace FacetPaint {

enum class Frame { Mesh, Object, World };

// Mesh frame -> object frame -> world frame.
struct Transforms
{
    Transform3d volume   = Transform3d::Identity(); // ModelVolume::get_matrix()
    Transform3d instance = Transform3d::Identity(); // ModelInstance::get_matrix()

    Transform3d object_from_mesh() const { return volume; }
    Transform3d world_from_mesh() const { return instance * volume; }
    Transform3d frame_from_mesh(Frame frame) const;
};

// A point on the surface: a point snapped to the closest surface point, the first hit of a ray, or
// the centroid of an original facet.
struct Seed
{
    enum class Type { Point, Ray, Facet };
    Type  type  = Type::Facet;
    Vec3d point = Vec3d::Zero(); // Point: the point; Ray: the origin.
    Vec3d dir   = Vec3d::UnitZ(); // Ray only.
    int   facet = 0;              // Facet only.
    Frame frame = Frame::World;

    static Seed at_point(const Vec3d &p, Frame frame = Frame::World) { Seed s; s.type = Type::Point; s.point = p; s.frame = frame; return s; }
    static Seed along_ray(const Vec3d &origin, const Vec3d &dir, Frame frame = Frame::World) { Seed s; s.type = Type::Ray; s.point = origin; s.dir = dir; s.frame = frame; return s; }
    static Seed on_facet(int facet) { Seed s; s.type = Type::Facet; s.facet = facet; return s; }
};

// What to paint. Leaves are combined with &&, || and !.
//
// Whole-facet leaves (all, facets, normal, connected) select original facets as a whole; region
// leaves (halfspace, slab, box, sphere, cylinder, capsule, stroke) select the part of the surface
// inside a solid and are resolved to the requested resolution. same_state (bucket fill) selects
// existing paint and may only be used at the top level or as an operand of a top-level &&.
class Selector
{
public:
    enum class Type {
        All, Facets, Normal, HalfSpace, Slab, Box, Sphere, Cylinder, Capsule, Stroke, Connected, SameState, And, Or, Not
    };

    // Every facet / no facet (clear_first with none() clears the channel).
    static Selector all();
    static Selector none() { return !all(); }
    // Original facets by index into ModelVolume::mesh().its.indices.
    static Selector facets(std::vector<int> ids);
    // Facets whose outward normal is within max_angle_deg of direction.
    static Selector normal(const Vec3d &direction, double max_angle_deg, Frame frame = Frame::World);
    // Overhangs the way the support gizmo's "by angle" selects them: facets whose normal is within
    // threshold_deg of straight down (threshold measured from the horizontal plane, like
    // support_threshold_angle).
    static Selector overhang(double threshold_deg, const Vec3d &up = Vec3d::UnitZ(), Frame frame = Frame::World);
    // The side of a plane the normal points to: (p - point) . normal >= 0.
    static Selector halfspace(const Vec3d &point, const Vec3d &normal, Frame frame = Frame::World);
    // Between two parallel planes: min <= p . normal <= max (normal is normalized first).
    static Selector slab(const Vec3d &normal, double min, double max, Frame frame = Frame::World);
    // A horizontal band min <= z <= max.
    static Selector z_band(double z_min, double z_max, Frame frame = Frame::World) { return slab(Vec3d::UnitZ(), z_min, z_max, frame); }
    // Axis-aligned box.
    static Selector box(const Vec3d &min, const Vec3d &max, Frame frame = Frame::World);
    // Oriented box: the columns of rotation are the box axes.
    static Selector oriented_box(const Vec3d &center, const Matrix3d &rotation, const Vec3d &half_size, Frame frame = Frame::World);
    static Selector sphere(const Vec3d &center, double radius, Frame frame = Frame::World);
    // Finite cylinder from base along axis for height.
    static Selector cylinder(const Vec3d &base, const Vec3d &axis, double height, double radius, Frame frame = Frame::World);
    // All points within radius of the segment a-b.
    static Selector capsule(const Vec3d &a, const Vec3d &b, double radius, Frame frame = Frame::World);
    // A brush stroke: a polyline of surface points (seeds are snapped to the surface) painted with
    // a ball of the given radius. With connected_only, like the GUI brush, only surface reachable
    // from the stroke's own facets through facets touched by the stroke is painted, so a thin
    // wall's other side is left alone.
    static Selector stroke(std::vector<Seed> path, double radius, bool connected_only = true);
    // Smart fill: the facets reachable from the seed without crossing an edge sharper than angle_deg.
    static Selector connected(const Seed &seed, double angle_deg = 30.);
    // Bucket fill: the existing region of the seed's current state, connected to the seed. A
    // negative angle_deg disables edge detection, otherwise the fill stops at sharper edges.
    static Selector same_state(const Seed &seed, double angle_deg = -1.);

    friend Selector operator&&(Selector lhs, Selector rhs);
    friend Selector operator||(Selector lhs, Selector rhs);
    friend Selector operator!(Selector operand);

    Type type() const { return m_type; }

    // Parameters, valid according to type().
    Type                  m_type = Type::All;
    Frame                 m_frame = Frame::World;
    std::vector<int>      m_ids;
    Vec3d                 m_v1 = Vec3d::Zero(), m_v2 = Vec3d::Zero(), m_v3 = Vec3d::Zero();
    Matrix3d              m_rotation = Matrix3d::Identity();
    double                m_a = 0., m_b = 0., m_c = 0.;
    bool                  m_flag = false;
    std::vector<Seed>     m_seeds;
    std::vector<Selector> m_children;
};

enum class Mode {
    // Subdivide triangles straddling a region boundary down to the resolution.
    Precise,
    // Paint whole original facets whose centroid is in the region.
    WholeFacets,
};

struct Request
{
    Selector            selector = Selector::all();
    // State to paint; NONE erases paint inside the selection.
    EnforcerBlockerType state    = EnforcerBlockerType::ENFORCER;
    // Only repaint surface currently in this state (replace one state with another).
    std::optional<EnforcerBlockerType> only_where;
    // Erase all paint of the channel before painting.
    bool                clear_first = false;
    Mode                mode        = Mode::Precise;
    // Longest side of a triangle on a region boundary, world frame, mm.
    double              resolution  = 0.3;
    // Hard cap on the triangles the operation may add. When it would be exceeded the resolution
    // is doubled until it fits (coarsen_to_fit) or the request fails with Status::TooFine.
    size_t              max_new_triangles = 300000;
    bool                coarsen_to_fit    = true;
    // Highest state the channel accepts. States above it in the existing data are dropped on load,
    // as the gizmos do. BLOCKER for support and seam, FUZZY_SKIN for fuzzy skin, ExtruderMax for colour.
    EnforcerBlockerType max_state = EnforcerBlockerType::ExtruderMax;
};

struct StateStats
{
    EnforcerBlockerType state     = EnforcerBlockerType::NONE;
    double              area      = 0.; // mm2, world frame
    double              fraction  = 0.; // of the whole surface
    size_t              triangles = 0;  // leaf triangles
    BoundingBoxf3       bbox_world;
    BoundingBoxf3       bbox_object;
};

struct Stats
{
    size_t                  original_facets = 0;
    size_t                  leaf_triangles  = 0; // leaves of all states, NONE included
    size_t                  live_triangles  = 0; // split nodes and leaves, original ones included
    double                  total_area      = 0.; // mm2, world frame
    std::vector<StateStats> states;               // painted states (not NONE), ascending

    const StateStats *find(EnforcerBlockerType state) const;
    double            area(EnforcerBlockerType state) const { const StateStats *s = this->find(state); return s ? s->area : 0.; }
};

enum class Status { Ok, TooFine };

struct Result
{
    Status                                  status = Status::Ok;
    std::string                             message;
    TriangleSelector::TriangleSplittingData data;    // the new paint (the existing one if not Ok)
    bool                                    changed = false;
    Stats                                   before;
    Stats                                   after;
    double                                  resolution_used = 0.;
    size_t                                  triangles_added = 0; // live triangles after minus before (may be negative -> 0)
    size_t                                  facets_selected = 0; // original facets the selection touched
};

// Apply a request to existing paint of one mesh. Throws Slic3r::InvalidArgument on a malformed
// selector or a seed that does not reach the mesh.
Result apply(const TriangleMesh &mesh, const TriangleSelector::TriangleSplittingData &existing, const Transforms &trafo, const Request &request);

// Statistics of existing paint.
Stats statistics(const TriangleMesh &mesh, const TriangleSelector::TriangleSplittingData &data, const Transforms &trafo,
                 EnforcerBlockerType max_state = EnforcerBlockerType::ExtruderMax);
Stats statistics(const TriangleSelector &selector, const TriangleMesh &mesh, const Transforms &trafo);

// The paint channels of a ModelVolume.
enum class Channel { Support, Seam, Color, FuzzySkin };

FacetsAnnotation       &annotation(ModelVolume &volume, Channel channel);
const FacetsAnnotation &annotation(const ModelVolume &volume, Channel channel);
// Highest state a channel accepts.
EnforcerBlockerType     max_state(Channel channel);

// Apply a request to one channel of a model part and store the result in its FacetsAnnotation
// when it changed (the same commit the gizmos do). request.max_state is overridden by the channel.
// instance_matrix is ModelInstance::get_matrix() of the instance the world frame refers to.
// The caller schedules the background process / refreshes the GUI as for a gizmo edit.
// Throws Slic3r::InvalidArgument for a volume that is not a model part or a state the channel does not accept.
Result apply_to_volume(ModelVolume &volume, const Transform3d &instance_matrix, Channel channel, Request request);

} // namespace FacetPaint
} // namespace Slic3r

#endif // slic3r_FacetPaint_hpp_
