#include "McpPreviewRenderer.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>

#include <boost/log/trivial.hpp>
#include <glad/gl.h>
#include <miniz.h>
#include <wx/thread.h>
#include <wx/timer.h>

#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Slicing.hpp"

#include "slic3r/GUI/3DScene.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GLModel.hpp"
#include "slic3r/GUI/GLShader.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/LibVGCode/LibVGCodeWrapper.hpp"
#include "../../libvgcode/include/ColorPrint.hpp"
#include "slic3r/GUI/OpenGLManager.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"

namespace Slic3r { namespace GUI { namespace Mcp {

using json = nlohmann::ordered_json;

namespace {

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// ---------------------------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------------------------

struct ViewTypeName { const char* name; libvgcode::EViewType type; const char* title; const char* unit; };

const std::vector<ViewTypeName>& view_type_table()
{
    static const std::vector<ViewTypeName> table = {
        { "feature_type",           libvgcode::EViewType::FeatureType,              "Line type",               "" },
        { "height",                 libvgcode::EViewType::Height,                   "Layer height",            "mm" },
        { "width",                  libvgcode::EViewType::Width,                    "Line width",              "mm" },
        { "speed",                  libvgcode::EViewType::Speed,                    "Speed",                   "mm/s" },
        { "actual_speed",           libvgcode::EViewType::ActualSpeed,              "Actual speed",            "mm/s" },
        { "fan_speed",              libvgcode::EViewType::FanSpeed,                 "Fan speed",               "%" },
        { "temperature",            libvgcode::EViewType::Temperature,              "Temperature",             "\xC2\xB0" "C" },
        { "volumetric_flow",        libvgcode::EViewType::VolumetricFlowRate,       "Volumetric flow rate",    "mm\xC2\xB3/s" },
        { "actual_volumetric_flow", libvgcode::EViewType::ActualVolumetricFlowRate, "Actual volumetric flow",  "mm\xC2\xB3/s" },
        { "layer_time",             libvgcode::EViewType::LayerTimeLinear,          "Layer time",              "s" },
        { "layer_time_log",         libvgcode::EViewType::LayerTimeLogarithmic,     "Layer time (log)",        "s" },
        { "tool",                   libvgcode::EViewType::Tool,                     "Filament",                "" },
        { "color_print",            libvgcode::EViewType::ColorPrint,               "Color print",             "" },
        { "summary",                libvgcode::EViewType::Summary,                  "Summary",                 "" },
        { "pressure_advance",       libvgcode::EViewType::PressureAdvance,          "Pressure advance",        "" },
        { "acceleration",           libvgcode::EViewType::Acceleration,             "Acceleration",            "mm/s\xC2\xB2" },
        { "jerk",                   libvgcode::EViewType::Jerk,                     "Jerk",                    "mm/s" },
    };
    return table;
}

const ViewTypeName* find_view_type(const std::string& name)
{
    for (const ViewTypeName& v : view_type_table())
        if (name == v.name)
            return &v;
    return nullptr;
}

struct OptionName { const char* name; libvgcode::EOptionType type; const char* label; };

const std::vector<OptionName>& option_table()
{
    static const std::vector<OptionName> table = {
        { "travels",       libvgcode::EOptionType::Travels,       "Travel" },
        { "wipes",         libvgcode::EOptionType::Wipes,         "Wipe" },
        { "retractions",   libvgcode::EOptionType::Retractions,   "Retract" },
        { "deretractions", libvgcode::EOptionType::Unretractions, "Unretract" },
        { "seams",         libvgcode::EOptionType::Seams,         "Seams" },
        { "tool_changes",  libvgcode::EOptionType::ToolChanges,   "Filament changes" },
        { "color_changes", libvgcode::EOptionType::ColorChanges,  "Color changes" },
        { "pauses",        libvgcode::EOptionType::PausePrints,   "Pauses" },
        { "custom_gcodes", libvgcode::EOptionType::CustomGCodes,  "Custom G-codes" },
    };
    return table;
}

// Stable keys for the extrusion roles; the labels are the application's own role names.
const char* feature_key(libvgcode::EGCodeExtrusionRole role)
{
    using R = libvgcode::EGCodeExtrusionRole;
    switch (role) {
    case R::None:                     return "none";
    case R::Perimeter:                return "inner_wall";
    case R::ExternalPerimeter:        return "outer_wall";
    case R::OverhangPerimeter:        return "overhang_wall";
    case R::InternalInfill:           return "sparse_infill";
    case R::SolidInfill:              return "internal_solid_infill";
    case R::TopSolidInfill:           return "top_surface";
    case R::Ironing:                  return "ironing";
    case R::BridgeInfill:             return "bridge";
    case R::GapFill:                  return "gap_infill";
    case R::Skirt:                    return "skirt";
    case R::SupportMaterial:          return "support";
    case R::SupportMaterialInterface: return "support_interface";
    case R::WipeTower:                return "prime_tower";
    case R::Custom:                   return "custom";
    case R::BottomSurface:            return "bottom_surface";
    case R::InternalBridgeInfill:     return "internal_bridge";
    case R::Brim:                     return "brim";
    case R::SupportTransition:        return "support_transition";
    case R::Mixed:                    return "multiple";
    default:                          return "";
    }
}

std::string feature_label(libvgcode::EGCodeExtrusionRole role)
{
    if (role == libvgcode::EGCodeExtrusionRole::None)
        return "Undefined";
    return ExtrusionEntity::role_to_string(libvgcode::convert(role));
}

bool find_feature(const std::string& key, libvgcode::EGCodeExtrusionRole& out)
{
    for (size_t i = 0; i < libvgcode::GCODE_EXTRUSION_ROLES_COUNT; ++i) {
        const auto role = static_cast<libvgcode::EGCodeExtrusionRole>(i);
        if (key == feature_key(role)) {
            out = role;
            return true;
        }
    }
    return false;
}

std::array<uint8_t, 3> to_rgb(const libvgcode::Color& c) { return { c[0], c[1], c[2] }; }

// ---------------------------------------------------------------------------------------------
// Camera maths. The application's GUI::Camera is not used: its select_view() switches between
// perspective and orthographic from app_config ("auto_perspective") and set_zoom() clamps to the
// scene box, both of which would make the picture depend on the user's settings.
// ---------------------------------------------------------------------------------------------

Vec3d orbit_direction(double azimuth_deg, double elevation_deg)
{
    // From the target towards the eye.
    const double az = azimuth_deg * M_PI / 180.0;
    const double el = elevation_deg * M_PI / 180.0;
    return Vec3d(-std::sin(az) * std::cos(el), -std::cos(az) * std::cos(el), std::sin(el));
}

Vec3d orbit_up(double azimuth_deg, double elevation_deg)
{
    if (std::abs(elevation_deg) < 89.9)
        return Vec3d::UnitZ();
    const double az = azimuth_deg * M_PI / 180.0;
    const Vec3d ahead(std::sin(az), std::cos(az), 0.0); // the horizontal direction the camera faces
    return elevation_deg > 0.0 ? ahead : Vec3d(-ahead);
}

Matrix4d look_at(const Vec3d& eye, const Vec3d& target, const Vec3d& up)
{
    const Vec3d z = (eye - target).normalized();
    const Vec3d x = up.cross(z).normalized();
    const Vec3d y = z.cross(x).normalized();
    Matrix4d m = Matrix4d::Identity();
    m.block<1, 3>(0, 0) = x.transpose();
    m.block<1, 3>(1, 0) = y.transpose();
    m.block<1, 3>(2, 0) = z.transpose();
    m(0, 3) = -x.dot(eye);
    m(1, 3) = -y.dot(eye);
    m(2, 3) = -z.dot(eye);
    return m;
}

Matrix4d frustum(bool perspective, double l, double r, double b, double t, double n, double f)
{
    // The same matrices as GUI::Camera::apply_projection().
    Matrix4d m = Matrix4d::Zero();
    if (perspective) {
        m(0, 0) = 2.0 * n / (r - l);
        m(0, 2) = (r + l) / (r - l);
        m(1, 1) = 2.0 * n / (t - b);
        m(1, 2) = (t + b) / (t - b);
        m(2, 2) = -(f + n) / (f - n);
        m(2, 3) = -2.0 * f * n / (f - n);
        m(3, 2) = -1.0;
    } else {
        m(0, 0) = 2.0 / (r - l);
        m(0, 3) = -(r + l) / (r - l);
        m(1, 1) = 2.0 / (t - b);
        m(1, 3) = -(t + b) / (t - b);
        m(2, 2) = -2.0 / (f - n);
        m(2, 3) = -(f + n) / (f - n);
        m(3, 3) = 1.0;
    }
    return m;
}

std::array<Vec3d, 8> box_corners(const BoundingBoxf3& box)
{
    std::array<Vec3d, 8> c;
    for (int i = 0; i < 8; ++i)
        c[i] = Vec3d((i & 1) ? box.max.x() : box.min.x(), (i & 2) ? box.max.y() : box.min.y(), (i & 4) ? box.max.z() : box.min.z());
    return c;
}

struct CameraState
{
    bool   perspective{ true };
    Vec3d  dir{ Vec3d::UnitZ() };   // target -> eye, unit
    Vec3d  up{ Vec3d::UnitZ() };
    Vec3d  target{ Vec3d::Zero() };
    double distance{ 1000.0 };
    double zoom{ 1.0 };
    int    width{ 1 };
    int    height{ 1 };

    Vec3d    eye() const { return target + distance * dir; }
    Matrix4d view() const { return look_at(eye(), target, up); }

    // Projection whose near and far planes enclose the scene box.
    Matrix4d projection(const BoundingBoxf3& scene, double* near_out = nullptr, double* far_out = nullptr) const
    {
        const Matrix4d v = view();
        double dmin = std::numeric_limits<double>::max();
        double dmax = -std::numeric_limits<double>::max();
        for (const Vec3d& c : box_corners(scene)) {
            const double d = -(v * c.homogeneous())(2);
            dmin = std::min(dmin, d);
            dmax = std::max(dmax, d);
        }
        const double pad = std::max(1.0, 0.02 * (dmax - dmin));
        double n = dmin - pad;
        double f = dmax + pad;
        const double hw = 0.5 * double(width) / zoom;  // half size of the picture at the target plane
        const double hh = 0.5 * double(height) / zoom;
        Matrix4d p;
        if (perspective) {
            n = std::max(n, std::max(0.01 * distance, 0.1));
            f = std::max(f, n + 1.0);
            const double s = n / distance;
            p = frustum(true, -hw * s, hw * s, -hh * s, hh * s, n, f);
        } else {
            if (f - n < 1.0)
                f = n + 1.0;
            p = frustum(false, -hw, hw, -hh, hh, n, f);
        }
        if (near_out != nullptr) *near_out = n;
        if (far_out != nullptr) *far_out = f;
        return p;
    }
};

// Fits the box into the picture, leaving `margin` of the size free on each side. With a free target
// the box is also centred; with a given target it stays the centre of the picture.
void fit_camera(CameraState& cam, const BoundingBoxf3& box, double margin, bool free_target)
{
    const std::array<Vec3d, 8> corners = box_corners(box);
    const double usable = std::max(0.05, 1.0 - 2.0 * margin);
    // first guess: orthographic extents on the camera plane
    {
        const Matrix4d v = cam.view();
        double ex = 0.0, ey = 0.0;
        double minx = 1e300, maxx = -1e300, miny = 1e300, maxy = -1e300;
        for (const Vec3d& c : corners) {
            const Vec3d e = (v * c.homogeneous()).head<3>();
            minx = std::min(minx, e.x()); maxx = std::max(maxx, e.x());
            miny = std::min(miny, e.y()); maxy = std::max(maxy, e.y());
            ex = std::max(ex, std::abs(e.x()));
            ey = std::max(ey, std::abs(e.y()));
        }
        if (free_target) {
            ex = 0.5 * (maxx - minx);
            ey = 0.5 * (maxy - miny);
        }
        ex = std::max(ex, 0.5);
        ey = std::max(ey, 0.5);
        cam.zoom = usable * std::min(0.5 * cam.width / ex, 0.5 * cam.height / ey);
    }
    // refine with the real projection (perspective makes the near side larger)
    for (int iter = 0; iter < 8; ++iter) {
        const Matrix4d vp = cam.projection(box) * cam.view();
        double minx = 1e300, maxx = -1e300, miny = 1e300, maxy = -1e300;
        bool behind = false;
        for (const Vec3d& c : corners) {
            const Vec4d h = vp * c.homogeneous();
            if (h.w() <= 1e-9) { behind = true; break; }
            minx = std::min(minx, h.x() / h.w()); maxx = std::max(maxx, h.x() / h.w());
            miny = std::min(miny, h.y() / h.w()); maxy = std::max(maxy, h.y() / h.w());
        }
        if (behind) {
            // the eye is inside or too close to the box: move it back
            cam.distance *= 2.0;
            continue;
        }
        double sx, sy, cx = 0.0, cy = 0.0;
        if (free_target) {
            cx = 0.5 * (minx + maxx);
            cy = 0.5 * (miny + maxy);
            sx = 0.5 * (maxx - minx);
            sy = 0.5 * (maxy - miny);
        } else {
            sx = std::max(std::abs(minx), std::abs(maxx));
            sy = std::max(std::abs(miny), std::abs(maxy));
        }
        const double s = std::max(std::max(sx, sy), 1e-6) / usable;
        if (free_target) {
            // ndc offset -> world offset on the target plane
            const Matrix4d v = cam.view();
            const Vec3d right = v.block<1, 3>(0, 0).transpose();
            const Vec3d upv   = v.block<1, 3>(1, 0).transpose();
            cam.target += right * (cx * 0.5 * cam.width / cam.zoom) + upv * (cy * 0.5 * cam.height / cam.zoom);
        }
        cam.zoom /= s;
        if (std::abs(s - 1.0) < 1e-3 && std::abs(cx) < 1e-3 && std::abs(cy) < 1e-3)
            break;
    }
}

// ---------------------------------------------------------------------------------------------
// GL state kept across a render, so the canvases find the context as they left it.
// ---------------------------------------------------------------------------------------------

struct GLStateGuard
{
    GLint viewport[4]{ 0, 0, 0, 0 };
    GLint draw_fbo{ 0 }, read_fbo{ 0 };
    GLint program{ 0 };
    GLint active_texture{ 0 };
    GLint vao{ 0 };
    GLint blend_src_rgb{ 0 }, blend_dst_rgb{ 0 }, blend_src_a{ 0 }, blend_dst_a{ 0 };
    GLfloat clear_color[4]{ 0, 0, 0, 0 };
    GLboolean depth_mask{ GL_TRUE };
    bool depth_test{ false }, blend{ false }, cull{ false }, scissor{ false }, multisample{ false };

    GLStateGuard()
    {
        glsafe(::glGetIntegerv(GL_VIEWPORT, viewport));
        glsafe(::glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo));
        glsafe(::glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fbo));
        glsafe(::glGetIntegerv(GL_CURRENT_PROGRAM, &program));
        glsafe(::glGetIntegerv(GL_ACTIVE_TEXTURE, &active_texture));
        glsafe(::glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao));
        glsafe(::glGetIntegerv(GL_BLEND_SRC_RGB, &blend_src_rgb));
        glsafe(::glGetIntegerv(GL_BLEND_DST_RGB, &blend_dst_rgb));
        glsafe(::glGetIntegerv(GL_BLEND_SRC_ALPHA, &blend_src_a));
        glsafe(::glGetIntegerv(GL_BLEND_DST_ALPHA, &blend_dst_a));
        glsafe(::glGetFloatv(GL_COLOR_CLEAR_VALUE, clear_color));
        glsafe(::glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_mask));
        depth_test  = ::glIsEnabled(GL_DEPTH_TEST) == GL_TRUE;
        blend       = ::glIsEnabled(GL_BLEND) == GL_TRUE;
        cull        = ::glIsEnabled(GL_CULL_FACE) == GL_TRUE;
        scissor     = ::glIsEnabled(GL_SCISSOR_TEST) == GL_TRUE;
#if !SLIC3R_OPENGL_ES
        multisample = ::glIsEnabled(GL_MULTISAMPLE) == GL_TRUE;
#endif
    }
    ~GLStateGuard()
    {
        auto set = [](GLenum cap, bool on) { if (on) ::glEnable(cap); else ::glDisable(cap); };
        ::glBindFramebuffer(GL_DRAW_FRAMEBUFFER, GLuint(draw_fbo));
        ::glBindFramebuffer(GL_READ_FRAMEBUFFER, GLuint(read_fbo));
        ::glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        ::glUseProgram(GLuint(program));
        ::glActiveTexture(GLenum(active_texture));
        ::glBindVertexArray(GLuint(vao));
        ::glBlendFuncSeparate(GLenum(blend_src_rgb), GLenum(blend_dst_rgb), GLenum(blend_src_a), GLenum(blend_dst_a));
        ::glClearColor(clear_color[0], clear_color[1], clear_color[2], clear_color[3]);
        ::glDepthMask(depth_mask);
        set(GL_DEPTH_TEST, depth_test);
        set(GL_BLEND, blend);
        set(GL_CULL_FACE, cull);
        set(GL_SCISSOR_TEST, scissor);
#if !SLIC3R_OPENGL_ES
        set(GL_MULTISAMPLE, multisample);
#endif
        // swallow any error left by the restore, so the canvas's glsafe checks do not see ours
        while (::glGetError() != GL_NO_ERROR) {}
    }
};

// Multisampled colour + depth render target, resolved into a single-sample one for the read-back.
struct OffscreenTarget
{
    GLuint fbo{ 0 }, color_rb{ 0 }, depth_rb{ 0 };
    GLuint resolve_fbo{ 0 }, resolve_rb{ 0 };
    int samples{ 0 };

    bool create(int w, int h, int requested_samples, std::string& error)
    {
        samples = requested_samples > 1 ? requested_samples : 0;
        glsafe(::glGenFramebuffers(1, &fbo));
        glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, fbo));
        glsafe(::glGenRenderbuffers(1, &color_rb));
        glsafe(::glBindRenderbuffer(GL_RENDERBUFFER, color_rb));
        if (samples > 0)
            glsafe(::glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA8, w, h));
        else
            glsafe(::glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, w, h));
        glsafe(::glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color_rb));
        glsafe(::glGenRenderbuffers(1, &depth_rb));
        glsafe(::glBindRenderbuffer(GL_RENDERBUFFER, depth_rb));
        if (samples > 0)
            glsafe(::glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH_COMPONENT24, w, h));
        else
            glsafe(::glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h));
        glsafe(::glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth_rb));
        const GLenum draw_buffers[] = { GL_COLOR_ATTACHMENT0 };
        glsafe(::glDrawBuffers(1, draw_buffers));
        GLenum status = ::glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            error = "offscreen framebuffer incomplete (status 0x" + to_hex(status) + ")";
            return false;
        }
        if (samples > 0) {
            glsafe(::glGenFramebuffers(1, &resolve_fbo));
            glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, resolve_fbo));
            glsafe(::glGenRenderbuffers(1, &resolve_rb));
            glsafe(::glBindRenderbuffer(GL_RENDERBUFFER, resolve_rb));
            glsafe(::glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, w, h));
            glsafe(::glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, resolve_rb));
            status = ::glCheckFramebufferStatus(GL_FRAMEBUFFER);
            if (status != GL_FRAMEBUFFER_COMPLETE) {
                error = "resolve framebuffer incomplete (status 0x" + to_hex(status) + ")";
                return false;
            }
            glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, fbo));
        }
        glsafe(::glBindRenderbuffer(GL_RENDERBUFFER, 0));
        return true;
    }

    void read(int w, int h, std::vector<uint8_t>& gl_rows)
    {
        gl_rows.resize(size_t(w) * size_t(h) * 4);
        if (samples > 0) {
            glsafe(::glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo));
            glsafe(::glBindFramebuffer(GL_DRAW_FRAMEBUFFER, resolve_fbo));
            glsafe(::glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST));
            glsafe(::glBindFramebuffer(GL_READ_FRAMEBUFFER, resolve_fbo));
        } else
            glsafe(::glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo));
        GLint pack = 4;
        glsafe(::glGetIntegerv(GL_PACK_ALIGNMENT, &pack));
        glsafe(::glPixelStorei(GL_PACK_ALIGNMENT, 1));
        glsafe(::glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, gl_rows.data()));
        glsafe(::glPixelStorei(GL_PACK_ALIGNMENT, pack));
    }

    ~OffscreenTarget()
    {
        for (GLuint* rb : { &color_rb, &depth_rb, &resolve_rb })
            if (*rb != 0) ::glDeleteRenderbuffers(1, rb);
        for (GLuint* fb : { &fbo, &resolve_fbo })
            if (*fb != 0) ::glDeleteFramebuffers(1, fb);
    }

    static std::string to_hex(GLenum v)
    {
        char buf[16];
        snprintf(buf, sizeof(buf), "%X", unsigned(v));
        return buf;
    }
};

json vec_json(const Vec3d& v) { return json::array({ v.x(), v.y(), v.z() }); }

json box_json(const BoundingBoxf3& b)
{
    if (!b.defined)
        return nullptr;
    json j = json::object();
    j["min"] = vec_json(b.min);
    j["max"] = vec_json(b.max);
    return j;
}

json mat_json(const Matrix4d& m)
{
    // column-major, as OpenGL takes it
    json a = json::array();
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            a.push_back(m(r, c));
    return a;
}

BoundingBoxf3 to_box(const libvgcode::AABox& b)
{
    if (b[0][0] > b[1][0])
        return BoundingBoxf3();
    return BoundingBoxf3(Vec3d(b[0][0], b[0][1], b[0][2]), Vec3d(b[1][0], b[1][1], b[1][2]));
}

} // namespace

// =============================================================================================
// Impl
// =============================================================================================

struct PreviewRenderer::Impl
{
    libvgcode::Viewer viewer;
    bool viewer_initialized{ false };

    // what is loaded in the viewer
    bool loaded{ false };
    const GCodeProcessorResult* loaded_result{ nullptr };
    unsigned int loaded_result_id{ 0 };
    int loaded_plate{ -1 };
    std::vector<std::string> loaded_tool_colors;
    std::vector<std::string> loaded_color_print_colors;
    BoundingBoxf3 toolpaths_box; // all extrusions

    // plate drawing
    GLModel bed_fill, bed_grid, bed_outline;
    BoundingBoxf3 bed_box;

    // shells
    GLVolumeCollection shells;
    size_t shells_print_id{ size_t(-1) };
    size_t shells_print_timestamp{ 0 };
    int    shells_plate{ -1 };

    wxTimer idle_timer;
    int idle_seconds{ 300 };

    Stats stats;

    Impl()
    {
        idle_timer.Bind(wxEVT_TIMER, [this](wxTimerEvent&) { on_idle(); });
    }

    void on_idle()
    {
        BOOST_LOG_TRIVIAL(info) << "MCP preview renderer: idle, releasing the toolpaths";
        release_gpu(false);
    }

    static GLCanvas3D* make_context_current(Plater& plater)
    {
        for (GLCanvas3D* canvas : { plater.get_view3D_canvas3D(), plater.get_preview_canvas3D() })
            if (canvas != nullptr && canvas->make_current_for_postinit())
                return canvas;
        return nullptr;
    }

    void release_gpu(bool full)
    {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr || make_context_current(*plater) == nullptr) {
            // no context: forget the data without touching GL (only at shutdown, when it is lost anyway)
            return;
        }
        if (viewer_initialized) {
            if (full) {
                viewer.shutdown();
                viewer_initialized = false;
            } else
                viewer.reset();
        }
        loaded = false;
        loaded_result = nullptr;
        loaded_result_id = 0;
        loaded_plate = -1;
        toolpaths_box = BoundingBoxf3();
        bed_fill.reset();
        bed_grid.reset();
        bed_outline.reset();
        bed_box = BoundingBoxf3();
        shells.clear();
        shells_print_id = size_t(-1);
        shells_plate = -1;
        ++stats.releases;
        stats.loaded = false;
        stats.vertices = 0;
        stats.cpu_bytes = 0;
        stats.gpu_bytes = 0;
    }

    // ---------------------------------------------------------------------------------------
    void build_bed(const BoundingBoxf3& box)
    {
        if (bed_box.defined && box.defined && bed_box.min.isApprox(box.min) && bed_box.max.isApprox(box.max) && bed_fill.is_initialized())
            return;
        bed_box = box;
        const float x0 = float(box.min.x()), y0 = float(box.min.y()), x1 = float(box.max.x()), y1 = float(box.max.y());
        const float z = 0.0f;
        {
            GLModel::Geometry g;
            g.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3 };
            g.add_vertex(Vec3f(x0, y0, z));
            g.add_vertex(Vec3f(x1, y0, z));
            g.add_vertex(Vec3f(x1, y1, z));
            g.add_vertex(Vec3f(x0, y1, z));
            g.add_triangle(0, 1, 2);
            g.add_triangle(0, 2, 3);
            bed_fill.reset();
            bed_fill.init_from(std::move(g));
        }
        {
            GLModel::Geometry g;
            g.format = { GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3 };
            unsigned int id = 0;
            const float step = 10.0f;
            for (float x = std::ceil(x0 / step) * step; x <= x1 + 1e-3f; x += step) {
                g.add_vertex(Vec3f(x, y0, z)); g.add_vertex(Vec3f(x, y1, z)); g.add_line(id, id + 1); id += 2;
            }
            for (float y = std::ceil(y0 / step) * step; y <= y1 + 1e-3f; y += step) {
                g.add_vertex(Vec3f(x0, y, z)); g.add_vertex(Vec3f(x1, y, z)); g.add_line(id, id + 1); id += 2;
            }
            bed_grid.reset();
            bed_grid.init_from(std::move(g));
        }
        {
            GLModel::Geometry g;
            g.format = { GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3 };
            g.add_vertex(Vec3f(x0, y0, z));
            g.add_vertex(Vec3f(x1, y0, z));
            g.add_vertex(Vec3f(x1, y1, z));
            g.add_vertex(Vec3f(x0, y1, z));
            g.add_line(0, 1); g.add_line(1, 2); g.add_line(2, 3); g.add_line(3, 0);
            bed_outline.reset();
            bed_outline.init_from(std::move(g));
        }
    }

    void render_bed(const Matrix4d& view, const Matrix4d& proj, const std::array<float, 4>& bg)
    {
        GLShaderProgram* shader = wxGetApp().get_shader("flat");
        if (shader == nullptr || !bed_fill.is_initialized())
            return;
        // The plate is a backdrop: drawn first and without depth writes, so it never hides a toolpath
        // (from below the toolpaths show through it).
        glsafe(::glDepthMask(GL_FALSE));
        glsafe(::glDisable(GL_DEPTH_TEST));
        glsafe(::glDisable(GL_CULL_FACE));
        glsafe(::glEnable(GL_BLEND));
        glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
        shader->start_using();
        shader->set_uniform("view_model_matrix", Transform3d(view));
        shader->set_uniform("projection_matrix", Transform3d(proj));
        // darker than a light background, lighter than a dark one
        const float lum = 0.299f * bg[0] + 0.587f * bg[1] + 0.114f * bg[2];
        const float k = lum > 0.5f ? -1.0f : 1.0f;
        auto shade = [&](float d) { return ColorRGBA(std::clamp(bg[0] + k * d, 0.f, 1.f), std::clamp(bg[1] + k * d, 0.f, 1.f), std::clamp(bg[2] + k * d, 0.f, 1.f), 1.0f); };
        bed_fill.set_color(shade(0.10f));
        bed_fill.render();
        bed_grid.set_color(shade(0.22f));
        bed_grid.render();
        bed_outline.set_color(shade(0.45f));
        bed_outline.render();
        shader->stop_using();
        glsafe(::glDepthMask(GL_TRUE));
    }

    // ---------------------------------------------------------------------------------------
    void load_shells(int plate_idx, PartPlate& plate)
    {
        PrintBase* print_base = nullptr;
        GCodeResult* result = nullptr;
        int index = -1;
        plate.get_print(&print_base, &result, &index);
        const Print* print = dynamic_cast<const Print*>(print_base);
        if (print == nullptr) {
            shells.clear();
            shells_plate = plate_idx;
            return;
        }
        if (shells_plate == plate_idx && shells_print_id == print->id().id && shells_print_timestamp == print->get_modified_count())
            return;
        shells.clear();
        shells.set_show_sinking_contours(false);
        const ModelObjectPtrs& model_objs = wxGetApp().model().objects;
        for (const PrintObject* obj : print->objects()) {
            const ModelObject* model_obj = obj->model_object();
            int object_idx = -1;
            for (int i = 0; i < int(model_objs.size()); ++i)
                if (model_objs[i]->id() == model_obj->id()) { object_idx = i; break; }
            if (object_idx == -1)
                continue;
            std::vector<int> instance_ids;
            for (int i = 0; i < int(model_obj->instances.size()); ++i)
                if (model_obj->instances[i]->is_printable())
                    instance_ids.push_back(i);
            const size_t first = shells.volumes.size();
            shells.load_object(model_obj, object_idx, instance_ids, "object", true, false);
            const SlicingParameters& sp = obj->slicing_parameters();
            if (sp.object_print_z_min != 0.0) {
                const Vec3d z_offset = sp.object_print_z_min * Vec3d::UnitZ();
                for (size_t i = first; i < shells.volumes.size(); ++i) {
                    GLVolume* v = shells.volumes[i];
                    v->set_volume_offset(v->get_volume_offset() + v->get_instance_transformation().get_matrix_no_offset().inverse() * z_offset);
                }
            }
        }
        shells.volumes.erase(std::remove_if(shells.volumes.begin(), shells.volumes.end(), [](GLVolume* v) {
            if (v->is_modifier) { delete v; return true; }
            return false;
        }), shells.volumes.end());
        for (GLVolume* v : shells.volumes) {
            v->zoom_to_volumes = false;
            v->color.a(0.25f);
            v->force_native_color = true;
            v->set_render_color();
        }
        shells_plate = plate_idx;
        shells_print_id = print->id().id;
        shells_print_timestamp = print->get_modified_count();
    }

    void render_shells(const Matrix4d& view, const Matrix4d& proj, double near_z, double far_z, int w, int h)
    {
        if (shells.volumes.empty())
            return;
        GLShaderProgram* shader = wxGetApp().get_shader("gouraud_light");
        if (shader == nullptr)
            return;
        glsafe(::glEnable(GL_DEPTH_TEST));
        glsafe(::glDepthMask(GL_FALSE));
        shader->start_using();
        shader->set_uniform("emission_factor", 0.1f);
        shader->set_uniform("z_far", far_z);
        shader->set_uniform("z_near", near_z);
        shells.render(GLVolumeCollection::ERenderType::Transparent, false, Transform3d(view), Transform3d(proj), { w, h });
        shader->set_uniform("emission_factor", 0.0f);
        shader->stop_using();
        glsafe(::glDepthMask(GL_TRUE));
    }

    BoundingBoxf3 shells_box() const
    {
        BoundingBoxf3 b;
        for (const GLVolume* v : shells.volumes)
            b.merge(v->transformed_bounding_box());
        return b;
    }
};

// =============================================================================================

PreviewRenderer& PreviewRenderer::instance()
{
    // Never destroyed: the viewer's GL objects can only be freed with the context current, which a
    // static destructor at exit cannot guarantee. shutdown() frees them while the windows exist.
    static PreviewRenderer* s_instance = new PreviewRenderer();
    return *s_instance;
}

PreviewRenderer::PreviewRenderer() : p(std::make_unique<Impl>()) {}
PreviewRenderer::~PreviewRenderer() = default;

void PreviewRenderer::release()
{
    p->idle_timer.Stop();
    p->release_gpu(false);
}

void PreviewRenderer::shutdown()
{
    p->idle_timer.Stop();
    p->release_gpu(true);
}

void PreviewRenderer::set_idle_release_seconds(int seconds)
{
    p->idle_seconds = std::max(0, seconds);
    if (p->idle_seconds == 0)
        p->idle_timer.Stop();
}

PreviewRenderer::Stats PreviewRenderer::stats() const { return p->stats; }

const std::vector<std::string>& PreviewRenderer::view_type_names()
{
    static const std::vector<std::string> names = [] {
        std::vector<std::string> n;
        for (const ViewTypeName& v : view_type_table()) n.emplace_back(v.name);
        return n;
    }();
    return names;
}

const std::vector<std::string>& PreviewRenderer::option_names()
{
    static const std::vector<std::string> names = [] {
        std::vector<std::string> n;
        for (const OptionName& o : option_table()) n.emplace_back(o.name);
        return n;
    }();
    return names;
}

std::vector<std::string> PreviewRenderer::feature_keys()
{
    std::vector<std::string> keys;
    for (size_t i = 1; i < libvgcode::GCODE_EXTRUSION_ROLES_COUNT; ++i)
        keys.emplace_back(feature_key(static_cast<libvgcode::EGCodeExtrusionRole>(i)));
    return keys;
}

PreviewImage PreviewRenderer::render(const PreviewRequest& req)
{
    const Clock::time_point t_start = Clock::now();
    PreviewImage out;
    Impl& d = *p;
    auto fail = [&out](const char* code, std::string message) -> PreviewImage& {
        out.ok = false;
        out.error_code = code;
        out.error_message = std::move(message);
        BOOST_LOG_TRIVIAL(warning) << "MCP preview renderer: " << out.error_code << ": " << out.error_message;
        return out;
    };

    // ---- arguments that need no application state
    const ViewTypeName* view_type = find_view_type(req.view_type);
    if (view_type == nullptr)
        return fail("invalid_argument", "unknown view_type \"" + req.view_type + "\"");
    if (req.width < 16 || req.height < 16)
        return fail("invalid_argument", "image size must be at least 16 x 16");
    if (req.time_mode != "normal" && req.time_mode != "silent" && req.time_mode != "stealth")
        return fail("invalid_argument", "time_mode must be \"normal\" or \"silent\"");
    for (const auto& option : req.options) {
        const std::string& name = option.first; // not a structured binding: lambdas capture those only from C++20
        if (std::none_of(option_table().begin(), option_table().end(), [&name](const OptionName& o) { return name == o.name; }))
            return fail("invalid_argument", "unknown option \"" + name + "\"");
    }
    for (const auto& [key, on] : req.features) {
        (void)on;
        libvgcode::EGCodeExtrusionRole role;
        if (!find_feature(key, role))
            return fail("invalid_argument", "unknown feature \"" + key + "\"");
    }

    // ---- application state
    if (wxTheApp == nullptr || wxGetApp().plater() == nullptr || wxGetApp().is_closing())
        return fail("gl_unavailable", "the application window is not available");
    if (!wxIsMainThread())
        return fail("render_failed", "render() must be called on the main thread");
    Plater& plater = *wxGetApp().plater();
    PartPlateList& plates = plater.get_partplate_list();
    const int plate_idx = req.plate < 0 ? plates.get_curr_plate_index() : req.plate;
    PartPlate* plate = plates.get_plate(plate_idx);
    if (plate == nullptr || plate_idx >= plates.get_plate_count())
        return fail("stale_id", "there is no plate " + std::to_string(plate_idx + 1) + " (the project has " + std::to_string(plates.get_plate_count()) + ")");

    const GCodeProcessorResult* result = req.result != nullptr ? req.result : plate->get_slice_result();
    if (req.result == nullptr && plater.is_background_process_slicing() && !plate->is_slice_result_valid())
        return fail("not_sliced", "plate " + std::to_string(plate_idx + 1) + " is being sliced; render again when slicing has finished");
    if (result == nullptr || result->moves.empty())
        return fail("not_sliced", "plate " + std::to_string(plate_idx + 1) + " has no slice result; slice it first");
    if (req.result == nullptr && !plate->is_slice_result_valid())
        return fail("not_sliced", "the slice result of plate " + std::to_string(plate_idx + 1) +
                                  " is out of date (the plate changed since it was sliced, or slicing is running); slice it again");

    // ---- GL
    GLCanvas3D* canvas = Impl::make_context_current(plater);
    if (canvas == nullptr)
        return fail("gl_unavailable", "could not make the OpenGL context current");
    if (!OpenGLManager::are_framebuffers_supported())
        return fail("gl_unavailable", "framebuffer objects are not supported by this OpenGL driver");
    while (::glGetError() != GL_NO_ERROR) {}

    GLint max_rb = 0, max_vp[2] = { 0, 0 }, max_samples = 0;
    glsafe(::glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &max_rb));
    glsafe(::glGetIntegerv(GL_MAX_VIEWPORT_DIMS, max_vp));
    glsafe(::glGetIntegerv(GL_MAX_SAMPLES, &max_samples));
    const int max_size = std::min({ int(max_rb), int(max_vp[0]), int(max_vp[1]), 16384 });
    if (int(req.width) > max_size || int(req.height) > max_size)
        return fail("invalid_argument", "image size is limited to " + std::to_string(max_size) + " pixels per side on this GPU");
    if (double(req.width) * double(req.height) > 64.0 * 1024.0 * 1024.0)
        return fail("invalid_argument", "image size is limited to 64 megapixels");

    GLStateGuard gl_state;

    if (!d.viewer_initialized) {
        try {
            d.viewer.init(reinterpret_cast<const char*>(::glGetString(GL_VERSION)));
            d.viewer_initialized = true;
        } catch (const std::exception& e) {
            return fail("gl_unavailable", std::string("could not initialise the toolpath viewer: ") + e.what());
        }
    }

    // ---- load, or reuse what is loaded
    const std::vector<std::string> tool_colors = plater.get_extruder_colors_from_plater_config(result);
    std::vector<std::string> color_print_colors;
    {
        const Model& model = plater.model();
        auto it = model.plates_custom_gcodes.find(plate_idx);
        const std::vector<CustomGCode::Item>* items = it != model.plates_custom_gcodes.end() ? &it->second.gcodes : nullptr;
        if (items != nullptr && !items->empty()) {
            color_print_colors = tool_colors;
            for (const CustomGCode::Item& item : *items)
                if (item.type == CustomGCode::ColorChange)
                    color_print_colors.emplace_back(item.color);
            color_print_colors.push_back("#808080"); // pause print or custom G-code, as the Preview tab
        }
    }

    bool cache_hit = true;
    double convert_ms = 0.0, upload_ms = 0.0;
    if (!d.loaded || d.loaded_result != result || d.loaded_result_id != result->id || d.loaded_plate != plate_idx) {
        cache_hit = false;
        const Clock::time_point t0 = Clock::now();
        libvgcode::GCodeInputData data;
        {
            result->lock();
            try {
                data = libvgcode::convert(*result, tool_colors, color_print_colors, d.viewer, nullptr);
            } catch (...) {
                result->unlock();
                return fail("render_failed", "could not convert the slice result");
            }
            result->unlock();
        }
        convert_ms = ms_since(t0);
        const Clock::time_point t1 = Clock::now();
        d.viewer.reset();
        d.viewer.reset_default_extrusion_roles_colors();
        d.viewer.load(std::move(data));
        upload_ms = ms_since(t1);
        d.loaded = true;
        d.loaded_result = result;
        d.loaded_result_id = result->id;
        d.loaded_plate = plate_idx;
        d.loaded_tool_colors = tool_colors;
        d.loaded_color_print_colors = color_print_colors;
        d.toolpaths_box = to_box(d.viewer.get_extrusion_bounding_box());
        ++d.stats.loads;
        d.stats.last_load_ms = convert_ms + upload_ms;
    } else {
        ++d.stats.cache_hits;
        if (d.loaded_tool_colors != tool_colors) {
            d.viewer.set_tool_colors(libvgcode::convert_filament_colors(tool_colors));
            d.loaded_tool_colors = tool_colors;
        }
        if (d.loaded_color_print_colors != color_print_colors) {
            d.viewer.set_color_print_colors(libvgcode::convert_filament_colors(color_print_colors.empty() ? tool_colors : color_print_colors));
            d.loaded_color_print_colors = color_print_colors;
        }
    }
    libvgcode::Viewer& viewer = d.viewer;

    const size_t layers_count = viewer.get_layers_count();
    if (layers_count == 0)
        return fail("not_sliced", "the slice result of plate " + std::to_string(plate_idx + 1) + " has no layers");

    // ---- settings of this request
    const Clock::time_point t_settings = Clock::now();
    viewer.set_view_type(view_type->type);
    {
        const libvgcode::ETimeMode mode = req.time_mode == "normal" ? libvgcode::ETimeMode::Normal : libvgcode::ETimeMode::Stealth;
        if (viewer.get_time_mode() != mode)
            viewer.set_time_mode(mode);
    }
    viewer.set_translucent_filaments(req.translucent_filaments);

    for (const OptionName& o : option_table()) {
        bool want = o.type == libvgcode::EOptionType::Seams; // the viewer's default
        auto it = req.options.find(o.name);
        if (it != req.options.end())
            want = it->second;
        if (viewer.is_option_visible(o.type) != want)
            viewer.toggle_option_visibility(o.type);
    }
    {
        std::array<bool, libvgcode::GCODE_EXTRUSION_ROLES_COUNT> want;
        want.fill(true);
        using R = libvgcode::EGCodeExtrusionRole;
        if (req.show_supports.has_value())
            for (R r : { R::SupportMaterial, R::SupportMaterialInterface, R::SupportTransition })
                want[size_t(r)] = *req.show_supports;
        if (req.show_wipe_tower.has_value())
            want[size_t(R::WipeTower)] = *req.show_wipe_tower;
        for (const auto& [key, on] : req.features) {
            R role;
            if (find_feature(key, role))
                want[size_t(role)] = on;
        }
        for (size_t i = 0; i < want.size(); ++i)
            if (viewer.is_extrusion_role_visible(R(i)) != want[i])
                viewer.toggle_extrusion_role_visibility(R(i));
    }

    // layers
    const std::vector<float> zs = viewer.get_layers_zs();
    size_t lmin = 0, lmax = layers_count - 1;
    if (req.z_min.has_value()) {
        auto it = std::lower_bound(zs.begin(), zs.end(), float(*req.z_min) - 1e-4f);
        lmin = size_t(std::distance(zs.begin(), it));
    }
    if (req.z_max.has_value()) {
        auto it = std::upper_bound(zs.begin(), zs.end(), float(*req.z_max) + 1e-4f);
        lmax = it == zs.begin() ? size_t(0) : size_t(std::distance(zs.begin(), it)) - 1;
        if (it == zs.begin())
            return fail("invalid_argument", "z_range.max is below the first layer (z " + std::to_string(zs.front()) + ")");
    }
    if (req.layer_min.has_value()) {
        if (*req.layer_min < 1 || size_t(*req.layer_min) > layers_count)
            return fail("invalid_argument", "layers.from must be in 1.." + std::to_string(layers_count));
        lmin = std::max(lmin, size_t(*req.layer_min - 1));
    }
    if (req.layer_max.has_value()) {
        if (*req.layer_max < 1 || size_t(*req.layer_max) > layers_count)
            return fail("invalid_argument", "layers.to must be in 1.." + std::to_string(layers_count));
        lmax = std::min(lmax, size_t(*req.layer_max - 1));
    }
    if (lmin >= layers_count)
        return fail("invalid_argument", "the requested range starts above the last layer");
    if (req.top_layer_only)
        lmin = lmax;
    if (lmin > lmax)
        return fail("invalid_argument", "the layer range is empty (from " + std::to_string(lmin + 1) + " > to " + std::to_string(lmax + 1) + ")");

    if (viewer.is_top_layer_only_view_range() != req.dim_lower_layers)
        viewer.toggle_top_layer_only_view_range();
    viewer.set_dim_previous_layers(req.dim_lower_layers);
    viewer.set_dim_previous_layers_brightness(req.dim_brightness);
    viewer.set_layers_view_range(libvgcode::Interval::value_type(lmin), libvgcode::Interval::value_type(lmax));

    // moves within the top layer
    const libvgcode::Interval enabled = viewer.get_view_enabled_range();
    size_t top_first = enabled[0];
    for (size_t i = enabled[1]; i > enabled[0]; --i) {
        if (viewer.get_vertex_at(i - 1).layer_id < lmax) { top_first = i; break; }
    }
    const size_t top_count = enabled[1] >= top_first ? enabled[1] - top_first : 0;
    size_t vis_min = enabled[0], vis_max = enabled[1];
    if (req.moves_from.has_value() || req.moves_to.has_value()) {
        const double from = std::clamp(req.moves_from.value_or(0.0), 0.0, 1.0);
        const double to   = std::clamp(req.moves_to.value_or(1.0), 0.0, 1.0);
        if (from > to)
            return fail("invalid_argument", "moves.from must not be greater than moves.to");
        vis_max = top_first + size_t(std::llround(to * double(top_count)));
        if (lmin == lmax || req.dim_lower_layers)
            vis_min = std::max(enabled[0], top_first + size_t(std::llround(from * double(top_count))));
        vis_min = std::min(vis_min, vis_max);
        viewer.set_view_visible_range(libvgcode::Interval::value_type(vis_min), libvgcode::Interval::value_type(vis_max));
    }
    const libvgcode::Interval visible = viewer.get_view_visible_range();

    // box of what is drawn: the extrusions of the visible range (all of them with dim_lower_layers)
    BoundingBoxf3 visible_box;
    {
        const size_t first = req.dim_lower_layers ? viewer.get_view_full_range()[0] : visible[0];
        for (size_t i = first; i <= visible[1] && i < viewer.get_vertices_count(); ++i) {
            const libvgcode::PathVertex& v = viewer.get_vertex_at(i);
            if (!v.is_extrusion() || !viewer.is_extrusion_role_visible(v.role))
                continue;
            const Vec3d pos(v.position[0], v.position[1], v.position[2]);
            visible_box.merge(pos);
            // the bottom of the line
            visible_box.merge(Vec3d(pos.x(), pos.y(), pos.z() - v.height));
        }
        if (visible_box.defined) {
            // half a line width around
            visible_box.min -= Vec3d(0.25, 0.25, 0.0);
            visible_box.max += Vec3d(0.25, 0.25, 0.0);
        }
    }
    const double settings_ms = ms_since(t_settings);

    // ---- boxes and camera
    BoundingBoxf3 plate_box = plate->get_bounding_box(false);
    plate_box.min.z() = 0.0;
    plate_box.max.z() = 0.0;
    if (req.show_shells)
        d.load_shells(plate_idx, *plate);

    BoundingBoxf3 fit_box;
    switch (req.camera.fit) {
    case PreviewFit::VisibleToolpaths: fit_box = visible_box.defined ? visible_box : d.toolpaths_box; break;
    case PreviewFit::Toolpaths:        fit_box = d.toolpaths_box; break;
    case PreviewFit::Plate: {
        fit_box = plate_box;
        fit_box.max.z() = d.toolpaths_box.defined ? d.toolpaths_box.max.z() : 0.0;
        break;
    }
    case PreviewFit::Box:
        if (!req.camera.box.defined)
            return fail("invalid_argument", "camera.fit is \"box\" but camera.box is missing");
        fit_box = req.camera.box;
        break;
    case PreviewFit::None: break;
    }
    if (!fit_box.defined && req.camera.fit != PreviewFit::None)
        fit_box = plate_box;

    std::string view_name = req.camera.view;
    if (view_name == "rear")
        view_name = "back";
    double az = req.camera.azimuth_deg, el = req.camera.elevation_deg;
    bool axis_view = false;
    if (!view_name.empty()) {
        static const std::map<std::string, std::pair<double, double>> named = {
            { "iso", { 45.0, 45.0 } }, { "top", { 0.0, 90.0 } }, { "bottom", { 0.0, -90.0 } }, { "front", { 0.0, 0.0 } },
            { "back", { 180.0, 0.0 } }, { "left", { 90.0, 0.0 } }, { "right", { 270.0, 0.0 } },
        };
        auto it = named.find(view_name);
        if (it == named.end())
            return fail("invalid_argument", "unknown camera view \"" + req.camera.view + "\" (iso, top, bottom, front, back, left, right)");
        az = it->second.first;
        el = it->second.second;
        axis_view = view_name != "iso";
    } else if (!std::isfinite(az) || !std::isfinite(el) || el < -90.0 || el > 90.0)
        return fail("invalid_argument", "camera.elevation_deg must be in -90..90 and azimuth_deg finite");

    CameraState cam;
    cam.width = int(req.width);
    cam.height = int(req.height);
    cam.perspective = req.camera.projection == PreviewProjection::Perspective ||
                      (req.camera.projection == PreviewProjection::Auto && !axis_view);
    cam.dir = orbit_direction(az, el);
    cam.up = orbit_up(az, el);
    if (req.camera.distance.has_value()) {
        if (!(*req.camera.distance > 0.0))
            return fail("invalid_argument", "camera.distance must be positive");
        cam.distance = *req.camera.distance;
    } else {
        cam.distance = std::max(1000.0, fit_box.defined ? 3.0 * fit_box.size().norm() : 0.0);
    }
    cam.target = req.camera.target.has_value() ? *req.camera.target : (fit_box.defined ? fit_box.center() : plate_box.center());
    if (req.camera.zoom.has_value()) {
        if (!(*req.camera.zoom > 0.0))
            return fail("invalid_argument", "camera.zoom must be positive");
        cam.zoom = *req.camera.zoom;
    } else if (fit_box.defined && req.camera.fit != PreviewFit::None) {
        fit_camera(cam, fit_box, std::clamp(req.camera.margin, 0.0, 0.45), !req.camera.target.has_value());
    } else
        cam.zoom = 1.0;

    BoundingBoxf3 scene_box = d.toolpaths_box;
    scene_box.merge(fit_box);
    if (req.show_bed)
        scene_box.merge(plate_box);
    if (req.show_shells)
        scene_box.merge(d.shells_box());
    if (!scene_box.defined)
        scene_box = BoundingBoxf3(cam.target - Vec3d::Ones(), cam.target + Vec3d::Ones());

    double near_z = 0.0, far_z = 0.0;
    const Matrix4d view = cam.view();
    const Matrix4d proj = cam.projection(scene_box, &near_z, &far_z);

    PreviewCameraInfo& ci = out.camera;
    ci.projection = cam.perspective ? "perspective" : "orthographic";
    ci.position = cam.eye();
    ci.target = cam.target;
    ci.up = view.block<1, 3>(1, 0).transpose();
    ci.forward = -cam.dir;
    ci.azimuth_deg = az;
    ci.elevation_deg = el;
    ci.zoom = cam.zoom;
    ci.fov_deg = cam.perspective ? 2.0 * std::atan(0.5 * cam.height / cam.zoom / cam.distance) * 180.0 / M_PI : 0.0;
    ci.near_z = near_z;
    ci.far_z = far_z;
    ci.viewport = { 0, 0, cam.width, cam.height };
    ci.view = view;
    ci.projection_matrix = proj;
    ci.view_projection = proj * view;

    // ---- draw
    const Clock::time_point t_render = Clock::now();
    // Multisampling of the offscreen target is independent of the window's pixel format
    // (OpenGLManager::can_multisample() is about the latter).
    int samples = 0;
    if (max_samples > 1)
        samples = req.samples < 0 ? std::min(8, int(max_samples)) : std::min(req.samples, int(max_samples));
    // Keep the multisampled colour + depth (8 bytes per sample) within a GPU memory budget: a
    // 4096 x 4096 picture at 8 samples would take 1 GiB, 8192 x 8192 4 GiB.
    {
        const double budget = 512.0 * 1024.0 * 1024.0;
        const double per_sample = double(cam.width) * double(cam.height) * 8.0;
        while (samples >= 2 && per_sample * samples > budget)
            samples /= 2;
    }
    if (samples < 2)
        samples = 0;
    auto target = std::make_unique<OffscreenTarget>();
    std::string fb_error;
    if (!target->create(cam.width, cam.height, samples, fb_error)) {
        BOOST_LOG_TRIVIAL(warning) << "MCP preview renderer: " << fb_error << ", retrying without multisampling";
        target = std::make_unique<OffscreenTarget>();
        samples = 0;
        if (!target->create(cam.width, cam.height, 0, fb_error))
            return fail("gl_unavailable", fb_error);
    }
    glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, target->fbo));
    glsafe(::glViewport(0, 0, cam.width, cam.height));
    glsafe(::glDisable(GL_SCISSOR_TEST));
#if !SLIC3R_OPENGL_ES
    if (samples > 0)
        glsafe(::glEnable(GL_MULTISAMPLE));
#endif
    glsafe(::glClearColor(req.background[0], req.background[1], req.background[2], req.background[3]));
    glsafe(::glDepthMask(GL_TRUE));
    glsafe(::glClearDepth(1.0));
    glsafe(::glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT));

    if (req.show_bed) {
        d.build_bed(plate->get_bounding_box(false));
        d.render_bed(view, proj, req.background);
    }

    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glDepthFunc(GL_LESS));
    glsafe(::glDisable(GL_BLEND));
    viewer.set_shadow_map(4, libvgcode::Mat4x4{ 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 }, 0.0f, 0.0f);
    viewer.set_tone(1.0f, 1.15f); // as the Preview tab without the realistic view
    viewer.set_light_top_dir({ -0.4574957f, 0.4574957f, 0.7624929f });
    viewer.set_gloss(false);
    viewer.set_clipping_plane({ 0.0f, 0.0f, 0.0f, 1.0f });
    const libvgcode::Mat4x4 vview = libvgcode::convert(static_cast<Matrix4f>(view.cast<float>()));
    const libvgcode::Mat4x4 vproj = libvgcode::convert(static_cast<Matrix4f>(proj.cast<float>()));
    viewer.render(vview, vproj);
    if (viewer.has_translucent_toolpaths())
        viewer.render_translucent(vview, vproj);

    if (req.show_shells)
        d.render_shells(view, proj, near_z, far_z, cam.width, cam.height);

    std::vector<uint8_t> gl_rows;
    target->read(cam.width, cam.height, gl_rows);
    GLenum gl_err = ::glGetError();
    std::string gl_err_text;
    while (gl_err != GL_NO_ERROR) {
        gl_err_text += (gl_err_text.empty() ? "" : ", ") + std::string("0x") + OffscreenTarget::to_hex(gl_err);
        gl_err = ::glGetError();
    }
    const double render_ms = ms_since(t_render);

    // ---- pixels, PNG
    const Clock::time_point t_encode = Clock::now();
    const size_t row = size_t(cam.width) * 4;
    if (req.background[3] >= 1.0f)
        for (size_t i = 3; i < gl_rows.size(); i += 4)
            gl_rows[i] = 255;
    out.width = req.width;
    out.height = req.height;
    out.rgba.resize(gl_rows.size());
    for (int y = 0; y < cam.height; ++y)
        std::memcpy(out.rgba.data() + size_t(y) * row, gl_rows.data() + size_t(cam.height - 1 - y) * row, row);
    if (req.encode_png) {
        size_t png_size = 0;
        void* png = tdefl_write_image_to_png_file_in_memory_ex(gl_rows.data(), cam.width, cam.height, 4, &png_size, MZ_DEFAULT_LEVEL, 1);
        if (png == nullptr)
            return fail("render_failed", "PNG encoding failed");
        out.png.assign(static_cast<const char*>(png), png_size);
        mz_free(png);
    }
    const double encode_ms = ms_since(t_encode);

    // ---- legend
    PreviewLegend& lg = out.legend;
    lg.view_type = view_type->name;
    lg.title = view_type->title;
    lg.unit = view_type->unit;
    switch (view_type->type) {
    case libvgcode::EViewType::FeatureType: {
        lg.kind = "categories";
        double total = 0.0;
        const std::vector<libvgcode::EGCodeExtrusionRole> roles = viewer.get_extrusion_roles();
        for (libvgcode::EGCodeExtrusionRole r : roles)
            total += viewer.get_extrusion_role_estimated_time(r);
        for (libvgcode::EGCodeExtrusionRole r : roles) {
            PreviewLegendItem it;
            it.key = feature_key(r);
            it.label = feature_label(r);
            it.color = to_rgb(viewer.get_extrusion_role_color(r));
            it.time_s = viewer.get_extrusion_role_estimated_time(r);
            if (total > 0.0)
                it.percent = 100.0 * *it.time_s / total;
            it.visible = viewer.is_extrusion_role_visible(r);
            lg.items.push_back(std::move(it));
        }
        break;
    }
    case libvgcode::EViewType::Tool: {
        lg.kind = "categories";
        const libvgcode::PaletteWithAlpha& colors = viewer.get_tool_colors();
        for (uint8_t id : viewer.get_used_extruders_ids()) {
            PreviewLegendItem it;
            it.key = "filament_" + std::to_string(int(id) + 1);
            it.label = "Filament " + std::to_string(int(id) + 1);
            if (id < colors.size())
                it.color = { colors[id][0], colors[id][1], colors[id][2] };
            lg.items.push_back(std::move(it));
        }
        break;
    }
    case libvgcode::EViewType::ColorPrint:
    case libvgcode::EViewType::Summary: {
        lg.kind = "categories";
        const libvgcode::PaletteWithAlpha& colors = viewer.get_color_print_colors();
        for (uint8_t id : viewer.get_used_extruders_ids()) {
            const std::vector<libvgcode::ColorPrint> cps = viewer.get_color_prints(id);
            if (cps.empty()) {
                const libvgcode::PaletteWithAlpha& tc = viewer.get_tool_colors();
                PreviewLegendItem it;
                it.key = "filament_" + std::to_string(int(id) + 1);
                it.label = "Filament " + std::to_string(int(id) + 1);
                if (id < tc.size())
                    it.color = { tc[id][0], tc[id][1], tc[id][2] };
                lg.items.push_back(std::move(it));
                continue;
            }
            for (const libvgcode::ColorPrint& cp : cps) {
                PreviewLegendItem it;
                it.key = "filament_" + std::to_string(int(id) + 1) + "_color_" + std::to_string(int(cp.color_id));
                it.label = "Filament " + std::to_string(int(id) + 1) + " from layer " + std::to_string(cp.layer_id + 1);
                if (!colors.empty()) {
                    const libvgcode::ColorWithAlpha& c = colors[size_t(cp.color_id) % colors.size()];
                    it.color = { c[0], c[1], c[2] };
                }
                it.time_s = cp.times[req.time_mode == "normal" ? 0 : 1];
                lg.items.push_back(std::move(it));
            }
        }
        break;
    }
    default: {
        lg.kind = "range";
        lg.logarithmic = view_type->type == libvgcode::EViewType::LayerTimeLogarithmic;
        const libvgcode::ColorRange& cr = viewer.get_color_range(view_type->type);
        const std::array<float, 2>& r = cr.get_range();
        if (r[0] <= r[1]) {
            lg.range = std::array<double, 2>{ double(r[0]), double(r[1]) };
            for (float value : cr.get_values()) {
                PreviewLegendItem it;
                it.value = value;
                it.color = to_rgb(cr.get_color_at(value));
                lg.items.push_back(std::move(it));
            }
        }
        break;
    }
    }
    for (libvgcode::EOptionType t : viewer.get_options()) {
        if (t == libvgcode::EOptionType::Travels || !viewer.is_option_visible(t))
            continue;
        for (const OptionName& o : option_table())
            if (o.type == t) {
                PreviewLegendItem it;
                it.key = o.name;
                it.label = o.label;
                it.color = to_rgb(viewer.get_option_color(t));
                lg.options.push_back(std::move(it));
            }
    }
    if (viewer.is_option_visible(libvgcode::EOptionType::Travels)) {
        PreviewLegendItem it;
        it.key = "travels";
        it.label = "Travel";
        it.color = to_rgb(viewer.get_option_color(libvgcode::EOptionType::Travels));
        it.time_s = viewer.get_travels_estimated_time();
        lg.options.push_back(std::move(it));
    }

    // ---- info
    d.stats.renders++;
    d.stats.loaded = true;
    d.stats.plate = plate_idx;
    d.stats.result_id = result->id;
    d.stats.vertices = viewer.get_vertices_count();
    d.stats.cpu_bytes = viewer.get_used_cpu_memory();
    d.stats.gpu_bytes = viewer.get_used_gpu_memory();
    d.stats.last_render_ms = ms_since(t_start);

    json& info = out.info;
    info = json::object();
    info["plate"] = plate_idx;
    info["result_id"] = result->id;
    json layers = json::object();
    layers["count"] = layers_count;
    layers["from"] = lmin + 1;
    layers["to"] = lmax + 1;
    layers["z_from"] = zs.empty() ? 0.0 : double(zs[lmin]);
    layers["z_to"] = zs.empty() ? 0.0 : double(zs[lmax]);
    info["layers"] = std::move(layers);
    json moves = json::object();
    moves["vertices"] = viewer.get_vertices_count();
    moves["visible_from"] = visible[0];
    moves["visible_to"] = visible[1];
    moves["top_layer_first"] = top_first;
    moves["top_layer_count"] = top_count;
    moves["gcode_line_from"] = viewer.get_vertex_at(visible[0]).gcode_id;
    moves["gcode_line_to"] = viewer.get_vertex_at(visible[1]).gcode_id;
    info["moves"] = std::move(moves);
    info["estimated_time_s"] = viewer.get_estimated_time();
    info["toolpaths_box"] = box_json(d.toolpaths_box);
    info["visible_box"] = box_json(visible_box);
    info["plate_box"] = box_json(plate->get_bounding_box(false));
    info["fit_box"] = box_json(fit_box);
    if (result->belt_tilt_angle > 0.f)
        info["warning"] = "belt printer: the toolpaths are drawn in the machine frame, not the designed (upright) view";
    if (!gl_err_text.empty())
        info["gl_errors"] = gl_err_text;
    json perf = json::object();
    perf["cache_hit"] = cache_hit;
    perf["convert_ms"] = convert_ms;
    perf["upload_ms"] = upload_ms;
    perf["settings_ms"] = settings_ms;
    perf["render_ms"] = render_ms;
    perf["encode_ms"] = encode_ms;
    perf["total_ms"] = ms_since(t_start);
    perf["samples"] = samples;
    perf["max_size"] = max_size;
    perf["viewer_cpu_bytes"] = d.stats.cpu_bytes;
    perf["viewer_gpu_bytes"] = d.stats.gpu_bytes;
    info["perf"] = std::move(perf);

    if (d.idle_seconds > 0)
        d.idle_timer.StartOnce(d.idle_seconds * 1000);
    out.ok = true;
    return out;
}

// =============================================================================================
// JSON
// =============================================================================================

namespace {

bool get_vec3(const json& j, Vec3d& out)
{
    if (!j.is_array() || j.size() != 3)
        return false;
    for (int i = 0; i < 3; ++i) {
        if (!j[i].is_number())
            return false;
        out[i] = j[i].get<double>();
    }
    return true;
}

bool parse_color(const json& j, std::array<float, 4>& out)
{
    if (j.is_string()) {
        std::string s = j.get<std::string>();
        if (!s.empty() && s[0] == '#')
            s = s.substr(1);
        if (s.size() != 6 && s.size() != 8)
            return false;
        for (char c : s)
            if (!std::isxdigit(static_cast<unsigned char>(c)))
                return false;
        for (size_t i = 0; i < s.size() / 2; ++i)
            out[i] = float(std::stoi(s.substr(2 * i, 2), nullptr, 16)) / 255.0f;
        if (s.size() == 6)
            out[3] = 1.0f;
        return true;
    }
    if (j.is_array() && (j.size() == 3 || j.size() == 4)) {
        for (size_t i = 0; i < j.size(); ++i) {
            if (!j[i].is_number())
                return false;
            out[i] = std::clamp(j[i].get<float>(), 0.0f, 1.0f);
        }
        if (j.size() == 3)
            out[3] = 1.0f;
        return true;
    }
    return false;
}

} // namespace

bool preview_request_from_json(const json& j, PreviewRequest& out, std::string& error)
{
    if (!j.is_object()) {
        error = "the request must be a JSON object";
        return false;
    }
    static const std::vector<std::string> known = { "plate", "size", "camera", "view_type", "time_mode", "layers", "z_range", "moves",
        "top_layer_only", "dim_lower_layers", "dim_brightness", "options", "features", "show_supports", "show_wipe_tower", "show_bed",
        "show_shells", "translucent_filaments", "background", "samples" };
    for (auto it = j.begin(); it != j.end(); ++it)
        if (std::find(known.begin(), known.end(), it.key()) == known.end()) {
            error = "unknown field \"" + it.key() + "\"";
            return false;
        }
    auto bad = [&error](const std::string& field, const std::string& what) { error = field + ": " + what; return false; };
    auto get_bool = [&](const json& o, const char* key, bool& dst, const std::string& path) {
        if (!o.contains(key)) return true;
        if (!o[key].is_boolean()) return bad(path + key, "expected true or false");
        dst = o[key].get<bool>();
        return true;
    };
    auto get_num = [&](const json& o, const char* key, std::optional<double>& dst, const std::string& path) {
        if (!o.contains(key) || o[key].is_null()) return true;
        if (!o[key].is_number()) return bad(path + key, "expected a number");
        dst = o[key].get<double>();
        return true;
    };
    auto get_int = [&](const json& o, const char* key, std::optional<int>& dst, const std::string& path) {
        if (!o.contains(key) || o[key].is_null()) return true;
        if (!o[key].is_number_integer()) return bad(path + key, "expected an integer");
        dst = o[key].get<int>();
        return true;
    };

    if (j.contains("plate")) {
        if (!j["plate"].is_number_integer()) return bad("plate", "expected a plate index (0-based) or -1 for the current plate");
        out.plate = j["plate"].get<int>();
    }
    if (j.contains("size")) {
        const json& s = j["size"];
        if (!s.is_object() || !s.contains("width") || !s.contains("height") || !s["width"].is_number_integer() || !s["height"].is_number_integer())
            return bad("size", "expected {\"width\": int, \"height\": int}");
        const int w = s["width"].get<int>(), h = s["height"].get<int>();
        if (w < 16 || h < 16) return bad("size", "width and height must be at least 16");
        out.width = unsigned(w);
        out.height = unsigned(h);
    }
    if (j.contains("camera")) {
        const json& c = j["camera"];
        if (!c.is_object()) return bad("camera", "expected an object");
        PreviewCameraSpec& cs = out.camera;
        if (c.contains("view")) {
            if (!c["view"].is_string()) return bad("camera.view", "expected a string");
            cs.view = c["view"].get<std::string>();
        }
        if (c.contains("azimuth_deg") || c.contains("elevation_deg")) {
            if (!c.contains("view"))
                cs.view.clear(); // angles given, no named view: orbit
            std::optional<double> a, e;
            if (!get_num(c, "azimuth_deg", a, "camera.") || !get_num(c, "elevation_deg", e, "camera.")) return false;
            if (a) cs.azimuth_deg = *a;
            if (e) cs.elevation_deg = *e;
        }
        if (c.contains("target")) {
            Vec3d t;
            if (!get_vec3(c["target"], t)) return bad("camera.target", "expected [x, y, z]");
            cs.target = t;
        }
        if (!get_num(c, "distance", cs.distance, "camera.") || !get_num(c, "zoom", cs.zoom, "camera.")) return false;
        std::optional<double> margin;
        if (!get_num(c, "margin", margin, "camera.")) return false;
        if (margin) cs.margin = *margin;
        if (c.contains("projection")) {
            const std::string pr = c["projection"].is_string() ? c["projection"].get<std::string>() : "";
            if (pr == "auto") cs.projection = PreviewProjection::Auto;
            else if (pr == "perspective") cs.projection = PreviewProjection::Perspective;
            else if (pr == "orthographic" || pr == "ortho") cs.projection = PreviewProjection::Orthographic;
            else return bad("camera.projection", "expected \"auto\", \"perspective\" or \"orthographic\"");
        }
        if (c.contains("fit")) {
            const std::string f = c["fit"].is_string() ? c["fit"].get<std::string>() : "";
            if (f == "visible" || f == "visible_toolpaths") cs.fit = PreviewFit::VisibleToolpaths;
            else if (f == "toolpaths") cs.fit = PreviewFit::Toolpaths;
            else if (f == "plate") cs.fit = PreviewFit::Plate;
            else if (f == "box") cs.fit = PreviewFit::Box;
            else if (f == "none") cs.fit = PreviewFit::None;
            else return bad("camera.fit", "expected \"visible\", \"toolpaths\", \"plate\", \"box\" or \"none\"");
        }
        if (c.contains("box")) {
            const json& b = c["box"];
            Vec3d mn, mx;
            if (!b.is_object() || !b.contains("min") || !b.contains("max") || !get_vec3(b["min"], mn) || !get_vec3(b["max"], mx))
                return bad("camera.box", "expected {\"min\": [x,y,z], \"max\": [x,y,z]}");
            cs.box = BoundingBoxf3(mn, mx);
            if (!c.contains("fit"))
                cs.fit = PreviewFit::Box;
        }
    }
    if (j.contains("view_type")) {
        if (!j["view_type"].is_string()) return bad("view_type", "expected a string");
        out.view_type = j["view_type"].get<std::string>();
        if (find_view_type(out.view_type) == nullptr) {
            std::string all;
            for (const ViewTypeName& v : view_type_table()) all += (all.empty() ? "" : ", ") + std::string(v.name);
            return bad("view_type", "unknown \"" + out.view_type + "\"; one of " + all);
        }
    }
    if (j.contains("time_mode")) {
        if (!j["time_mode"].is_string()) return bad("time_mode", "expected \"normal\" or \"silent\"");
        out.time_mode = j["time_mode"].get<std::string>();
    }
    if (j.contains("layers")) {
        const json& l = j["layers"];
        if (!l.is_object()) return bad("layers", "expected {\"from\": int, \"to\": int} (1-based, inclusive)");
        if (!get_int(l, "from", out.layer_min, "layers.") || !get_int(l, "to", out.layer_max, "layers.")) return false;
    }
    if (j.contains("z_range")) {
        const json& z = j["z_range"];
        if (!z.is_object()) return bad("z_range", "expected {\"min\": mm, \"max\": mm}");
        if (!get_num(z, "min", out.z_min, "z_range.") || !get_num(z, "max", out.z_max, "z_range.")) return false;
    }
    if (j.contains("moves")) {
        const json& m = j["moves"];
        if (!m.is_object()) return bad("moves", "expected {\"from\": 0..1, \"to\": 0..1}");
        if (!get_num(m, "from", out.moves_from, "moves.") || !get_num(m, "to", out.moves_to, "moves.")) return false;
    }
    if (!get_bool(j, "top_layer_only", out.top_layer_only, "") || !get_bool(j, "dim_lower_layers", out.dim_lower_layers, "") ||
        !get_bool(j, "show_bed", out.show_bed, "") || !get_bool(j, "show_shells", out.show_shells, "") ||
        !get_bool(j, "translucent_filaments", out.translucent_filaments, ""))
        return false;
    {
        std::optional<double> b;
        if (!get_num(j, "dim_brightness", b, "")) return false;
        if (b) out.dim_brightness = float(std::clamp(*b, 0.0, 1.0));
    }
    for (const char* key : { "show_supports", "show_wipe_tower" }) {
        if (!j.contains(key)) continue;
        if (!j[key].is_boolean()) return bad(key, "expected true or false");
        (std::string(key) == "show_supports" ? out.show_supports : out.show_wipe_tower) = j[key].get<bool>();
    }
    for (const char* key : { "options", "features" }) {
        if (!j.contains(key)) continue;
        const json& o = j[key];
        if (!o.is_object()) return bad(key, "expected an object of name: true/false");
        for (auto it = o.begin(); it != o.end(); ++it) {
            if (!it.value().is_boolean()) return bad(std::string(key) + "." + it.key(), "expected true or false");
            if (std::string(key) == "options") {
                const auto& names = PreviewRenderer::option_names();
                if (std::find(names.begin(), names.end(), it.key()) == names.end())
                    return bad("options." + it.key(), "unknown option");
                out.options[it.key()] = it.value().get<bool>();
            } else {
                libvgcode::EGCodeExtrusionRole role;
                if (!find_feature(it.key(), role))
                    return bad("features." + it.key(), "unknown feature");
                out.features[it.key()] = it.value().get<bool>();
            }
        }
    }
    if (j.contains("background") && !parse_color(j["background"], out.background))
        return bad("background", "expected \"#rrggbb\", \"#rrggbbaa\" or [r, g, b(, a)] in 0..1");
    if (j.contains("samples")) {
        if (!j["samples"].is_number_integer()) return bad("samples", "expected an integer");
        out.samples = j["samples"].get<int>();
    }
    return true;
}

json to_json(const PreviewCameraInfo& c)
{
    json j = json::object();
    j["projection"] = c.projection;
    j["position"] = vec_json(c.position);
    j["target"] = vec_json(c.target);
    j["up"] = vec_json(c.up);
    j["forward"] = vec_json(c.forward);
    j["azimuth_deg"] = c.azimuth_deg;
    j["elevation_deg"] = c.elevation_deg;
    j["zoom_px_per_mm"] = c.zoom;
    if (c.projection == "perspective")
        j["fov_deg"] = c.fov_deg;
    j["near"] = c.near_z;
    j["far"] = c.far_z;
    j["viewport"] = json::array({ c.viewport[0], c.viewport[1], c.viewport[2], c.viewport[3] });
    j["view_matrix"] = mat_json(c.view);
    j["projection_matrix"] = mat_json(c.projection_matrix);
    j["view_projection_matrix"] = mat_json(c.view_projection);
    j["matrix_layout"] = "column-major 4x4, world -> clip; pixel (u, v) from the top-left maps to ndc ((u + 0.5) / width * 2 - 1, 1 - (v + 0.5) / height * 2)";
    return j;
}

json to_json(const PreviewLegend& l)
{
    auto item_json = [](const PreviewLegendItem& it) {
        json j = json::object();
        if (!it.key.empty()) j["key"] = it.key;
        if (!it.label.empty()) j["label"] = it.label;
        char hex[8];
        snprintf(hex, sizeof(hex), "#%02X%02X%02X", it.color[0], it.color[1], it.color[2]);
        j["color"] = hex;
        if (it.value) j["value"] = *it.value;
        if (it.time_s) j["time_s"] = *it.time_s;
        if (it.percent) j["percent"] = *it.percent;
        if (!it.visible) j["visible"] = false;
        return j;
    };
    json j = json::object();
    j["view_type"] = l.view_type;
    j["title"] = l.title;
    j["kind"] = l.kind;
    if (!l.unit.empty()) j["unit"] = l.unit;
    if (l.range) j["range"] = json::array({ (*l.range)[0], (*l.range)[1] });
    if (l.logarithmic) j["logarithmic"] = true;
    json items = json::array();
    for (const PreviewLegendItem& it : l.items) items.push_back(item_json(it));
    j["items"] = std::move(items);
    json options = json::array();
    for (const PreviewLegendItem& it : l.options) options.push_back(item_json(it));
    j["options"] = std::move(options);
    return j;
}

json to_json(const PreviewImage& img)
{
    json j = json::object();
    j["ok"] = img.ok;
    if (!img.ok) {
        json e = json::object();
        e["code"] = img.error_code;
        e["message"] = img.error_message;
        j["error"] = std::move(e);
        return j;
    }
    j["size"] = json::object({ { "width", img.width }, { "height", img.height } });
    j["camera"] = to_json(img.camera);
    j["legend"] = to_json(img.legend);
    j["info"] = img.info;
    return j;
}

}}} // namespace Slic3r::GUI::Mcp
