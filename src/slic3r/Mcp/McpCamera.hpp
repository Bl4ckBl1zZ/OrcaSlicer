#ifndef slic3r_Mcp_Camera_hpp_
#define slic3r_Mcp_Camera_hpp_

// The camera of MCP screenshots: a private one, computed here from scratch so that the user's
// camera is never read or moved and the exact matrices can be handed back to the agent (a pixel of
// a returned picture can be turned into a ray again).
//
// Conventions: Z up. Azimuth 0 looks from the front (camera on the -Y side, looking towards +Y);
// the camera orbits counter-clockwise seen from above as the azimuth grows, so 90 looks from the
// right (+X side). Elevation 0 is horizontal, +90 looks straight down, -90 straight up.
// Pixels: u to the right, v down, (0, 0) is the top-left corner of the picture.
//
// No wx, no GL: unit-testable.

#include <vector>

#include "libslic3r/Point.hpp"

#include "McpJson.hpp"

namespace Slic3r { namespace GUI { namespace Mcp {

struct CameraView
{
    bool   perspective { true };
    double azimuth_deg { 45. };
    double elevation_deg { 35. };
    Vec3d  target { Vec3d::Zero() }; // world
    double distance { 100. };        // eye to target, mm
    double fov_y_deg { 35. };        // perspective: vertical field of view
    double ortho_height { 100. };    // orthographic: world height covered by the picture, mm
    double near_z { 1. };
    double far_z { 1000. };
    int    width { 1024 };
    int    height { 768 };

    // Derived by update().
    Vec3d       eye { Vec3d::Zero() };
    Vec3d       up { Vec3d::UnitZ() };
    Transform3d view { Transform3d::Identity() };       // world -> eye (eye looks down -Z)
    Transform3d projection { Transform3d::Identity() }; // eye -> clip (OpenGL conventions)

    // Recomputes eye, up, view and projection from the parameters above.
    void update();

    // Sets distance (perspective) or ortho_height (orthographic) so that all `points` are inside
    // the picture with a border; `zoom` 2 shows the middle half. Does not move the target.
    void fit(const std::vector<Vec3d>& points, double zoom = 1., double fill = 0.9);
    // Sets near_z / far_z so that all `points` are between the planes. For an orthographic view it
    // may also push the eye back so that everything is in front of it.
    void set_depth_range(const std::vector<Vec3d>& points);

    // World point -> pixel. Returns false when the point is behind the eye.
    bool project(const Vec3d& world, Vec2d& pixel) const;
    // Pixel -> ray in world coordinates.
    void ray(const Vec2d& pixel, Vec3d& origin, Vec3d& direction) const;

    // {"projection", "azimuth_deg", "elevation_deg", "target", "position", "up", "distance",
    //  "fov_y_deg" | "ortho_height", "near", "far", "width", "height", "view_matrix",
    //  "projection_matrix"}; matrices are row-major 4x4.
    json to_json() const;
    // Rebuilds a view from to_json() output (as an agent passes it back). Returns false when a
    // required member is missing or malformed.
    static bool from_json(const json& value, CameraView& out);
};

// The corners of an axis-aligned box.
std::vector<Vec3d> box_corners(const Vec3d& min, const Vec3d& max);

}}} // namespace Slic3r::GUI::Mcp

#endif
