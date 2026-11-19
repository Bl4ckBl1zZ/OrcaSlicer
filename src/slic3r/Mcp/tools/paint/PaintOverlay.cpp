#include "PaintOverlay.hpp"

#if __has_include("../../McpRenderer.hpp")
#define MCP_PAINT_HAS_RENDERER 1
#endif

#ifdef MCP_PAINT_HAS_RENDERER

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "libslic3r/Color.hpp"
#include "libslic3r/FacetPaint.hpp"
#include "libslic3r/Model.hpp"
#include "slic3r/GUI/3DScene.hpp"
#include "slic3r/GUI/GLModel.hpp"
#include "slic3r/GUI/GLShader.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Gizmos/GLGizmoPainterBase.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"

#include "../../McpIds.hpp"
#include "../../McpRenderer.hpp"
#include "PaintStats.hpp"

namespace Slic3r { namespace GUI { namespace Mcp { namespace Paint {

namespace {

std::string hex(const ColorRGBA& c)
{
    char buf[8];
    auto byte = [](float v) { return static_cast<int>(std::lround(std::clamp(v, 0.f, 1.f) * 255.f)); };
    std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", byte(c.r()), byte(c.g()), byte(c.b()));
    return buf;
}

// Colours as the gizmos draw them (TriangleSelectorGUI), filament colours for colour paint.
ColorRGBA state_color(FacetPaint::Channel channel, EnforcerBlockerType state, const std::vector<ColorRGBA>& filaments)
{
    if (channel == FacetPaint::Channel::Color) {
        const size_t i = static_cast<size_t>(state) - 1;
        return i < filaments.size() ? filaments[i] : ColorRGBA(0.5f, 0.5f, 0.5f, 1.f);
    }
    if (state == EnforcerBlockerType::BLOCKER)
        return TriangleSelectorGUI::blockers_color;
    if (channel == FacetPaint::Channel::Seam)
        return ColorRGBA(0.2f, 0.4f, 1.f, 1.f); // seam enforcers blue, so they differ from support enforcers in a legend
    return TriangleSelectorGUI::enforcers_color;
}

struct DrawState
{
    GLShaderProgram* shader { nullptr };
    GLboolean        cull { GL_FALSE };
    GLint            depth_func { GL_LESS };
};

DrawState begin_draw(const CameraView& camera)
{
    DrawState s;
    s.shader = wxGetApp().get_shader("flat");
    if (s.shader == nullptr)
        return s;
    glsafe(::glGetBooleanv(GL_CULL_FACE, &s.cull));
    glsafe(::glGetIntegerv(GL_DEPTH_FUNC, &s.depth_func));
    glsafe(::glDisable(GL_CULL_FACE));
    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glDepthFunc(GL_LEQUAL));
    // Pull the overlay towards the eye so it wins over the surface it lies on.
    glsafe(::glEnable(GL_POLYGON_OFFSET_FILL));
    glsafe(::glPolygonOffset(-2.f, -4.f));
    s.shader->start_using();
    s.shader->set_uniform("projection_matrix", camera.projection);
    return s;
}

void end_draw(const DrawState& s)
{
    if (s.shader == nullptr)
        return;
    s.shader->stop_using();
    glsafe(::glDisable(GL_POLYGON_OFFSET_FILL));
    glsafe(::glDepthFunc(s.depth_func));
    if (s.cull)
        glsafe(::glEnable(GL_CULL_FACE));
}

void draw_its(DrawState& s, const CameraView& camera, const Transform3d& world, const indexed_triangle_set& its, const ColorRGBA& color)
{
    if (its.indices.empty())
        return;
    GLModel model;
    model.init_from(its);
    model.set_color(color);
    s.shader->set_uniform("view_model_matrix", camera.view * world);
    model.render(s.shader);
}

// Calls fn(object, instance, volume, world matrix) for every model part of every instance on the plate.
template<class Fn> void for_each_part_on_plate(int plate_idx, Fn fn)
{
    const Model& model = wxGetApp().plater()->model();
    for (int o = 0; o < static_cast<int>(model.objects.size()); ++o) {
        const ModelObject* object = model.objects[static_cast<size_t>(o)];
        for (int i = 0; i < static_cast<int>(object->instances.size()); ++i) {
            if (Ids::plate_of_instance(o, i) != plate_idx)
                continue;
            const ModelInstance* instance = object->instances[static_cast<size_t>(i)];
            for (const ModelVolume* volume : object->volumes)
                if (volume->is_model_part())
                    fn(*object, *instance, *volume, Transform3d(instance->get_matrix() * volume->get_matrix()));
        }
    }
}

void draw_channel(const Renderer::RenderTarget& target, FacetPaint::Channel channel)
{
    std::vector<ColorRGBA> filaments;
    for (const std::string& c : wxGetApp().plater()->get_extruder_colors_from_plater_config()) {
        ColorRGBA rgba;
        decode_color(c, rgba);
        filaments.push_back(rgba);
    }
    const int max_state = static_cast<int>(FacetPaint::max_state(channel));
    json      states = json::object();
    double    area = 0.;
    DrawState s = begin_draw(*target.camera);
    if (s.shader != nullptr) {
        for_each_part_on_plate(target.plate_idx, [&](const ModelObject&, const ModelInstance&, const ModelVolume& volume, const Transform3d& world) {
            const FacetsAnnotation& annotation = FacetPaint::annotation(volume, channel);
            if (annotation.empty())
                return;
            for (int st = 1; st <= max_state; ++st) {
                const EnforcerBlockerType state = static_cast<EnforcerBlockerType>(st);
                const indexed_triangle_set its = annotation.get_facets_strict(volume, state);
                if (its.indices.empty())
                    continue;
                const ColorRGBA color = state_color(channel, state, filaments);
                draw_its(s, *target.camera, world, its, color);
                states[state_name(channel, state)] = hex(color);
                area += double(its.indices.size()); // only "something was drawn"
            }
        });
    }
    end_draw(s);
    json& legend = *target.legend;
    legend["paint_channel"] = channel_name(channel);
    legend["states"] = std::move(states);
    legend["note"] = "Painted states drawn flat over the parts, in these colours; unpainted surface keeps its normal colour.";
    if (area == 0.)
        legend["empty"] = true;
}

void draw_overhang(const Renderer::RenderTarget& target, const json& options)
{
    double threshold = 45.;
    if (options.is_object() && options.contains("overhang_threshold_deg") && options["overhang_threshold_deg"].is_number())
        threshold = options["overhang_threshold_deg"].get<double>();
    const ColorRGBA color(1.f, 0.35f, 0.f, 1.f);
    const double    cos_limit = std::cos(threshold * M_PI / 180.);
    double          area = 0.;
    DrawState       s = begin_draw(*target.camera);
    if (s.shader != nullptr) {
        for_each_part_on_plate(target.plate_idx, [&](const ModelObject&, const ModelInstance&, const ModelVolume& volume, const Transform3d& world) {
            const indexed_triangle_set& mesh = volume.mesh().its;
            const Matrix3d              normal_matrix = world.linear().inverse().transpose();
            indexed_triangle_set        its;
            its.vertices = mesh.vertices;
            for (const stl_triangle_vertex_indices& f : mesh.indices) {
                const Vec3d a = mesh.vertices[f[0]].cast<double>(), b = mesh.vertices[f[1]].cast<double>(), c = mesh.vertices[f[2]].cast<double>();
                const Vec3d n = (normal_matrix * (b - a).cross(c - a)).normalized();
                // As FacetPaint's overhang selector: normal within threshold of straight down.
                if (-n.z() >= cos_limit - 1e-6) {
                    its.indices.push_back(f);
                    area += 0.5 * ((world * b - world * a).cross(world * c - world * a)).norm();
                }
            }
            draw_its(s, *target.camera, world, its, color);
        });
    }
    end_draw(s);
    json& legend = *target.legend;
    legend["overhang"] = hex(color);
    legend["threshold_deg"] = threshold;
    legend["area_mm2"] = std::round(area * 1000.) / 1000.;
    legend["note"] = "Faces whose normal is within threshold_deg of straight down (paint_apply's overhang selector), drawn flat.";
}

} // namespace

void register_overlays()
{
    struct Entry
    {
        const char*         name;
        FacetPaint::Channel channel;
        const char*         description;
    };
    const Entry entries[] = {
        { "support_paint", FacetPaint::Channel::Support, "support enforcers (green) and blockers (red) as painted, flat over the parts" },
        { "seam_paint", FacetPaint::Channel::Seam, "seam enforcers (blue) and blockers (red)" },
        { "color_paint", FacetPaint::Channel::Color, "colour painting, flat in the filament colours" },
        { "fuzzy_paint", FacetPaint::Channel::FuzzySkin, "fuzzy skin painting (green)" },
    };
    for (const Entry& e : entries) {
        OverlayPlugin plugin;
        plugin.name = e.name;
        plugin.description = e.description;
        const FacetPaint::Channel channel = e.channel;
        plugin.draw = [channel](const Renderer::RenderTarget& target, const json&) { draw_channel(target, channel); };
        RenderPlugins::add_overlay(std::move(plugin));
    }
    OverlayPlugin overhang;
    overhang.name = "overhang";
    overhang.description = "faces within editor.overhang_threshold_deg (default 45) of facing straight down, in orange, including faces "
                           "on the bed (as the support painting gizmo)";
    overhang.options_schema = json::object({{"overhang_threshold_deg",
        json::object({{"type", "number"}, {"minimum", 0}, {"maximum", 90}, {"default", 45}, {"description", "Used by overlay \"overhang\"."}})}});
    overhang.draw = [](const Renderer::RenderTarget& target, const json& options) { draw_overhang(target, options); };
    RenderPlugins::add_overlay(std::move(overhang));
}

bool overlays_available() { return true; }

}}}} // namespace Slic3r::GUI::Mcp::Paint

#else // no core renderer in this tree yet

namespace Slic3r { namespace GUI { namespace Mcp { namespace Paint {

void register_overlays() {}
bool overlays_available() { return false; }

}}}} // namespace Slic3r::GUI::Mcp::Paint

#endif
