#ifndef slic3r_Mcp_PaintSelector_hpp_
#define slic3r_Mcp_PaintSelector_hpp_

// The selector language of paint_apply (and mesh_query-like questions of paint_get): a JSON
// expression parsed and validated here, then compiled per painted part into a
// Slic3r::FacetPaint::Selector (libslic3r/FacetPaint.hpp).
//
// Frames. Positions and directions in a selector are in one of
//   "plate"  - mm from the origin of the plate the instance sits on (what scene_get reports),
//   "world"  - the scene's world coordinates (all plates share them; screenshot cameras use them),
//   "object" - the object's own coordinates: before the instance's placement, rotation and scale.
// The compiled selectors run in FacetPaint with the instance matrix made plate-relative, so
// FacetPaint's World frame IS the plate frame; "world" leaves are shifted by the plate origin.
//
// Seeds (a point on the surface: smart fill, bucket fill, strokes) are resolved once against all
// painted parts, so a seed paints only the part it lands on.
//
// Main thread only (reads the Model).

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "libslic3r/FacetPaint.hpp"
#include "libslic3r/Point.hpp"

#include "../../McpJson.hpp"

namespace Slic3r {
class AABBMesh;
class ModelObject;
class ModelVolume;
namespace GUI { namespace Mcp { namespace Paint {

enum class FrameKind { Plate, World, Object };

FrameKind   frame_from(const json& value, const std::string& path, FrameKind fallback = FrameKind::Plate);
const char* frame_name(FrameKind frame);

// One model part to paint and the frames it is seen in.
struct PaintTarget
{
    ModelObject* object { nullptr };
    int          obj_idx { -1 };
    ModelVolume* volume { nullptr };
    int          vol_idx { -1 };
    int          inst_idx { 0 };
    int          plate_idx { -1 };          // -1: the instance is on no plate (plate frame = world frame)
    Vec3d        plate_origin { Vec3d::Zero() };
    Transform3d  instance_world { Transform3d::Identity() }; // ModelInstance::get_matrix()

    // The instance matrix with the plate origin taken out: world frame of FacetPaint = plate frame.
    Transform3d instance_plate() const;
    FacetPaint::Transforms transforms() const; // volume matrix + instance_plate()
    std::string id() const;                      // v<N>
};

// Where a seed landed.
struct SeedHit
{
    size_t target { 0 };       // index into the targets
    int    facet { -1 };       // original facet of that part
    Vec3d  point { Vec3d::Zero() };  // plate frame of that target
    Vec3d  normal { Vec3d::Zero() }; // plate (= world) directions
    double distance { 0. };    // point seed: how far the given point was from the surface (mm); ray: along the ray
};

struct SeedSpec
{
    enum class Type { Point, Ray, Facet, Pixel };
    Type        type { Type::Point };
    std::string path;          // "selector.connected.seed", for messages
    Vec3d       point { Vec3d::Zero() };
    Vec3d       dir { Vec3d::Zero() };
    FrameKind   frame { FrameKind::Plate };
    int         facet { -1 };
    std::string part;          // facet seed: v<N> of the part (optional when only one part is painted)
    // Pixel seed: the world ray through the pixel, from the screenshot camera.
    Vec2d       pixel { Vec2d::Zero() };
    std::optional<SeedHit> hit;
};

struct SelectorSpec
{
    enum class Kind {
        All, Facets, Normal, Overhang, ZRange, Slab, HalfSpace, Box, OrientedBox, Sphere, Cylinder, Capsule, Stroke, Connected, SameState,
        And, Or, Not
    };
    Kind        kind { Kind::All };
    std::string path;
    FrameKind   frame { FrameKind::Plate };
    std::vector<int> ids;      // facets
    std::string part;          // facets: restrict to one part
    Vec3d       a { Vec3d::Zero() }, b { Vec3d::Zero() }, c { Vec3d::Zero() };
    Matrix3d    rotation { Matrix3d::Identity() };
    double      x { 0. }, y { 0. }, z { 0. };
    bool        flag { true };
    std::vector<SeedSpec>     seeds;
    std::vector<SelectorSpec> children;
};

// Parses and validates a selector. Throws ToolError(invalid_argument) naming the path of the
// offending member ("selector.and[1].box.min") and what is accepted.
SelectorSpec parse_selector(const json& value, const std::string& path = "selector");

// Parses a camera object as returned by screenshot (view_matrix, projection_matrix, width, height)
// and turns a pixel into a world ray. Throws ToolError(invalid_argument).
void pixel_ray(const json& camera, const Vec2d& pixel, const std::string& path, Vec3d& origin, Vec3d& direction);

// Resolves every seed of `spec` against the targets (closest surface point, first ray hit, facet).
// Throws ToolError(seed_missed) when a ray hits nothing, invalid_argument for a bad facet seed.
void resolve_seeds(SelectorSpec& spec, const std::vector<PaintTarget>& targets);

// The seeds and where they landed, for the result.
json seeds_json(const SelectorSpec& spec, const std::vector<PaintTarget>& targets);

// True when the selector contains a leaf that needs a seed / a leaf that is region-based.
bool has_seeds(const SelectorSpec& spec);

// The FacetPaint selector for one target. Seeds that landed on another part select nothing here.
// Throws ToolError(invalid_argument) for facet ids out of range.
FacetPaint::Selector compile(const SelectorSpec& spec, const std::vector<PaintTarget>& targets, size_t target);

// The JSON schema fragment "Selector" (and "Seed"), with descriptions, for tool schemas.
const char* selector_defs_json();

}}}} // namespace Slic3r::GUI::Mcp::Paint

#endif
