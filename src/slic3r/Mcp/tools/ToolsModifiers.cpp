// Modifier bodies and height ranges: modifier_add, modifier_update, modifier_delete, layer_range.
//
// modifier_add repeats the model steps of ObjectList::load_generic_subobject / load_modifier
// (GUI_ObjectList.cpp) with public calls, without the GUI selection, the size dialog and the tip
// dialog: build the mesh, ModelObject::add_volume, orient the body to the bed with the inverse of the
// instance's rotation and scale, place it, name it, set its extruder, then let the object list
// rebuild its rows. Unlike the menu it does not bake the instance transform into every volume
// (apply_object_instance_transfrom_to_all_volumes): the body is placed correctly under the existing
// instance transform and the transforms of the other volumes stay untouched.
//
// Frames (docs/mcp/tools-modifiers.md):
//   object  the ModelObject's own frame, in which volume matrices are expressed (default);
//   world   the 3D scene's frame (all plates on one grid);
//   plate   world minus the origin of the plate of the object's first instance (what scene_get uses
//           for instance positions).
// World and plate points are converted through the first instance.

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>

#include "libslic3r/Geometry.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"

#include "../McpFiles.hpp"
#include "../McpIds.hpp"
#include "../McpRefresh.hpp"
#include "../McpScene.hpp"
#include "../McpSchema.hpp"
#include "../McpTool.hpp"
#include "settings/SettingsCommon.hpp"

namespace Slic3r { namespace GUI { namespace Mcp {

namespace {

using namespace SettingsEdit;

const double DEG = PI / 180.;

Plater& plater() { return *wxGetApp().plater(); }

const char* const TYPE_NAMES[] = {"part", "negative_part", "modifier", "support_blocker", "support_enforcer", "precise_seam_center",
                                  "precise_seam_left", "precise_seam_right", "precise_seam_enforced", "precise_seam_blocked",
                                  "precise_seam_neutral"};

ModelVolumeType volume_type_from(const std::string& name)
{
    static const std::vector<std::pair<std::string, ModelVolumeType>> table = {
        {"part", ModelVolumeType::MODEL_PART},
        {"negative_part", ModelVolumeType::NEGATIVE_VOLUME},
        {"modifier", ModelVolumeType::PARAMETER_MODIFIER},
        {"support_blocker", ModelVolumeType::SUPPORT_BLOCKER},
        {"support_enforcer", ModelVolumeType::SUPPORT_ENFORCER},
        {"precise_seam_center", ModelVolumeType::PRECISE_SEAM_CENTER},
        {"precise_seam_left", ModelVolumeType::PRECISE_SEAM_LEFT},
        {"precise_seam_right", ModelVolumeType::PRECISE_SEAM_RIGHT},
        {"precise_seam_enforced", ModelVolumeType::PRECISE_SEAM_ENFORCED},
        {"precise_seam_blocked", ModelVolumeType::PRECISE_SEAM_BLOCKED},
        {"precise_seam_neutral", ModelVolumeType::PRECISE_SEAM_NEUTRAL},
    };
    for (const auto& [key, type] : table)
        if (key == name)
            return type;
    fail("invalid_argument", "`type`: unknown volume type \"" + name + "\".");
}

// Settings live on parts and modifiers only (the GUI offers no settings for the other bodies).
Where settings_scope_of(ModelVolumeType type, const char* what)
{
    if (type == ModelVolumeType::MODEL_PART)
        return Where::Part;
    if (type == ModelVolumeType::PARAMETER_MODIFIER)
        return Where::Modifier;
    fail("invalid_argument", std::string("`") + what + "`: only parts and modifiers carry settings.",
         "Negative parts, support blockers/enforcers and precise-seam bodies act through their shape; use type \"modifier\" for a region "
         "with different settings.");
}

int count_parts(const ModelObject& object)
{
    int parts = 0;
    for (const ModelVolume* volume : object.volumes)
        parts += volume->is_model_part() ? 1 : 0;
    return parts;
}

int volume_index(const ModelObject& object, const ModelVolume* volume)
{
    for (size_t i = 0; i < object.volumes.size(); ++i)
        if (object.volumes[i] == volume)
            return static_cast<int>(i);
    return -1;
}

Vec3d vec3_arg(const json& value) { return Vec3d(value[0].get<double>(), value[1].get<double>(), value[2].get<double>()); }

Vec3d plate_origin_of(int obj_idx)
{
    const int plate_idx = Ids::plate_of_instance(obj_idx, 0);
    PartPlateList& plates = plater().get_partplate_list();
    if (plate_idx < 0 || plate_idx >= plates.get_plate_count())
        return Vec3d::Zero();
    return plates.get_plate(plate_idx)->get_origin();
}

json box_json(const BoundingBoxf3& box)
{
    json out = json::object();
    out["min"] = Scene::vec3(box.min.x(), box.min.y(), box.min.z());
    out["max"] = Scene::vec3(box.max.x(), box.max.y(), box.max.z());
    return out;
}

// A point given in `frame` -> the object's frame.
Vec3d to_object_frame(int obj_idx, const ModelObject& object, const Vec3d& point, const std::string& frame)
{
    if (frame == "object")
        return point;
    const Transform3d inverse = object.instances.front()->get_matrix().inverse();
    if (frame == "world")
        return inverse * point;
    return inverse * (point + plate_origin_of(obj_idx));
}

// Where the body ended up: its box in object, plate and world frames.
json placement_json(int obj_idx, const ModelObject& object, const ModelVolume& volume)
{
    const Transform3d   instance = object.instances.front()->get_matrix();
    const BoundingBoxf3 box_object = volume.mesh().transformed_bounding_box(volume.get_matrix());
    const BoundingBoxf3 box_world = volume.mesh().transformed_bounding_box(instance * volume.get_matrix());
    BoundingBoxf3       box_plate = box_world;
    box_plate.translate(-plate_origin_of(obj_idx));
    const int plate_idx = Ids::plate_of_instance(obj_idx, 0);
    json out = json::object();
    out["instance"] = Ids::of(*object.instances.front());
    if (plate_idx >= 0)
        out["plate"] = plate_idx + 1;
    else
        out["plate"] = nullptr;
    out["bbox_object"] = box_json(box_object);
    out["bbox_plate"] = box_json(box_plate);
    out["bbox_world"] = box_json(box_world);
    const Vec3d center_plate = box_plate.center();
    out["center_plate"] = Scene::vec3(center_plate.x(), center_plate.y(), center_plate.z());
    const Vec3d size = box_world.size();
    out["size_world"] = Scene::vec3(size.x(), size.y(), size.z());
    if (object.instances.size() > 1)
        out["note"] = "The object has several instances; boxes are given for the first. Every instance gets the body.";
    return out;
}

// --- shape ----------------------------------------------------------------------------------------

double default_side()
{
    return plater().canvas3D()->get_size_proportional_to_max_bed_size(0.1);
}

struct Shape
{
    TriangleMesh mesh;
    std::string  default_name;
    bool         builtin { false };
    bool         slab { false };
    json         size_used = json::array();
};

std::vector<double> sizes_from(const json& shape)
{
    std::vector<double> out;
    if (shape.contains("size"))
        for (const json& v : shape["size"])
            out.push_back(v.get<double>());
    return out;
}

// world_box: the instance's box in world coordinates (parts only), for fit and slab.
Shape make_shape(const json& shape, const BoundingBoxf3& world_box, const json& place)
{
    Shape out;
    if (shape.contains("file")) {
        const boost::filesystem::path path =
            Files::check_read(shape["file"].get<std::string>(), {".stl", ".obj", ".3mf", ".amf", ".step", ".stp", ".drc"}, "shape.file");
        Model model;
        try {
            model = Model::read_from_file(path.string(), nullptr, nullptr, LoadStrategy::LoadModel);
        } catch (const std::exception& e) {
            fail("load_failed", "Could not read \"" + path.string() + "\": " + e.what(), "Check that the file is a valid mesh.");
        }
        model.add_default_instances();
        out.mesh = model.mesh();
        if (out.mesh.facets_count() == 0)
            fail("load_failed", "\"" + path.string() + "\" holds no mesh.");
        if (shape.contains("scale"))
            out.mesh.scale(static_cast<float>(shape["scale"].get<double>()));
        out.default_name = path.filename().string();
        const Vec3d size = out.mesh.bounding_box().size();
        out.size_used = Scene::vec3(size.x(), size.y(), size.z());
        return out;
    }

    const std::string   primitive = shape["primitive"].get<std::string>();
    std::vector<double> size = sizes_from(shape);
    const bool          fit = place.is_object() && place.contains("fit");
    const double        margin = place.is_object() && place.contains("margin") ? place["margin"].get<double>() : 0.;
    auto need = [&](size_t count, const char* layout) {
        if (size.size() != count && !size.empty())
            fail("invalid_argument", "`shape.size` for a " + primitive + " is " + layout + " (" + std::to_string(count) + " number" +
                                         (count > 1 ? "s" : "") + "), got " + std::to_string(size.size()) + ".");
    };
    const double side = default_side();
    if (fit && primitive != "cube" && primitive != "slab")
        fail("invalid_argument", "`place.fit` works with the cube and slab primitives only.");

    auto angle_step = [](double radius) {
        const double step = 2. * std::acos(1. - std::min(1., 0.002 / std::max(radius, 1e-6)));
        return std::clamp(step, 2. * PI / 720., 2. * PI / 16.);
    };

    out.builtin = true;
    if (primitive == "cube") {
        if (size.size() == 1)
            size = {size[0], size[0], size[0]};
        need(3, "[x, y, z] or one edge length");
        if (size.empty())
            size = {side, side, side};
        if (fit) {
            size[0] = world_box.size().x() + 2. * margin;
            size[1] = world_box.size().y() + 2. * margin;
            if (!shape.contains("size"))
                size[2] = world_box.size().z() + 2. * margin;
        }
        out.mesh = TriangleMesh(its_make_cube(size[0], size[1], size[2]));
        out.default_name = "Generic-Cube";
    } else if (primitive == "slab") {
        need(3, "[x, y, z]");
        if (size.empty())
            size = {world_box.size().x() * 1.5, world_box.size().y() * 1.5, world_box.size().z() * 0.5};
        if (fit) {
            size[0] = world_box.size().x() + 2. * margin;
            size[1] = world_box.size().y() + 2. * margin;
        }
        out.mesh = TriangleMesh(its_make_cube(size[0], size[1], size[2]));
        out.default_name = "Generic-Slab";
        out.slab = true;
    } else if (primitive == "cylinder" || primitive == "disc") {
        need(2, "[diameter, height]");
        if (size.empty())
            size = {side, primitive == "disc" ? 0.2 : side};
        out.mesh = TriangleMesh(its_make_cylinder(0.5 * size[0], size[1], angle_step(0.5 * size[0])));
        out.default_name = primitive == "disc" ? "Generic-Disc" : "Generic-Cylinder";
    } else if (primitive == "cone") {
        need(2, "[base diameter, height]");
        if (size.empty())
            size = {side, side};
        out.mesh = TriangleMesh(its_make_cone(0.5 * size[0], size[1], angle_step(0.5 * size[0])));
        out.default_name = "Generic-Cone";
    } else if (primitive == "sphere") {
        need(1, "[diameter]");
        if (size.empty())
            size = {side};
        out.mesh = TriangleMesh(its_make_sphere(0.5 * size[0], angle_step(0.5 * size[0])));
        out.default_name = "Generic-Sphere";
    } else if (primitive == "torus") {
        need(2, "[inner diameter, tube diameter]");
        if (size.empty())
            size = {0.75 * side, 0.25 * side};
        const double tube = 0.5 * size[1];
        const double radius = 0.5 * size[0] + tube;
        out.mesh = TriangleMesh(its_make_torus(radius, tube, angle_step(radius + tube), angle_step(tube)));
        out.default_name = "Generic-Torus";
    } else {
        fail("invalid_argument", "`shape.primitive`: unknown primitive \"" + primitive + "\".");
    }
    for (double v : size)
        out.size_used.push_back(Scene::round4(v));
    return out;
}

// The body's pose: centre (object frame) and rotation about the bed axes (radians).
struct Pose
{
    Vec3d center = Vec3d::Zero();
    Vec3d rotation = Vec3d::Zero();
    std::string described;
};

// Resolves place.center_rel / place.center (+ frame) / defaults into a centre in the object frame.
Pose resolve_pose(int obj_idx, const ModelObject& object, const json& place, const BoundingBoxf3& world_box, const Shape* shape,
                  const Vec3d& fallback_object_center)
{
    Pose pose;
    const bool has_rel = place.is_object() && place.contains("center_rel");
    const bool has_center = place.is_object() && place.contains("center");
    if (has_rel && has_center)
        fail("invalid_argument", "`place`: give either `center_rel` or `center`, not both.");
    if (place.is_object() && place.contains("frame") && !has_center)
        fail("invalid_argument", "`place.frame` applies to `place.center` only.",
             "center_rel is always relative to the object's box in the object frame.");
    if (has_rel) {
        const BoundingBoxf3& box = object.raw_mesh_bounding_box();
        const Vec3d          rel = vec3_arg(place["center_rel"]);
        pose.center = box.min + Vec3d(rel.x() * box.size().x(), rel.y() * box.size().y(), rel.z() * box.size().z());
        pose.described = "center_rel";
    } else if (has_center) {
        const std::string frame = place.value("frame", std::string("object"));
        pose.center = to_object_frame(obj_idx, object, vec3_arg(place["center"]), frame);
        pose.described = "center in " + frame + " frame";
    } else if (shape != nullptr) {
        // Default: the centre of the object; a slab sits on the bed like the menu's.
        Vec3d world = world_box.center();
        if (shape->slab)
            world.z() = world_box.min.z() + 0.5 * shape->mesh.bounding_box().size().z();
        pose.center = object.instances.front()->get_matrix().inverse() * world;
        pose.described = shape->slab ? "default: on the bed under the object's centre" : "default: the object's centre";
    } else {
        pose.center = fallback_object_center;
        pose.described = "unchanged";
    }
    if (place.is_object() && place.contains("rotation"))
        pose.rotation = vec3_arg(place["rotation"]) * DEG;
    return pose;
}

// Sets the volume matrix so that the body is aligned with the bed, rotated by `rotation` about the bed
// axes (X, then Y, then Z), scaled by `scale` along its own axes, and the centre of its mesh's own
// bounding box lands on `center` (object frame).
void apply_pose(const ModelObject& object, ModelVolume& volume, const Vec3d& center, const Vec3d& rotation, const Vec3d& scale)
{
    const Transform3d instance_linear = object.instances.front()->get_transformation().get_matrix_no_offset();
    const Vec3d       mesh_center = volume.mesh().bounding_box().center();
    const Transform3d linear = instance_linear.inverse() * Geometry::rotation_transform(rotation) * Geometry::scale_transform(scale);
    const Transform3d matrix = Geometry::translation_transform(center) * linear * Geometry::translation_transform(-mesh_center);
    volume.set_transformation(Geometry::Transformation(matrix));
}

// Where the centre of the volume's mesh box is, in the object frame.
Vec3d body_center(const ModelVolume& volume)
{
    return volume.get_matrix() * volume.mesh().bounding_box().center();
}

// The body's current rotation about the bed axes and its scale along its own axes.
void current_pose(const ModelObject& object, const ModelVolume& volume, Vec3d& rotation, Vec3d& scale)
{
    const Transform3d instance_linear = object.instances.front()->get_transformation().get_matrix_no_offset();
    const Transform3d in_bed = instance_linear * volume.get_transformation().get_matrix_no_offset();
    const Geometry::Transformation t(in_bed);
    rotation = t.get_rotation();
    scale = t.get_scaling_factor();
}

void check_filament(int filament)
{
    if (filament > Ids::filament_count())
        fail("invalid_argument", "`filament`: there is no filament slot " + std::to_string(filament) + ".",
             "The project has " + std::to_string(Ids::filament_count()) + " filament slot(s); filament_list shows them.");
}

void refresh_structure(int obj_idx, bool parts_changed)
{
    if (parts_changed)
        plater().get_view3D_canvas3D()->update_instance_printable_state_for_object(static_cast<size_t>(obj_idx));
    Refresh::full();
    Refresh::object(obj_idx);
}

json changed_result(int obj_idx, const ModelObject& object, const ModelVolume* volume)
{
    json out = json::object();
    out["ok"] = true;
    out["changed"] = Scene::object_json(obj_idx, Scene::Detail::Normal);
    if (volume != nullptr) {
        const int vol_idx = volume_index(object, volume);
        out["volume"] = Scene::volume_json(object, vol_idx, Scene::Detail::Full);
        out["placement"] = placement_json(obj_idx, object, *volume);
    }
    return out;
}

void warn_precise_seam(ToolContext& ctx, int obj_idx, const ModelObject& object, const ModelVolume& volume)
{
    if (!volume.is_precise_seam())
        return;
    const BoundingBoxf3 body = volume.mesh().transformed_bounding_box(object.instances.front()->get_matrix() * volume.get_matrix());
    const BoundingBoxf3 parts = object.instance_bounding_box(0);
    const bool overlap = body.min.x() <= parts.max.x() && parts.min.x() <= body.max.x() && body.min.y() <= parts.max.y() &&
                         parts.min.y() <= body.max.y() && body.min.z() <= parts.max.z() && parts.min.z() <= body.max.z();
    if (!overlap)
        ctx.note("The precise-seam body does not touch the object's bounding box, so it has no effect: it must cross the outer wall.");
    (void) obj_idx;
}

// --- modifier_add ---------------------------------------------------------------------------------

json modifier_add(ToolContext& ctx)
{
    const ObjectRef     target = Ids::object(ctx.arg("object", ""), "object");
    ModelObject&        object = *target.object;
    const int           obj_idx = target.obj_idx;
    const std::string   type_name = ctx.arg("type", "");
    const ModelVolumeType type = volume_type_from(type_name);
    const json          place = ctx.has("place") ? ctx.args()["place"] : json::object();
    if (object.instances.empty())
        fail("invalid_argument", "Object " + Ids::of(object) + " has no instance to place a body on.");

    // Validate everything first.
    json settings = ctx.has("settings") ? ctx.args()["settings"] : json(nullptr);
    const bool has_settings = settings.is_object() && !settings.empty();
    if (has_settings) {
        const Where where = settings_scope_of(type, "settings");
        const DynamicPrintConfig reference = full_config();
        parse_values(settings, where, &reference);
    }
    const int filament = ctx.arg<int>("filament", -1);
    if (filament >= 0)
        check_filament(filament);

    const BoundingBoxf3 world_box = object.instance_bounding_box(0);
    Shape               shape = make_shape(ctx.args()["shape"], world_box, place);
    const Pose          pose = resolve_pose(obj_idx, object, place, world_box, &shape, Vec3d::Zero());

    // One undo step.
    ctx.snapshot("add " + type_name + " to " + Ids::of(object));

    ModelVolume* volume = object.add_volume(std::move(shape.mesh), type);
    apply_pose(object, *volume, pose.center, pose.rotation, Vec3d::Ones());
    volume->name = ctx.has("name") ? ctx.arg("name", "") : shape.default_name;
    // As the menu does: parts take the object's extruder, other bodies 0 (= the object's).
    int extruder = 0;
    if (type == ModelVolumeType::MODEL_PART && object.config.has("extruder"))
        extruder = object.config.opt_int("extruder");
    if (filament >= 0)
        extruder = filament;
    volume->config.set_key_value("extruder", new ConfigOptionInt(extruder));
    volume->source.is_from_builtin_objects = shape.builtin;
    if (has_settings)
        apply_to_model_config(volume->config, settings_scope_of(type, "settings"), settings, nullptr);
    object.invalidate_bounding_box();
    // The object list shows volumes in this order (ObjectList::reorder_volumes_and_get_selection).
    object.sort_volumes(true);

    refresh_structure(obj_idx, type == ModelVolumeType::MODEL_PART);
    if (has_settings)
        refresh_after_model_config(obj_idx, &object, volume);
    ctx.set_slice_invalidated();
    warn_precise_seam(ctx, obj_idx, object, *volume);

    json out = changed_result(obj_idx, object, volume);
    out["placement"]["placed_by"] = pose.described;
    out["placement"]["shape_size"] = shape.size_used;
    return out;
}

// --- modifier_update ------------------------------------------------------------------------------

json modifier_update(ToolContext& ctx)
{
    const VolumeRef target = Ids::volume(ctx.arg("target", ""), "target");
    ModelObject&    object = *target.object;
    ModelVolume&    volume = *target.volume;
    const int       obj_idx = target.obj_idx;
    const json      place = ctx.has("place") ? ctx.args()["place"] : json(nullptr);
    const bool      has_type = ctx.has("type"), has_name = ctx.has("name"), has_filament = ctx.has("filament");
    const bool      has_size = ctx.has("size"), has_scale = ctx.has("scale");
    if (!has_type && !has_name && !has_filament && place.is_null() && !has_size && !has_scale)
        fail("invalid_argument", "Nothing to change: give at least one of `type`, `name`, `filament`, `place`, `size`, `scale`.",
             "Settings of a body are changed with settings_set scope {\"volume\": id}.");
    if (has_size && has_scale)
        fail("invalid_argument", "Give either `size` or `scale`, not both.");

    ModelVolumeType new_type = volume.type();
    if (has_type) {
        new_type = volume_type_from(ctx.arg("type", ""));
        if (volume.is_model_part() && new_type != ModelVolumeType::MODEL_PART && count_parts(object) == 1)
            fail("last_part", "The type of the last solid part of " + Ids::of(object) + " cannot be changed.",
                 "Add another part first, or change a different volume.");
        if ((volume.is_svg() || volume.is_text()) &&
            (new_type == ModelVolumeType::SUPPORT_BLOCKER || new_type == ModelVolumeType::SUPPORT_ENFORCER))
            fail("invalid_argument", "A text or SVG volume cannot become a support blocker or enforcer (the GUI does not offer it).");
    }
    const int filament = ctx.arg<int>("filament", 0);
    if (has_filament)
        check_filament(filament);
    if (has_name && ctx.arg("name", "").empty())
        fail("invalid_argument", "`name` must not be empty.");

    // Current pose, to keep what the call does not change.
    Vec3d rotation, scale;
    current_pose(object, volume, rotation, scale);
    const Vec3d   old_center = body_center(volume);
    Pose          pose;
    if (!place.is_null()) {
        pose = resolve_pose(obj_idx, object, place, object.instance_bounding_box(0), nullptr, old_center);
        if (!place.contains("rotation"))
            pose.rotation = rotation;
    } else {
        pose.center = old_center;
        pose.rotation = rotation;
    }
    Vec3d new_scale = scale;
    if (has_scale)
        new_scale = vec3_arg(ctx.args()["scale"]);
    if (has_size) {
        const Vec3d mesh_size = volume.mesh().bounding_box().size();
        const Vec3d want = vec3_arg(ctx.args()["size"]);
        for (int i = 0; i < 3; ++i) {
            if (mesh_size[i] < 1e-9)
                fail("invalid_argument", "`size`: the body is flat along axis " + std::to_string(i) + "; it cannot be resized there.");
            new_scale[i] = want[i] / mesh_size[i];
        }
    }
    for (int i = 0; i < 3; ++i)
        if (!(new_scale[i] > 1e-6))
            fail("invalid_argument", "`scale` and `size` must be positive.");
    ctx.snapshot("update " + Ids::of(volume));
    const bool parts_changed = has_type && (volume.is_model_part() != (new_type == ModelVolumeType::MODEL_PART));
    if (has_type)
        volume.set_type(new_type);
    if (has_name)
        volume.name = ctx.arg("name", "");
    if (has_filament)
        volume.config.set_key_value("extruder", new ConfigOptionInt(filament));
    if (!place.is_null() || has_size || has_scale)
        apply_pose(object, volume, pose.center, pose.rotation, new_scale);
    object.invalidate_bounding_box();
    if (has_type)
        object.sort_volumes(true);
    refresh_structure(obj_idx, parts_changed);
    ctx.set_slice_invalidated();
    warn_precise_seam(ctx, obj_idx, object, volume);
    if (has_type && !volume.config.empty() && new_type != ModelVolumeType::MODEL_PART && new_type != ModelVolumeType::PARAMETER_MODIFIER &&
        volume.config.keys().size() > 1)
        ctx.note("The body keeps its setting overrides, but a " + ctx.arg("type", "") + " does not use them.");
    return changed_result(obj_idx, object, &volume);
}

// --- modifier_delete ------------------------------------------------------------------------------

json modifier_delete(ToolContext& ctx)
{
    const std::vector<ItemRef> items = Ids::items(ctx.args()["targets"], "targets");
    std::set<ModelVolume*>     doomed;
    std::set<int>              objects;
    for (size_t i = 0; i < items.size(); ++i) {
        if (items[i].kind != IdKind::Volume)
            fail("invalid_argument", "`targets[" + std::to_string(i) + "]` is not a volume id.",
                 "This tool deletes parts and modifiers (v...). Objects are deleted with the object tools.");
        doomed.insert(items[i].volume);
        objects.insert(items[i].obj_idx);
    }
    for (int obj_idx : objects) {
        const ModelObject& object = *plater().model().objects[static_cast<size_t>(obj_idx)];
        int parts_left = 0;
        for (const ModelVolume* volume : object.volumes)
            if (volume->is_model_part() && doomed.count(const_cast<ModelVolume*>(volume)) == 0)
                ++parts_left;
        if (parts_left == 0)
            fail("last_part", "Deleting these volumes would leave " + Ids::of(object) + " without a solid part.",
                 "Delete the whole object instead, or keep one part.");
        for (const ModelVolume* volume : object.volumes)
            if (doomed.count(const_cast<ModelVolume*>(volume)) != 0 && (volume->is_cut_connector() || (object.is_cut() && !volume->is_modifier() &&
                                                                                                       !volume->is_support_modifier())))
                fail("invalid_argument", Ids::of(*volume) + " belongs to a cut object; deleting it would break the cut correspondence.",
                     "Do this in the application, which asks how to handle the cut information.");
    }

    ctx.snapshot("delete " + std::to_string(doomed.size()) + " volume(s)");
    json deleted = json::array();
    for (int obj_idx : objects) {
        ModelObject& object = *plater().model().objects[static_cast<size_t>(obj_idx)];
        for (int v = static_cast<int>(object.volumes.size()) - 1; v >= 0; --v) {
            ModelVolume* volume = object.volumes[static_cast<size_t>(v)];
            if (doomed.count(volume) == 0)
                continue;
            deleted.push_back(Ids::of(*volume));
            object.delete_volume(static_cast<size_t>(v));
        }
        object.invalidate_bounding_box();
    }
    for (int obj_idx : objects)
        refresh_structure(obj_idx, true);
    ctx.set_slice_invalidated();

    json changed = json::array();
    for (int obj_idx : objects)
        changed.push_back(Scene::object_json(obj_idx, Scene::Detail::Normal));
    json out = json::object();
    out["ok"] = true;
    out["deleted"] = std::move(deleted);
    out["changed"] = std::move(changed);
    return out;
}

// --- layer_range ----------------------------------------------------------------------------------

const double Z_EPS = 1e-4;

json ranges_json(const ModelObject& object)
{
    json ranges = json::array();
    for (const auto& [range, config] : object.layer_config_ranges) {
        json item = json::object();
        item["z_min"] = Scene::round4(range.first);
        item["z_max"] = Scene::round4(range.second);
        item["overrides"] = config_overrides_json(config.get(), "extruder");
        ranges.push_back(std::move(item));
    }
    return ranges;
}

t_layer_config_ranges::iterator find_range(ModelObject& object, double z_min, double z_max)
{
    for (auto it = object.layer_config_ranges.begin(); it != object.layer_config_ranges.end(); ++it)
        if (std::abs(it->first.first - z_min) < Z_EPS && std::abs(it->first.second - z_max) < Z_EPS)
            return it;
    return object.layer_config_ranges.end();
}

[[noreturn]] void fail_no_range(const ModelObject& object, double z_min, double z_max)
{
    json details = json::object();
    details["ranges"] = ranges_json(object);
    fail("invalid_argument",
         "Object " + Ids::of(object) + " has no height range " + json(Scene::round4(z_min)).dump() + "-" + json(Scene::round4(z_max)).dump() + ".",
         "layer_range with action \"list\" shows the ranges; give their exact z_min and z_max.", details);
}

void check_bounds(const ModelObject& object, double z_min, double z_max, const t_layer_height_range* ignore)
{
    if (!(z_min >= -Z_EPS))
        fail("invalid_argument", "`z_min` must be 0 or more (got " + json(z_min).dump() + ").");
    if (!(z_max > z_min + Z_EPS))
        fail("invalid_argument", "`z_max` must be greater than `z_min` (got " + json(z_min).dump() + " - " + json(z_max).dump() + ").");
    const double height = object.instances.empty() ? 0. : object.instance_bounding_box(0).size().z();
    if (z_min > height - Z_EPS)
        fail("invalid_argument", "The range starts above the object, which is " + json(Scene::round4(height)).dump() + " mm high.",
             "Height ranges are measured from the bed, from 0 to the object's height.");
    for (const auto& [range, config] : object.layer_config_ranges) {
        if (ignore != nullptr && range == *ignore)
            continue;
        if (z_min < range.second - Z_EPS && range.first < z_max - Z_EPS) {
            json details = json::object();
            details["conflicts_with"] = {{"z_min", Scene::round4(range.first)}, {"z_max", Scene::round4(range.second)}};
            details["ranges"] = ranges_json(object);
            fail("overlap",
                 "The range " + json(Scene::round4(z_min)).dump() + "-" + json(Scene::round4(z_max)).dump() + " overlaps the existing range " +
                     json(Scene::round4(range.first)).dump() + "-" + json(Scene::round4(range.second)).dump() + ".",
                 "Ranges of one object must not overlap; they may touch. Change the bounds, or update the existing range.", details);
        }
    }
}

// The printer's limits for a layer height, as the object list applies them (GUI_ObjectList.cpp).
void check_layer_height(const DynamicPrintConfig& values_config)
{
    if (!values_config.has("layer_height"))
        return;
    const double       value = values_config.opt_float("layer_height");
    const auto&        printer = wxGetApp().preset_bundle->printers.get_edited_preset().config;
    const double       low = printer.opt_float("min_layer_height", 0);
    double             high = printer.opt_float("max_layer_height", 0);
    if (high < EPSILON)
        high = 0.75 * printer.opt_float("nozzle_diameter", 0);
    if (value < low - 1e-9 || value > high + 1e-9) {
        json details = {{"key", "layer_height"}, {"value", value}, {"min", low}, {"max", high}};
        fail("out_of_range",
             "\"layer_height\" " + json(value).dump() + " is outside the printer's limits " + json(low).dump() + "-" + json(high).dump() + " mm.",
             "Use a layer height between min_layer_height and max_layer_height of the printer preset.", details);
    }
}

json layer_range(ToolContext& ctx)
{
    const ObjectRef   target = Ids::object(ctx.arg("object", ""), "object");
    ModelObject&      object = *target.object;
    const int         obj_idx = target.obj_idx;
    const std::string action = ctx.arg("action", "list");
    const json        values = ctx.has("values") ? ctx.args()["values"] : json(nullptr);
    const json        reset = ctx.has("reset") ? ctx.args()["reset"] : json(nullptr);

    json out = json::object();
    if (action == "list") {
        for (const char* key : {"values", "reset", "new_z_min", "new_z_max"})
            if (ctx.has(key))
                fail("invalid_argument", std::string("`") + key + "` is not used by action \"list\".");
        out["object"] = Ids::of(object);
        out["layer_ranges"] = ranges_json(object);
        return out;
    }
    if (!ctx.has("z_min") || !ctx.has("z_max"))
        fail("invalid_argument", "action \"" + action + "\" needs `z_min` and `z_max`.");
    const double z_min = ctx.arg<double>("z_min", 0.);
    const double z_max = ctx.arg<double>("z_max", 0.);
    const DynamicPrintConfig reference = full_config();

    if (action == "add") {
        if (ctx.has("new_z_min") || ctx.has("new_z_max") || ctx.has("reset"))
            fail("invalid_argument", "action \"add\" takes `z_min`, `z_max` and `values`; `new_z_*` and `reset` belong to \"update\".");
        check_bounds(object, z_min, z_max, nullptr);
        DynamicPrintConfig parsed;
        for (const Parsed& item : parse_values(values, Where::LayerRange, &reference))
            parsed.set_deserialize_strict(item.key, item.text);
        check_layer_height(parsed);

        ctx.snapshot("add height range " + json(Scene::round4(z_min)).dump() + "-" + json(Scene::round4(z_max)).dump() + " to " + Ids::of(object));
        // Default config as ObjectList::get_default_layer_config: the object's (or process) layer height, extruder 0.
        DynamicPrintConfig config;
        const double layer_height = object.config.has("layer_height") ? object.config.opt_float("layer_height")
                                                                       : wxGetApp().preset_bundle->prints.get_edited_preset().config.opt_float("layer_height");
        config.set_key_value("layer_height", new ConfigOptionFloat(layer_height));
        config.set_key_value("extruder", new ConfigOptionInt(0));
        ModelConfig& range_config = object.layer_config_ranges[{z_min, z_max}];
        range_config.assign_config(std::move(config));
        apply_to_model_config(range_config, Where::LayerRange, values, nullptr);
    } else if (action == "update" || action == "delete") {
        auto it = find_range(object, z_min, z_max);
        if (it == object.layer_config_ranges.end())
            fail_no_range(object, z_min, z_max);
        const t_layer_height_range old_range = it->first;
        if (action == "delete") {
            for (const char* key : {"values", "reset", "new_z_min", "new_z_max"})
                if (ctx.has(key))
                    fail("invalid_argument", std::string("`") + key + "` is not used by action \"delete\".");
            ctx.snapshot("delete height range of " + Ids::of(object));
            object.layer_config_ranges.erase(it);
        } else {
            const double new_min = ctx.arg<double>("new_z_min", old_range.first);
            const double new_max = ctx.arg<double>("new_z_max", old_range.second);
            const bool   moved = std::abs(new_min - old_range.first) > Z_EPS || std::abs(new_max - old_range.second) > Z_EPS;
            if (!moved && values.is_null() && reset.is_null())
                fail("invalid_argument", "Nothing to update: give `new_z_min`/`new_z_max`, `values` or `reset`.");
            if (moved)
                check_bounds(object, new_min, new_max, &old_range);
            DynamicPrintConfig parsed;
            for (const Parsed& item : parse_values(values, Where::LayerRange, &reference))
                parsed.set_deserialize_strict(item.key, item.text);
            check_layer_height(parsed);
            if (!reset.is_null())
                for (const json& key : reset)
                    if (key.get<std::string>() == "layer_height")
                        fail("invalid_argument", "`reset`: a height range always has a layer height; set a new value instead of resetting it.");

            ctx.snapshot("update height range of " + Ids::of(object));
            ModelConfig config = it->second;
            const json  changes = apply_to_model_config(config, Where::LayerRange, values, reset);
            object.layer_config_ranges.erase(it);
            object.layer_config_ranges[{new_min, new_max}] = std::move(config);
            out["changes"] = changes;
        }
    } else {
        fail("invalid_argument", "`action` must be list, add, update or delete.");
    }

    refresh_after_layer_ranges(obj_idx);
    Refresh::object(obj_idx);
    ctx.set_slice_invalidated();
    bool has_modifiers = false;
    for (const ModelVolume* volume : object.volumes)
        has_modifiers = has_modifiers || volume->is_modifier();
    if (has_modifiers && action != "delete")
        ctx.note("Height-range settings apply to the object's parts, not to its modifiers: inside a modifier the modifier's own settings win.");

    out["ok"] = true;
    out["object"] = Ids::of(object);
    out["layer_ranges"] = ranges_json(object);
    out["changed"] = Scene::object_json(obj_idx, Scene::Detail::Normal);
    return out;
}

} // namespace

MCP_TOOL_GROUP(Modifiers)
{
    {
        ToolDef tool;
        tool.name = "modifier_add";
        tool.title = "Add a part, modifier, blocker, enforcer or seam body";
        tool.description =
            "Add a body to an object, as the object list's context menu does: type part, negative_part, modifier (a region with its own "
            "settings), support_blocker, support_enforcer, or a precise-seam body (precise_seam_center|left|right fix the seam inside the "
            "body; _enforced|_blocked|_neutral retype seam candidates). Shape: a primitive {primitive: cube|cylinder|sphere|cone|disc|torus|slab, "
            "size} with sizes in mm (cube [x,y,z] or one edge; cylinder/cone/disc [diameter,height]; sphere [diameter]; torus [inner diameter, "
            "tube diameter]; slab [x,y,z], default 1.5x the object's footprint and half its height) or a mesh file {file, scale}. The body is "
            "aligned with the bed (whatever the object's rotation), then rotated by place.rotation (degrees about the bed's X, Y, Z axes, in "
            "that order, around the body's centre). Placement: place.center_rel [0..1]^3 across the object's part box in the object frame "
            "([0.5,0.5,1] = top centre; same numbers as scene_get's center_rel), or place.center with place.frame object (default; the "
            "object's own frame, as scene_get volume centres) | plate (as scene_get instance positions) | world. Default: the object's centre "
            "(a slab: on the bed). place.fit object_xy sizes a cube/slab to the object's footprint plus place.margin. `filament` (0 = the "
            "object's), `settings` = per-body overrides (part and modifier only; same rules as settings_set scope volume). One undo step. "
            "Returns the object, the new volume (detail full) and `placement` with the body's box in object, plate and world frames to "
            "verify where it went. Move/resize/retype later with modifier_update; delete with modifier_delete; height ranges are layer_range.";
        tool.input_schema = Schema::parse(R"JSON({
            "type": "object",
            "properties": {
                "object": {"$ref": "#/$defs/Id", "description": "Object id (o...)."},
                "type": {"enum": ["part", "negative_part", "modifier", "support_blocker", "support_enforcer", "precise_seam_center",
                                  "precise_seam_left", "precise_seam_right", "precise_seam_enforced", "precise_seam_blocked", "precise_seam_neutral"]},
                "shape": {"oneOf": [
                    {"type": "object", "properties": {
                        "primitive": {"enum": ["cube", "cylinder", "sphere", "cone", "disc", "torus", "slab"]},
                        "size": {"type": "array", "items": {"type": "number", "exclusiveMinimum": 0}, "minItems": 1, "maxItems": 3,
                                 "description": "mm. cube [x,y,z] or [edge]; cylinder, cone, disc [diameter, height]; sphere [diameter]; torus [inner diameter, tube diameter]; slab [x,y,z]."}},
                     "required": ["primitive"], "additionalProperties": false},
                    {"type": "object", "properties": {
                        "file": {"type": "string", "minLength": 1, "description": "Mesh file (stl, obj, 3mf, amf, step, stp) inside the read roots."},
                        "scale": {"type": "number", "exclusiveMinimum": 0, "default": 1}},
                     "required": ["file"], "additionalProperties": false}]},
                "place": {"type": "object", "properties": {
                    "center_rel": {"$ref": "#/$defs/Vec3", "description": "0..1 across the object's part box (object frame); [0.5,0.5,1] is top centre."},
                    "center": {"$ref": "#/$defs/Vec3", "description": "Centre of the body in mm, in `frame`."},
                    "frame": {"$ref": "#/$defs/Frame", "description": "Frame of `center`; default object."},
                    "rotation": {"$ref": "#/$defs/Vec3", "description": "Degrees about the bed's X, Y, Z axes (applied in that order) after aligning the body to the bed."},
                    "fit": {"enum": ["object_xy"], "description": "Size a cube or slab to the object's footprint."},
                    "margin": {"type": "number", "minimum": 0, "description": "mm added on each side with fit."}},
                    "additionalProperties": false},
                "name": {"type": "string", "minLength": 1, "maxLength": 200},
                "filament": {"type": "integer", "minimum": 0, "description": "1-based filament slot; 0 = the object's."},
                "settings": {"type": "object", "description": "Per-body setting overrides {key: value} (part and modifier only)."}
            },
            "required": ["object", "type", "shape"],
            "additionalProperties": false})JSON", "modifier_add");
        tool.handler = modifier_add;
        registry.add(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "modifier_update";
        tool.title = "Retype, rename, recolour, move or resize a body";
        tool.description =
            "Change a part or body of an object: `type` (part, negative_part, modifier, support_blocker, support_enforcer, precise_seam_*; the "
            "last solid part cannot change type), `name`, `filament` (0 = the object's), and its pose: `place` (center_rel / center + frame / "
            "rotation, as in modifier_add; rotation is absolute, about the bed axes), `size` [x,y,z] mm along the body's own axes, or `scale` "
            "[x,y,z] relative to its mesh. What is not given is kept. One undo step. Returns the object, the volume and its placement boxes. "
            "Settings of the body are settings_set with scope {\"volume\": id}.";
        tool.input_schema = Schema::parse(R"JSON({
            "type": "object",
            "properties": {
                "target": {"$ref": "#/$defs/Id", "description": "Volume id (v...)."},
                "type": {"enum": ["part", "negative_part", "modifier", "support_blocker", "support_enforcer", "precise_seam_center",
                                  "precise_seam_left", "precise_seam_right", "precise_seam_enforced", "precise_seam_blocked", "precise_seam_neutral"]},
                "name": {"type": "string", "minLength": 1, "maxLength": 200},
                "filament": {"type": "integer", "minimum": 0},
                "place": {"type": "object", "properties": {
                    "center_rel": {"$ref": "#/$defs/Vec3"},
                    "center": {"$ref": "#/$defs/Vec3"},
                    "frame": {"$ref": "#/$defs/Frame"},
                    "rotation": {"$ref": "#/$defs/Vec3"}},
                    "additionalProperties": false},
                "size": {"type": "array", "items": {"type": "number", "exclusiveMinimum": 0}, "minItems": 3, "maxItems": 3},
                "scale": {"type": "array", "items": {"type": "number", "exclusiveMinimum": 0}, "minItems": 3, "maxItems": 3}
            },
            "required": ["target"],
            "additionalProperties": false})JSON", "modifier_update");
        tool.handler = modifier_update;
        registry.add(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "modifier_delete";
        tool.title = "Delete parts or bodies";
        tool.description =
            "Delete parts, negative parts, modifiers, support blockers/enforcers or seam bodies (volume ids). An object must keep at least one "
            "solid part (last_part); delete the object instead. One undo step. Returns the deleted ids and the changed objects.";
        tool.input_schema = Schema::parse(R"JSON({
            "type": "object",
            "properties": {
                "targets": {"type": "array", "items": {"$ref": "#/$defs/Id"}, "minItems": 1, "description": "Volume ids (v...)."}
            },
            "required": ["targets"],
            "additionalProperties": false})JSON", "modifier_delete");
        tool.destructive = true;
        tool.handler = modifier_delete;
        registry.add(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "layer_range";
        tool.title = "Height range modifiers";
        tool.description =
            "Height-range modifier of an object: settings that apply between two heights (mm from the bed), e.g. a finer layer_height or "
            "denser infill for z 0-5. action list | add {z_min, z_max, values} | update {z_min, z_max (identify the range), new_z_min?, "
            "new_z_max?, values?, reset?} | delete {z_min, z_max}. A range always has a layer_height (default: the object's or the process's) "
            "and takes the keys of a part plus layer_height (settings_search scope layer_range). Ranges of one object must not overlap "
            "(overlap); layer_height must be within the printer's min/max layer height. Range settings apply to parts, not to modifiers. "
            "One undo step. Returns the object's ranges.";
        tool.input_schema = Schema::parse(R"JSON({
            "type": "object",
            "properties": {
                "action": {"enum": ["list", "add", "update", "delete"], "default": "list"},
                "object": {"$ref": "#/$defs/Id"},
                "z_min": {"type": "number", "minimum": 0},
                "z_max": {"type": "number", "exclusiveMinimum": 0},
                "new_z_min": {"type": "number", "minimum": 0},
                "new_z_max": {"type": "number", "exclusiveMinimum": 0},
                "values": {"type": "object", "description": "{key: value} for the range."},
                "reset": {"type": "array", "items": {"type": "string"}, "description": "Keys to remove from the range (not layer_height)."}
            },
            "required": ["object"],
            "additionalProperties": false})JSON", "layer_range");
        tool.destructive = true;
        tool.idempotent = true;
        tool.handler = layer_range;
        registry.add(std::move(tool));
    }
}

}}} // namespace Slic3r::GUI::Mcp
