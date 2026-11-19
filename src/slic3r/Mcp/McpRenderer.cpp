#include "McpRenderer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <memory>

#include <boost/log/trivial.hpp>

#include <glad/gl.h>

#include "libslic3r/Color.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"
#include "libslic3r/GCode/Thumbnails.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/3DScene.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GLModel.hpp"
#include "slic3r/GUI/GLShader.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/OpenGLManager.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"

#include "McpError.hpp"
#include "McpIds.hpp"
#include "McpScene.hpp"

namespace Slic3r { namespace GUI { namespace Mcp {

namespace {

const size_t MAX_PNG_BYTES = 900 * 1024; // above this a PNG is re-encoded as JPEG

// Flat colours of the plain bed. Chosen to stay apart from typical filament colours and to read
// well for a model looking at the picture.
const ColorRGBA BACKGROUND { 0.93f, 0.94f, 0.95f, 1.0f };
const ColorRGBA BED_FILL { 0.78f, 0.80f, 0.82f, 1.0f };
const ColorRGBA BED_GRID_FINE { 0.66f, 0.68f, 0.70f, 1.0f };
const ColorRGBA BED_GRID_BOLD { 0.45f, 0.47f, 0.50f, 1.0f };
const ColorRGBA BED_BORDER { 0.20f, 0.22f, 0.25f, 1.0f };
const ColorRGBA AXIS_X { 0.85f, 0.10f, 0.10f, 1.0f };
const ColorRGBA AXIS_Y { 0.10f, 0.60f, 0.10f, 1.0f };
const ColorRGBA AXIS_Z { 0.10f, 0.25f, 0.90f, 1.0f };
const ColorRGBA HIGHLIGHT { 1.00f, 0.42f, 0.00f, 1.0f };

const float TRANSLUCENT_ALPHA = 0.35f;

[[noreturn]] void gl_unavailable(const std::string& why)
{
    fail("gl_unavailable", "Cannot render a picture: " + why + ".", "Bring the OrcaSlicer window on screen once (the 3D view must have been shown), then retry.");
}

std::string hex(const ColorRGBA& color)
{
    return encode_color(ColorRGB(color.r(), color.g(), color.b()));
}

ColorRGBA type_color(ModelVolumeType type)
{
    switch (type) {
    case ModelVolumeType::MODEL_PART: return { 0.70f, 0.72f, 0.74f, 1.0f };
    case ModelVolumeType::NEGATIVE_VOLUME: return { 0.25f, 0.25f, 0.28f, 1.0f };
    case ModelVolumeType::PARAMETER_MODIFIER: return { 0.95f, 0.78f, 0.10f, 1.0f };
    case ModelVolumeType::SUPPORT_BLOCKER: return { 0.90f, 0.15f, 0.15f, 1.0f };
    case ModelVolumeType::SUPPORT_ENFORCER: return { 0.15f, 0.35f, 0.95f, 1.0f };
    default: return { 0.65f, 0.20f, 0.80f, 1.0f }; // precise-seam bodies
    }
}

ColorRGBA palette_color(size_t index)
{
    static const std::array<ColorRGBA, 10> palette = {{
        { 0.12f, 0.47f, 0.71f, 1.f }, { 1.00f, 0.50f, 0.05f, 1.f }, { 0.17f, 0.63f, 0.17f, 1.f }, { 0.84f, 0.15f, 0.16f, 1.f }, { 0.58f, 0.40f, 0.74f, 1.f },
        { 0.55f, 0.34f, 0.29f, 1.f }, { 0.89f, 0.47f, 0.76f, 1.f }, { 0.50f, 0.50f, 0.50f, 1.f }, { 0.74f, 0.74f, 0.13f, 1.f }, { 0.09f, 0.75f, 0.81f, 1.f },
    }};
    return palette[index % palette.size()];
}

// --- framebuffer ---------------------------------------------------------------------------------

class Framebuffer
{
public:
    Framebuffer(int width, int height) : m_width(width), m_height(height)
    {
        GLint max_samples = 0;
        glsafe(::glGetIntegerv(GL_MAX_SAMPLES, &max_samples));
        m_samples = OpenGLManager::can_multisample() ? std::min<GLint>(4, max_samples) : 0;

        glsafe(::glGenFramebuffers(1, &m_fbo));
        glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, m_fbo));
        glsafe(::glGenRenderbuffers(1, &m_color));
        glsafe(::glBindRenderbuffer(GL_RENDERBUFFER, m_color));
        if (m_samples > 1)
            glsafe(::glRenderbufferStorageMultisample(GL_RENDERBUFFER, m_samples, GL_RGBA8, width, height));
        else
            glsafe(::glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height));
        glsafe(::glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, m_color));
        glsafe(::glGenRenderbuffers(1, &m_depth));
        glsafe(::glBindRenderbuffer(GL_RENDERBUFFER, m_depth));
        if (m_samples > 1)
            glsafe(::glRenderbufferStorageMultisample(GL_RENDERBUFFER, m_samples, GL_DEPTH_COMPONENT24, width, height));
        else
            glsafe(::glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height));
        glsafe(::glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_depth));
        const GLenum buffers[] = { GL_COLOR_ATTACHMENT0 };
        glsafe(::glDrawBuffers(1, buffers));
        m_complete = ::glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    }

    ~Framebuffer()
    {
        glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, 0));
        if (m_depth != 0) glsafe(::glDeleteRenderbuffers(1, &m_depth));
        if (m_color != 0) glsafe(::glDeleteRenderbuffers(1, &m_color));
        if (m_fbo != 0) glsafe(::glDeleteFramebuffers(1, &m_fbo));
    }

    Framebuffer(const Framebuffer&) = delete;
    Framebuffer& operator=(const Framebuffer&) = delete;

    bool complete() const { return m_complete; }

    // RGBA rows, bottom row first (as OpenGL returns them).
    bool read(std::vector<unsigned char>& pixels)
    {
        pixels.assign(static_cast<size_t>(m_width) * static_cast<size_t>(m_height) * 4, 0);
        glsafe(::glPixelStorei(GL_PACK_ALIGNMENT, 1));
        if (m_samples <= 1) {
            glsafe(::glBindFramebuffer(GL_READ_FRAMEBUFFER, m_fbo));
            glsafe(::glReadPixels(0, 0, m_width, m_height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data()));
            return true;
        }
        // Multisampled buffers cannot be read directly: resolve into a plain one first.
        GLuint resolve_fbo = 0, resolve_color = 0;
        glsafe(::glGenFramebuffers(1, &resolve_fbo));
        glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, resolve_fbo));
        glsafe(::glGenRenderbuffers(1, &resolve_color));
        glsafe(::glBindRenderbuffer(GL_RENDERBUFFER, resolve_color));
        glsafe(::glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, m_width, m_height));
        glsafe(::glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, resolve_color));
        const bool ok = ::glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        if (ok) {
            glsafe(::glBindFramebuffer(GL_READ_FRAMEBUFFER, m_fbo));
            glsafe(::glBindFramebuffer(GL_DRAW_FRAMEBUFFER, resolve_fbo));
            glsafe(::glBlitFramebuffer(0, 0, m_width, m_height, 0, 0, m_width, m_height, GL_COLOR_BUFFER_BIT, GL_NEAREST));
            glsafe(::glBindFramebuffer(GL_READ_FRAMEBUFFER, resolve_fbo));
            glsafe(::glReadPixels(0, 0, m_width, m_height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data()));
        }
        glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, m_fbo));
        glsafe(::glDeleteRenderbuffers(1, &resolve_color));
        glsafe(::glDeleteFramebuffers(1, &resolve_fbo));
        return ok;
    }

private:
    int    m_width;
    int    m_height;
    GLint  m_samples { 0 };
    GLuint m_fbo { 0 };
    GLuint m_color { 0 };
    GLuint m_depth { 0 };
    bool   m_complete { false };
};

// Makes the editor canvas' GL context current for the lifetime of the object and puts back the
// state the canvas expects afterwards.
class GlScope
{
public:
    GlScope()
    {
        Plater* plater = wxGetApp().plater();
        m_canvas = plater != nullptr ? plater->get_view3D_canvas3D() : nullptr;
        if (m_canvas == nullptr)
            gl_unavailable("the 3D view does not exist yet");
        if (!m_canvas->is_initialized())
            gl_unavailable("the 3D view has not been initialised yet");
        if (!m_canvas->make_current_for_postinit())
            gl_unavailable("the OpenGL context cannot be made current");
        if (OpenGLManager::get_framebuffers_type() != OpenGLManager::EFramebufferType::Arb)
            gl_unavailable("this OpenGL driver has no framebuffer objects");
    }

    ~GlScope()
    {
        glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, 0));
        glsafe(::glDisable(GL_BLEND));
        glsafe(::glDepthMask(GL_TRUE));
        glsafe(::glEnable(GL_CULL_FACE));
        glsafe(::glDisable(GL_DEPTH_TEST));
        // The canvas viewport, as the thumbnail generator restores it.
        if (Plater* plater = wxGetApp().plater(); plater != nullptr)
            plater->get_camera().apply_viewport();
    }

    GlScope(const GlScope&) = delete;
    GlScope& operator=(const GlScope&) = delete;

    GLCanvas3D& canvas() { return *m_canvas; }

private:
    GLCanvas3D* m_canvas { nullptr };
};

// --- simple geometry -------------------------------------------------------------------------------

void draw_lines(GLShaderProgram& shader, const std::vector<std::pair<Vec3f, Vec3f>>& segments, const ColorRGBA& color)
{
    if (segments.empty())
        return;
    GLModel::Geometry data;
    data.format = { GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3 };
    data.reserve_vertices(segments.size() * 2);
    data.reserve_indices(segments.size() * 2);
    unsigned int index = 0;
    for (const auto& [a, b] : segments) {
        data.add_vertex(a);
        data.add_vertex(b);
        data.add_line(index, index + 1);
        index += 2;
    }
    GLModel model;
    model.init_from(std::move(data));
    model.set_color(color);
    model.render(&shader);
}

void draw_quad(GLShaderProgram& shader, const Vec3f& a, const Vec3f& b, const Vec3f& c, const Vec3f& d, const ColorRGBA& color)
{
    GLModel::Geometry data;
    data.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3 };
    data.add_vertex(a);
    data.add_vertex(b);
    data.add_vertex(c);
    data.add_vertex(d);
    data.add_triangle(0, 1, 2);
    data.add_triangle(0, 2, 3);
    GLModel model;
    model.init_from(std::move(data));
    model.set_color(color);
    model.render(&shader);
}

// The plate as a plain rectangle with a 10 mm grid (every fifth line bold), a border and the axes
// at its origin corner. Drawn slightly below z = 0 so that first layers sit on top of it.
void draw_bed(const CameraView& camera, const BoundingBoxf3& plate, const Vec3d& origin)
{
    GLShaderProgram* shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;
    shader->start_using();
    shader->set_uniform("view_model_matrix", camera.view);
    shader->set_uniform("projection_matrix", camera.projection);

    const float x0 = static_cast<float>(plate.min.x()), x1 = static_cast<float>(plate.max.x());
    const float y0 = static_cast<float>(plate.min.y()), y1 = static_cast<float>(plate.max.y());
    const float z_fill = -0.06f, z_line = -0.03f;

    glsafe(::glDisable(GL_CULL_FACE));
    // Seen from below, a solid plate would hide everything: draw only its lines then.
    if (camera.eye.z() > 0.)
        draw_quad(*shader, { x0, y0, z_fill }, { x1, y0, z_fill }, { x1, y1, z_fill }, { x0, y1, z_fill }, BED_FILL);

    std::vector<std::pair<Vec3f, Vec3f>> fine, bold;
    const float ox = static_cast<float>(origin.x()), oy = static_cast<float>(origin.y());
    for (int i = static_cast<int>(std::ceil((x0 - ox) / 10.f)); ox + 10.f * static_cast<float>(i) <= x1; ++i) {
        const float x = ox + 10.f * static_cast<float>(i);
        (i % 5 == 0 ? bold : fine).emplace_back(Vec3f(x, y0, z_line), Vec3f(x, y1, z_line));
    }
    for (int i = static_cast<int>(std::ceil((y0 - oy) / 10.f)); oy + 10.f * static_cast<float>(i) <= y1; ++i) {
        const float y = oy + 10.f * static_cast<float>(i);
        (i % 5 == 0 ? bold : fine).emplace_back(Vec3f(x0, y, z_line), Vec3f(x1, y, z_line));
    }
    draw_lines(*shader, fine, BED_GRID_FINE);
    draw_lines(*shader, bold, BED_GRID_BOLD);
    draw_lines(*shader, { { { x0, y0, z_line }, { x1, y0, z_line } }, { { x1, y0, z_line }, { x1, y1, z_line } },
                          { { x1, y1, z_line }, { x0, y1, z_line } }, { { x0, y1, z_line }, { x0, y0, z_line } } }, BED_BORDER);

    // Axes from the plate origin: X red, Y green, Z blue, a tenth of the plate long.
    const float len = 0.1f * std::max(x1 - x0, y1 - y0);
    const float oz = 0.02f;
    draw_lines(*shader, { { { ox, oy, oz }, { ox + len, oy, oz } } }, AXIS_X);
    draw_lines(*shader, { { { ox, oy, oz }, { ox, oy + len, oz } } }, AXIS_Y);
    draw_lines(*shader, { { { ox, oy, oz }, { ox, oy, oz + len } } }, AXIS_Z);
    glsafe(::glEnable(GL_CULL_FACE));
    shader->stop_using();
}

// --- camera ----------------------------------------------------------------------------------------

struct PresetAngles
{
    const char* name;
    double      azimuth;
    double      elevation;
};

const PresetAngles PRESETS[] = {
    { "iso", 45., 35. },   { "iso_left", 315., 35. }, { "iso_back", 135., 35. }, { "iso_back_left", 225., 35. }, { "top", 0., 90. },
    { "bottom", 0., -90. }, { "front", 0., 0. },       { "back", 180., 0. },      { "left", 270., 0. },           { "right", 90., 0. },
};

void merge_box(BoundingBoxf3& into, const BoundingBoxf3& box)
{
    if (box.defined)
        into.merge(box);
}

CameraView make_camera(const CameraRequest& request, int width, int height, const Vec3d& plate_origin, const BoundingBoxf3& plate_box,
                       const BoundingBoxf3& content_box, const BoundingBoxf3& ids_box)
{
    CameraView view;
    view.width = width;
    view.height = height;
    view.perspective = !request.orthographic;
    for (const PresetAngles& preset : PRESETS)
        if (request.preset == preset.name) {
            view.azimuth_deg = preset.azimuth;
            view.elevation_deg = preset.elevation;
        }
    if (request.has_azimuth)
        view.azimuth_deg = request.azimuth_deg;
    if (request.has_elevation)
        view.elevation_deg = request.elevation_deg;

    // The plate with what stands on it: what "zoom 1" frames by default.
    BoundingBoxf3 plate_and_content = plate_box;
    merge_box(plate_and_content, content_box);

    BoundingBoxf3 frame = plate_and_content;
    if (request.target == "objects" && content_box.defined)
        frame = content_box;
    else if (request.target == "ids" && ids_box.defined)
        frame = ids_box;

    view.target = frame.center();
    view.fit(box_corners(frame.min, frame.max), request.zoom);
    if (request.has_target_point)
        view.target = plate_origin + request.target_point; // same scale, centred on the point
    if (request.has_distance && view.perspective)
        view.distance = request.distance;

    // Everything that may be drawn must lie between the clipping planes.
    BoundingBoxf3 depth_box = plate_and_content;
    merge_box(depth_box, ids_box);
    depth_box.min.z() = std::min(depth_box.min.z(), -1.);
    view.set_depth_range(box_corners(depth_box.min, depth_box.max));
    return view;
}

// --- encoding --------------------------------------------------------------------------------------

Picture encode(const std::vector<unsigned char>& pixels, int width, int height, const std::string& format)
{
    ThumbnailData data;
    data.set(static_cast<unsigned int>(width), static_cast<unsigned int>(height));
    if (!data.is_valid() || data.pixels.size() != pixels.size())
        fail("internal", "Cannot allocate the picture.");
    data.pixels = pixels;

    Picture picture;
    auto    compress = [&data](GCodeThumbnailsFormat fmt) { return GCodeThumbnails::compress_thumbnail(data, fmt); };
    std::unique_ptr<GCodeThumbnails::CompressedImageBuffer> buffer;
    if (format != "jpeg") {
        buffer = compress(GCodeThumbnailsFormat::PNG);
        picture.mime_type = "image/png";
    }
    if (!buffer || buffer->data == nullptr || buffer->size == 0 || buffer->size > MAX_PNG_BYTES) {
        buffer = compress(GCodeThumbnailsFormat::JPG);
        picture.mime_type = "image/jpeg";
    }
    if (!buffer || buffer->data == nullptr || buffer->size == 0)
        fail("internal", "Cannot encode the picture.");
    picture.bytes.assign(static_cast<const char*>(buffer->data), buffer->size);
    return picture;
}

BoundingBoxf3 plate_box_of(int plate_idx)
{
    PartPlateList& plates = wxGetApp().plater()->get_partplate_list();
    BoundingBoxf3  box = plates.get_plate(plate_idx)->get_build_volume();
    // The bed itself: the build volume's footprint at z = 0.
    box.max.z() = 0.;
    box.min.z() = 0.;
    return box;
}

// One volume to draw.
struct DrawItem
{
    GLVolume*   volume;
    ColorRGBA   color;
    bool        translucent;
    double      depth; // along the view direction, for back-to-front order
};

std::set<std::pair<int, int>> volume_set(const json& ids, const char* what, std::set<std::pair<int, int>>* instances = nullptr)
{
    // (object index, volume index); a volume index of -1 stands for every volume of the object.
    std::set<std::pair<int, int>> out;
    if (ids.is_null())
        return out;
    for (const ItemRef& ref : Ids::items(ids, what)) {
        if (ref.kind == IdKind::Volume)
            out.emplace(ref.obj_idx, ref.vol_idx);
        else if (ref.kind == IdKind::Instance && instances != nullptr)
            instances->emplace(ref.obj_idx, ref.inst_idx);
        else
            out.emplace(ref.obj_idx, -1);
    }
    return out;
}

bool in_set(const std::set<std::pair<int, int>>& set, int obj_idx, int vol_idx)
{
    return set.count({ obj_idx, vol_idx }) != 0 || set.count({ obj_idx, -1 }) != 0;
}

void add_objects_in_view(json& info, const CameraView& camera, const std::map<std::pair<int, int>, BoundingBoxf3>& instance_boxes)
{
    const Model& model = wxGetApp().plater()->model();
    json         list = json::array();
    for (const auto& [key, box] : instance_boxes) {
        const ModelObject& object = *model.objects[static_cast<size_t>(key.first)];
        double min_u = 1e30, min_v = 1e30, max_u = -1e30, max_v = -1e30;
        bool   any = false;
        for (const Vec3d& corner : box_corners(box.min, box.max)) {
            Vec2d pixel;
            if (!camera.project(corner, pixel))
                continue;
            any = true;
            min_u = std::min(min_u, pixel.x());
            max_u = std::max(max_u, pixel.x());
            min_v = std::min(min_v, pixel.y());
            max_v = std::max(max_v, pixel.y());
        }
        Vec2d centre;
        if (!any || !camera.project(box.center(), centre))
            continue;
        json item = json::object();
        item["id"] = Ids::of(object);
        item["instance"] = Ids::of(*object.instances[static_cast<size_t>(key.second)]);
        item["name"] = object.name;
        item["pixel"] = json::array({ std::lround(centre.x()), std::lround(centre.y()) });
        item["pixel_box"] = json::array({ std::lround(min_u), std::lround(min_v), std::lround(max_u), std::lround(max_v) });
        item["in_picture"] = centre.x() >= 0 && centre.x() < camera.width && centre.y() >= 0 && centre.y() < camera.height;
        list.push_back(std::move(item));
    }
    info["objects_in_view"] = std::move(list);
}

} // namespace

namespace {

std::vector<ViewPlugin>& view_plugins()
{
    static std::vector<ViewPlugin> plugins;
    return plugins;
}

std::vector<OverlayPlugin>& overlay_plugins()
{
    static std::vector<OverlayPlugin> plugins;
    return plugins;
}

} // namespace

void RenderPlugins::add_view(ViewPlugin plugin)
{
    for (ViewPlugin& existing : view_plugins())
        if (existing.name == plugin.name) {
            existing = std::move(plugin);
            return;
        }
    view_plugins().push_back(std::move(plugin));
}

void RenderPlugins::add_overlay(OverlayPlugin plugin)
{
    for (OverlayPlugin& existing : overlay_plugins())
        if (existing.name == plugin.name) {
            existing = std::move(plugin);
            return;
        }
    overlay_plugins().push_back(std::move(plugin));
}

const std::vector<ViewPlugin>&    RenderPlugins::views() { return view_plugins(); }
const std::vector<OverlayPlugin>& RenderPlugins::overlays() { return overlay_plugins(); }

const ViewPlugin* RenderPlugins::find_view(const std::string& name)
{
    for (const ViewPlugin& plugin : view_plugins())
        if (plugin.name == name)
            return &plugin;
    return nullptr;
}

const OverlayPlugin* RenderPlugins::find_overlay(const std::string& name)
{
    for (const OverlayPlugin& plugin : overlay_plugins())
        if (plugin.name == name)
            return &plugin;
    return nullptr;
}

CameraRequest CameraRequest::from_json(const json& value)
{
    CameraRequest out;
    if (value.is_null())
        return out;
    out.preset = value.value("preset", std::string("iso"));
    if (auto it = value.find("azimuth_deg"); it != value.end() && it->is_number()) {
        out.has_azimuth = true;
        out.azimuth_deg = it->get<double>();
    }
    if (auto it = value.find("elevation_deg"); it != value.end() && it->is_number()) {
        out.has_elevation = true;
        out.elevation_deg = it->get<double>();
    }
    out.orthographic = value.value("projection", std::string("perspective")) == "orthographic";
    out.zoom = value.value("zoom", 1.);
    if (auto it = value.find("distance"); it != value.end() && it->is_number()) {
        out.has_distance = true;
        out.distance = it->get<double>();
    }
    if (auto it = value.find("target"); it != value.end() && !it->is_null()) {
        if (it->is_string()) {
            out.target = it->get<std::string>();
            if (out.target == "toolpaths")
                out.target = "objects"; // the content of the pass, whatever it draws
        } else if (it->is_array() && it->size() == 3 && (*it)[0].is_number()) {
            out.has_target_point = true;
            out.target_point = Vec3d((*it)[0].get<double>(), (*it)[1].get<double>(), (*it)[2].get<double>());
        } else if (it->is_array()) {
            out.target = "ids";
            out.target_ids = *it;
        }
    }
    if (out.has_distance && out.orthographic)
        fail("invalid_argument", "`camera.distance` has no effect on an orthographic view.", "Use `camera.zoom` to enlarge an orthographic view.");
    return out;
}

CameraView Renderer::camera_for(const CameraRequest& request, int width, int height, const Vec3d& plate_origin, const BoundingBoxf3& plate_box,
                                const BoundingBoxf3& content_box, const BoundingBoxf3& ids_box)
{
    return make_camera(request, width, height, plate_origin, plate_box, content_box, ids_box);
}

Picture Renderer::render_custom(int plate_idx, int width, int height, const CameraRequest& camera_request, const BoundingBoxf3& scene_box, bool show_bed,
                                const std::string& format, const std::function<void(const RenderTarget&)>& pass)
{
    GlScope        gl;
    PartPlateList& plates = wxGetApp().plater()->get_partplate_list();
    const Vec3d    origin = plates.get_plate(plate_idx)->get_origin();
    const BoundingBoxf3 plate_box = plate_box_of(plate_idx);

    const CameraView camera = make_camera(camera_request, width, height, origin, plate_box, scene_box, BoundingBoxf3());

    Framebuffer framebuffer(width, height);
    if (!framebuffer.complete())
        gl_unavailable("the offscreen framebuffer could not be created");

    glsafe(::glViewport(0, 0, width, height));
    glsafe(::glClearColor(BACKGROUND.r(), BACKGROUND.g(), BACKGROUND.b(), 1.f));
    glsafe(::glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT));
    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glDepthFunc(GL_LESS));
    glsafe(::glDepthMask(GL_TRUE));
    glsafe(::glDisable(GL_BLEND));

    if (show_bed)
        draw_bed(camera, plate_box, origin);

    json         legend = json::object();
    RenderTarget target;
    target.camera = &camera;
    target.plate_idx = plate_idx;
    target.plate_origin = origin;
    target.legend = &legend;
    if (pass)
        pass(target);

    std::vector<unsigned char> pixels;
    if (!framebuffer.read(pixels))
        gl_unavailable("the picture could not be read back");

    Picture picture = encode(pixels, width, height, format);
    picture.info = json::object();
    picture.info["size"] = json::array({ width, height });
    picture.info["format"] = picture.mime_type == "image/png" ? "png" : "jpeg";
    picture.info["plate"] = plate_idx + 1;
    picture.info["camera"] = camera.to_json();
    if (!legend.empty())
        picture.info["legend"] = std::move(legend);
    return picture;
}

Picture Renderer::render_editor(const EditorRequest& request)
{
    Plater*      plater = wxGetApp().plater();
    const Model& model = plater->model();

    // Resolve ids before any GL work.
    std::set<std::pair<int, int>> only_instances;
    const std::set<std::pair<int, int>> only = volume_set(request.only, "editor.only", &only_instances);
    const std::set<std::pair<int, int>> highlight = volume_set(request.highlight, "editor.highlight");
    const std::set<std::pair<int, int>> transparent = volume_set(request.transparent, "editor.transparent");
    const bool                          has_only = !request.only.is_null();

    BoundingBoxf3 ids_box;
    if (request.camera.target == "ids") {
        for (const ItemRef& ref : Ids::items(request.camera.target_ids, "camera.target")) {
            if (ref.kind == IdKind::Instance)
                ids_box.merge(ref.object->instance_bounding_box(static_cast<size_t>(ref.inst_idx)));
            else if (ref.kind == IdKind::Volume) {
                for (const ModelInstance* instance : ref.object->instances)
                    ids_box.merge(ref.volume->mesh().transformed_bounding_box(instance->get_matrix() * ref.volume->get_matrix()));
            } else {
                for (size_t i = 0; i < ref.object->instances.size(); ++i)
                    ids_box.merge(ref.object->instance_bounding_box(i));
            }
        }
    }

    GlScope          gl;
    GLShaderProgram* shader = wxGetApp().get_shader("thumbnail");
    if (shader == nullptr)
        gl_unavailable("the shaders are not loaded yet");

    // What to draw: the volumes of the 3D editor scene that belong to the plate.
    std::vector<DrawItem>                           items;
    BoundingBoxf3                                   content_box;
    std::map<std::pair<int, int>, BoundingBoxf3>    instance_boxes;
    std::map<int, size_t>                           object_order;
    std::vector<ColorRGBA>                          filament_colors = plater->get_extruders_colors();
    json                                            legend_entries = json::object();
    const BoundingBoxf3                             plate_box = plate_box_of(request.plate_idx);
    for (GLVolume* volume : gl.canvas().get_volumes().volumes) {
        if (volume == nullptr || !volume->model.is_initialized())
            continue;
        const int obj_idx = volume->object_idx();
        const int vol_idx = volume->volume_idx();
        const int inst_idx = volume->instance_idx();
        ColorRGBA color;
        bool      translucent = false;
        if (volume->is_wipe_tower) {
            // The wipe tower of the plate, if it stands on it.
            const Vec3d centre = volume->transformed_bounding_box().center();
            if (has_only || centre.x() < plate_box.min.x() || centre.x() > plate_box.max.x() || centre.y() < plate_box.min.y() || centre.y() > plate_box.max.y())
                continue;
            color = { 0.85f, 0.85f, 0.60f, 1.0f };
            translucent = true;
            legend_entries["wipe_tower"] = hex(color);
        } else {
            if (obj_idx < 0 || obj_idx >= static_cast<int>(model.objects.size()) || vol_idx < 0)
                continue;
            const ModelObject& object = *model.objects[static_cast<size_t>(obj_idx)];
            if (vol_idx >= static_cast<int>(object.volumes.size()) || inst_idx < 0 || inst_idx >= static_cast<int>(object.instances.size()))
                continue;
            if (Ids::plate_of_instance(obj_idx, inst_idx) != request.plate_idx)
                continue;
            if (has_only && !in_set(only, obj_idx, vol_idx) && only_instances.count({ obj_idx, inst_idx }) == 0)
                continue;
            const ModelVolume& model_volume = *object.volumes[static_cast<size_t>(vol_idx)];
            const bool         is_part = model_volume.is_model_part();
            if (!is_part && !request.show_modifiers)
                continue;

            if (request.color_by == "type") {
                color = type_color(model_volume.type());
                legend_entries[Scene::volume_type_name(model_volume)] = hex(color);
            } else if (request.color_by == "object") {
                const size_t order = object_order.emplace(obj_idx, object_order.size()).first->second;
                color = is_part ? palette_color(order) : type_color(model_volume.type());
                legend_entries[Ids::of(object)] = hex(palette_color(order));
            } else {
                // The filament colour the editor shows; bodies that are not parts get their type colour.
                if (is_part) {
                    color = adjust_color_for_rendering(volume->color);
                    const int slot = model_volume.config.has("extruder") && model_volume.config.extruder() > 0 ? model_volume.config.extruder()
                                   : (object.config.has("extruder") ? object.config.extruder() : 1);
                    if (slot >= 1 && slot <= static_cast<int>(filament_colors.size()))
                        color = adjust_color_for_rendering(filament_colors[static_cast<size_t>(slot - 1)]);
                    legend_entries["filament " + std::to_string(slot)] = hex(color);
                } else {
                    color = type_color(model_volume.type());
                    legend_entries[Scene::volume_type_name(model_volume)] = hex(color);
                }
            }
            color.a(1.0f);
            translucent = !is_part;
            if (in_set(highlight, obj_idx, vol_idx)) {
                color = HIGHLIGHT;
                legend_entries["highlight"] = hex(HIGHLIGHT);
            }
            if (in_set(transparent, obj_idx, vol_idx))
                translucent = true;

            const BoundingBoxf3& box = volume->transformed_bounding_box();
            if (is_part) {
                content_box.merge(box);
                instance_boxes[{ obj_idx, inst_idx }].merge(box);
            }
        }
        if (translucent)
            color.a(TRANSLUCENT_ALPHA);
        items.push_back(DrawItem { volume, color, translucent, 0. });
    }

    const Vec3d      origin = plater->get_partplate_list().get_plate(request.plate_idx)->get_origin();
    const CameraView camera = make_camera(request.camera, request.width, request.height, origin, plate_box, content_box, ids_box);

    Framebuffer framebuffer(request.width, request.height);
    if (!framebuffer.complete())
        gl_unavailable("the offscreen framebuffer could not be created");

    glsafe(::glViewport(0, 0, request.width, request.height));
    glsafe(::glClearColor(BACKGROUND.r(), BACKGROUND.g(), BACKGROUND.b(), 1.f));
    glsafe(::glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT));
    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glDepthFunc(GL_LESS));
    glsafe(::glDepthMask(GL_TRUE));
    glsafe(::glDisable(GL_BLEND));

    if (request.show_bed)
        draw_bed(camera, plate_box, origin);

    // Opaque volumes first, then the translucent ones back to front without writing depth.
    const Vec3d forward = (camera.target - camera.eye).normalized();
    for (DrawItem& item : items)
        item.depth = forward.dot(item.volume->transformed_bounding_box().center() - camera.eye);
    std::stable_sort(items.begin(), items.end(), [](const DrawItem& a, const DrawItem& b) {
        if (a.translucent != b.translucent)
            return !a.translucent;
        return a.translucent ? a.depth > b.depth : a.depth < b.depth;
    });

    ModelObjectPtrs& objects = plater->model().objects;
    shader->start_using();
    shader->set_uniform("emission_factor", 0.1f);
    shader->set_uniform("ban_light", false);
    glsafe(::glEnable(GL_CULL_FACE));
    bool blending = false;
    for (const DrawItem& item : items) {
        if (item.translucent && !blending) {
            glsafe(::glEnable(GL_BLEND));
            glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
            glsafe(::glDepthMask(GL_FALSE));
            blending = true;
        }
        GLVolume&         volume = *item.volume;
        const Transform3d world = volume.world_matrix();
        volume.model.set_color(item.color);
        shader->set_uniform("volume_world_matrix", world);
        shader->set_uniform("view_model_matrix", camera.view * world);
        shader->set_uniform("projection_matrix", camera.projection);
        const Matrix3d view_normal = camera.view.matrix().block(0, 0, 3, 3) * world.matrix().block(0, 0, 3, 3).inverse().transpose();
        shader->set_uniform("view_normal_matrix", view_normal);
        // The volume may have been deactivated by a gizmo, selected or hovered by the user; none of
        // that belongs in the agent's picture.
        const bool was_active = volume.is_active;
        const bool was_selected = volume.selected;
        volume.is_active = true;
        volume.selected = false;
        // Colour painting is drawn by simple_render in the filament colours; with another colour
        // scheme, a highlight or translucency the plain volume colour is what was asked for.
        const bool plain = request.color_by != "filament" || item.translucent || item.color == HIGHLIGHT;
        volume.simple_render(shader, objects, filament_colors, false, plain);
        volume.is_active = was_active;
        volume.selected = was_selected;
    }
    shader->stop_using();
    if (blending) {
        glsafe(::glDisable(GL_BLEND));
        glsafe(::glDepthMask(GL_TRUE));
    }

    json overlay_legend = json::object();
    if (!request.overlay.empty()) {
        const OverlayPlugin* overlay = RenderPlugins::find_overlay(request.overlay);
        if (overlay == nullptr || !overlay->draw)
            fail("invalid_argument", "`editor.overlay`: there is no overlay \"" + request.overlay + "\" in this build.");
        RenderTarget target;
        target.camera = &camera;
        target.plate_idx = request.plate_idx;
        target.plate_origin = origin;
        target.legend = &overlay_legend;
        overlay->draw(target, request.options);
    }

    std::vector<unsigned char> pixels;
    if (!framebuffer.read(pixels))
        gl_unavailable("the picture could not be read back");

    Picture picture = encode(pixels, request.width, request.height, request.format);
    picture.info = json::object();
    picture.info["size"] = json::array({ request.width, request.height });
    picture.info["format"] = picture.mime_type == "image/png" ? "png" : "jpeg";
    picture.info["plate"] = request.plate_idx + 1;
    picture.info["camera"] = camera.to_json();
    add_objects_in_view(picture.info, camera, instance_boxes);
    json legend = json::object();
    if (!overlay_legend.empty())
        legend["overlay"] = std::move(overlay_legend);
    legend["color_by"] = request.color_by;
    legend["colors"] = std::move(legend_entries);
    legend["background"] = hex(BACKGROUND);
    legend["bed"] = json::object({ { "fill", hex(BED_FILL) }, { "grid_mm", 10 }, { "bold_grid_mm", 50 }, { "axes", "X red, Y green, Z blue, from the plate origin" } });
    picture.info["legend"] = std::move(legend);
    return picture;
}

}}} // namespace Slic3r::GUI::Mcp
