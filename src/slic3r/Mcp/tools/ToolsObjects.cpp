// Object tools: object_add_primitive, object_transform, object_duplicate, delete, object_split,
// object_merge, object_cut, object_arrange, object_orient, mesh_edit.
//
// Edits are made on the Model and followed by the refresh recipes (McpRefresh.hpp), the way the
// object manipulation panel works, so the user's selection is left alone. Exceptions are noted in
// docs/mcp/tools-objects.md.

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/CutUtils.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_Factories.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"

#include "../McpGeometry.hpp"
#include "../McpIds.hpp"
#include "../McpRefresh.hpp"
#include "../McpScene.hpp"
#include "../McpSchema.hpp"
#include "../McpTool.hpp"
#include "../McpUtil.hpp"
#include "edit/EditCommon.hpp"
#include "edit/EditJobs.hpp"

namespace Slic3r { namespace GUI { namespace Mcp {

namespace {

using namespace Edit;

const double DEG = PI / 180.;

Vec3d vec3_arg(const json& v) { return Vec3d(v[0].get<double>(), v[1].get<double>(), v[2].get<double>()); }

json vec3_json(const Vec3d& v) { return Scene::vec3(v.x(), v.y(), v.z()); }

Vec3d axis_arg(const json& axis, const char* what)
{
    if (axis.is_string()) {
        const std::string a = axis.get<std::string>();
        if (a == "x") return Vec3d::UnitX();
        if (a == "y") return Vec3d::UnitY();
        if (a == "z") return Vec3d::UnitZ();
        fail("invalid_argument", std::string("`") + what + "` must be \"x\", \"y\", \"z\" or a vector.");
    }
    Vec3d v = vec3_arg(axis);
    if (v.norm() < 1e-9)
        fail("invalid_argument", std::string("`") + what + "` must not be the zero vector.");
    return v.normalized();
}

std::string object_id_of(int obj_idx) { return Ids::of(*model().objects[static_cast<size_t>(obj_idx)]); }

// ---------------------------------------------------------------------------------------------
// object_add_primitive
// ---------------------------------------------------------------------------------------------

double primitive_angle_step(double radius)
{
    // As GUI_ObjectList.cpp: facets within 0.002 mm of the circle, 16 to 720 segments.
    const double step = 2. * std::acos(1. - std::min(1., 0.002 / radius));
    return std::clamp(step, 2. * PI / 720., 2. * PI / 16.);
}

json object_add_primitive(ToolContext& ctx)
{
    const std::string shape = ctx.arg("shape", "");
    const int         plate_idx = Ids::plate(ctx.has("plate") ? ctx.args()["plate"] : json(), "plate");
    if (plates().get_plate(plate_idx)->is_locked())
        fail("plate_locked", "Plate " + std::to_string(plate_idx + 1) + " is locked.", "Unlock it with plate_manage or choose another plate.");

    // Handy models: the GUI's "Add handy model" menu, loaded from the application's resources.
    if (shape == "handy_model") {
        const std::string key = ctx.arg("model", "");
        const auto&       models = MenuFactory::handy_models();
        auto              it = std::find_if(models.begin(), models.end(), [&key](const MenuFactory::HandyModel& m) { return m.key == key; });
        if (it == models.end()) {
            json keys = json::array();
            for (const auto& m : models)
                keys.push_back(m.key);
            fail("invalid_argument", "`model`: unknown handy model \"" + key + "\".", "Use one of the keys in details.models.", json::object({{"models", keys}}));
        }
        std::vector<boost::filesystem::path> files;
        for (const auto& name : it->file_names)
            files.push_back(boost::filesystem::path(Slic3r::resources_dir()) / "handy_models" / name);
        ctx.snapshot("add handy model " + key);
        const int previous = plates().get_curr_plate_index();
        if (previous != plate_idx)
            plater().select_plate(plate_idx);
        const std::set<size_t> before = object_id_set();
        plater().load_files(files, LoadStrategy::LoadModel);
        if (previous != plate_idx)
            plater().select_plate(previous);
        const std::vector<int> created = new_object_indices(before);
        if (created.empty())
            fail("load_failed", "The handy model \"" + key + "\" could not be loaded.", "See dialogs.");
        ctx.set_slice_invalidated();
        json out = json::object();
        out["ok"] = true;
        out["created"] = objects_json(created);
        return out;
    }

    const std::vector<double> size = ctx.has("size") ? ctx.args()["size"].get<std::vector<double>>() : std::vector<double>();
    const size_t wanted = shape == "cube" ? 3 : shape == "sphere" ? 1 : 2;
    if (!size.empty() && size.size() != wanted)
        fail("invalid_argument", "`size` for a " + shape + " has " + std::to_string(wanted) + " number(s), got " + std::to_string(size.size()) + ".",
             shape == "cube"   ? "cube: [x, y, z]" :
             shape == "sphere" ? "sphere: [diameter]" :
             shape == "torus"  ? "torus: [inner diameter, tube diameter]" : shape + ": [diameter, height]");
    // Defaults as the GUI's: a tenth of the larger bed side; a disc is 0.2 mm high.
    const Vec3d  bed = plate_size();
    const double side = std::max(10., std::round(0.1 * std::max(bed.x(), bed.y())));
    auto         s = [&](size_t i, double fallback) { return size.empty() ? fallback : size[i]; };
    TriangleMesh mesh;
    std::string  default_name;
    if (shape == "cube") {
        mesh = TriangleMesh(its_make_cube(s(0, side), s(1, side), s(2, side)));
        default_name = "Cube";
    } else if (shape == "cylinder" || shape == "disc") {
        const double d = s(0, side), h = s(1, shape == "disc" ? 0.2 : side);
        mesh = TriangleMesh(its_make_cylinder(0.5 * d, h, primitive_angle_step(0.5 * d)));
        default_name = shape == "disc" ? "Disc" : "Cylinder";
    } else if (shape == "sphere") {
        const double d = s(0, side);
        mesh = TriangleMesh(its_make_sphere(0.5 * d, primitive_angle_step(0.5 * d)));
        default_name = "Sphere";
    } else if (shape == "cone") {
        const double d = s(0, side), h = s(1, side);
        mesh = TriangleMesh(its_make_cone(0.5 * d, h, primitive_angle_step(0.5 * d)));
        default_name = "Cone";
    } else if (shape == "torus") {
        const double tube = 0.5 * s(1, 0.25 * side);
        const double radius = 0.5 * s(0, 0.75 * side) + tube;
        mesh = TriangleMesh(its_make_torus(radius, tube, primitive_angle_step(radius + tube), primitive_angle_step(tube)));
        default_name = "Torus";
    } else
        fail("invalid_argument", "Unknown `shape` \"" + shape + "\".", "Use cube, cylinder, sphere, cone, disc, torus or handy_model.");
    const std::string name = ctx.arg("name", default_name.c_str());

    ctx.snapshot("add " + shape);
    Model&       m = model();
    ModelObject* object = m.add_object();
    object->name = name;
    object->add_instance();
    const BoundingBoxf3 bb = mesh.bounding_box();
    ModelVolume*        volume = object->add_volume(mesh);
    volume->name = name;
    object->sort_volumes(true);
    object->config.set_key_value("extruder", new ConfigOptionInt(1));
    object->invalidate_bounding_box();
    object->translate(-bb.center());
    Vec3d target;
    if (ctx.has("center")) {
        const json& c = ctx.args()["center"];
        target = plate_origin(plate_idx) + Vec3d(c[0].get<double>(), c[1].get<double>(), 0.);
    } else
        target = plate_center(plate_idx);
    object->instances[0]->set_offset(Vec3d(target.x(), target.y(), -object->origin_translation.z()));
    object->ensure_on_bed();
    object->instances[0]->set_assemble_transformation(object->instances[0]->get_transformation());
    refresh_structure();
    ctx.set_slice_invalidated();
    json out = json::object();
    out["ok"] = true;
    out["created"] = objects_json({static_cast<int>(m.objects.size()) - 1});
    return out;
}

// ---------------------------------------------------------------------------------------------
// object_transform
// ---------------------------------------------------------------------------------------------

// The pieces of one target: an instance (object frame = its world matrix) or a volume (matrix in its
// object's frame).
struct TransformTarget
{
    int            obj_idx { -1 };
    ModelInstance* instance { nullptr };
    ModelVolume*   volume { nullptr };
};

BoundingBoxf3 instance_box(const ModelObject& object, const ModelInstance& instance)
{
    return object.instance_bounding_box(instance, false);
}

// Bounding box of a volume in its object's frame.
BoundingBoxf3 volume_box(const ModelVolume& volume) { return volume.mesh().transformed_bounding_box(volume.get_matrix()); }

Transform3d about(const Vec3d& pivot, const Transform3d& op)
{
    return Slic3r::Geometry::translation_transform(pivot) * op * Slic3r::Geometry::translation_transform(-pivot);
}

Transform3d rotation_about_axis(const Vec3d& axis, double angle_rad)
{
    Transform3d t = Transform3d::Identity();
    t.linear() = Eigen::AngleAxisd(angle_rad, axis).toRotationMatrix();
    return t;
}

Transform3d linear_only(const Matrix3d& m)
{
    Transform3d t = Transform3d::Identity();
    t.linear() = m;
    return t;
}

// Rotation that takes direction `from` onto `to`.
Matrix3d rotation_between(const Vec3d& from, const Vec3d& to)
{
    return Eigen::Quaterniond::FromTwoVectors(from.normalized(), to.normalized()).toRotationMatrix();
}

json object_transform(ToolContext& ctx)
{
    Model&                     m = model();
    const std::vector<ItemRef> refs = Ids::items(ctx.args()["targets"], "targets");
    const json&                a = ctx.args();
    static const char* const   ops[] = {"translate", "move_to", "rotate", "set_rotation", "scale", "set_scale", "set_size", "mirror", "reset", "lay_flat"};
    bool any = false;
    for (const char* op : ops)
        any = any || a.contains(op);
    if (!any && !a.contains("drop_to_bed"))
        fail("invalid_argument", "Nothing to do: give at least one of translate, move_to, rotate, set_rotation, scale, set_scale, set_size, mirror, reset, lay_flat.");
    const bool        drop = ctx.arg<bool>("drop_to_bed", true);
    const std::string frame = ctx.arg("frame", "plate");

    // Expand objects to their instances; keep volumes.
    std::vector<TransformTarget> targets;
    std::set<size_t>             seen;
    bool                         has_volumes = false, has_instances = false;
    for (const ItemRef& ref : refs) {
        if (ref.kind == IdKind::Volume) {
            if (seen.insert(ref.volume->id().id).second)
                targets.push_back({ref.obj_idx, nullptr, ref.volume});
            has_volumes = true;
        } else if (ref.kind == IdKind::Instance) {
            if (seen.insert(ref.instance->id().id).second)
                targets.push_back({ref.obj_idx, ref.instance, nullptr});
            has_instances = true;
        } else {
            for (ModelInstance* instance : ref.object->instances)
                if (seen.insert(instance->id().id).second)
                    targets.push_back({ref.obj_idx, instance, nullptr});
            has_instances = true;
            if (ref.object->instances.size() > 1 && a.contains("move_to") && a["move_to"].contains("center"))
                fail("invalid_argument", "`move_to.center` with " + Ids::of(*ref.object) + ", which has " + std::to_string(ref.object->instances.size()) +
                     " instances, would stack them on one spot.", "Pass the instance ids (i...) to place each instance, or use translate.");
        }
    }
    if (has_volumes && (a.contains("move_to") || a.contains("lay_flat") || a.contains("set_size")))
        fail("invalid_argument", "move_to, lay_flat and set_size apply to objects and instances, not to parts (v...).",
             "Move a part with translate (object frame) or rotate/scale it.");

    // Validate values.
    if (a.contains("scale")) {
        const json& s = a["scale"];
        const Vec3d v = s.is_number() ? Vec3d::Constant(s.get<double>()) : vec3_arg(s);
        if (v.minCoeff() <= 0.)
            fail("invalid_argument", "`scale` factors must be greater than 0.", "Use mirror to flip.");
    }
    if (a.contains("set_scale")) {
        const json& s = a["set_scale"];
        const Vec3d v = s.is_number() ? Vec3d::Constant(s.get<double>()) : vec3_arg(s);
        if (v.minCoeff() <= 0.)
            fail("invalid_argument", "`set_scale` factors must be greater than 0.", "Use mirror to flip.");
    }
    int move_plate = -1;
    if (a.contains("move_to") && a["move_to"].contains("plate")) {
        move_plate = Ids::plate(a["move_to"]["plate"], "move_to.plate");
        if (plates().get_plate(move_plate)->is_locked())
            fail("plate_locked", "Plate " + std::to_string(move_plate + 1) + " is locked; nothing can be moved onto it.",
                 "Unlock it with plate_manage action \"unlock\".");
    }
    if (a.contains("lay_flat") && a["lay_flat"].contains("facet")) {
        for (const TransformTarget& t : targets) {
            const ModelObject* object = m.objects[static_cast<size_t>(t.obj_idx)];
            const ModelVolume* volume = nullptr;
            if (a["lay_flat"].contains("volume")) {
                const VolumeRef ref = Ids::volume(a["lay_flat"]["volume"].get<std::string>(), "lay_flat.volume");
                if (ref.object != object)
                    fail("invalid_argument", "`lay_flat.volume` " + Ids::of(*ref.volume) + " is not a part of " + Ids::of(*object) + ".");
                volume = ref.volume;
            } else {
                int parts = 0;
                for (const ModelVolume* v : object->volumes)
                    if (v->is_model_part()) {
                        ++parts;
                        volume = v;
                    }
                if (parts != 1)
                    fail("invalid_argument", Ids::of(*object) + " has " + std::to_string(parts) + " parts; `lay_flat.facet` needs `lay_flat.volume` to say which.",
                         "Add lay_flat.volume with the part's id (v...).");
            }
            const long long facet = a["lay_flat"]["facet"].get<long long>();
            if (facet < 0 || facet >= static_cast<long long>(volume->mesh().its.indices.size()))
                fail("invalid_argument", "`lay_flat.facet` " + std::to_string(facet) + " is out of range: " + Ids::of(*volume) + " has " +
                     std::to_string(volume->mesh().its.indices.size()) + " facets.", "Facet ids come from mesh_query.");
        }
    }

    std::vector<std::string> names;
    for (const ItemRef& ref : refs)
        names.push_back(ref.kind == IdKind::Object ? Ids::of(*ref.object) : ref.kind == IdKind::Instance ? Ids::of(*ref.instance) : Ids::of(*ref.volume));
    ctx.snapshot("transform " + ids_summary(names));

    std::set<int> changed;
    json          details = json::array();
    for (TransformTarget& t : targets) {
        ModelObject* object = m.objects[static_cast<size_t>(t.obj_idx)];
        changed.insert(t.obj_idx);
        json detail = json::object();

        if (t.volume != nullptr) {
            // ---------------- a part: its matrix in the object's frame ----------------
            ModelVolume* volume = t.volume;
            auto         center = [&]() { return volume_box(*volume).center(); };
            const bool   local = frame == "local";
            if (a.contains("reset")) {
                const Vec3d c = center();
                Slic3r::Geometry::Transformation tr = volume->get_transformation();
                for (const auto& what : a["reset"]) {
                    const std::string w = what.get<std::string>();
                    if (w == "rotation") tr.set_rotation(Vec3d::Zero());
                    if (w == "scale") tr.set_scaling_factor(Vec3d::Ones());
                    if (w == "mirror") tr.set_mirror(Vec3d::Ones());
                }
                volume->set_transformation(tr);
                volume->set_offset(volume->get_offset() + c - center());
            }
            if (a.contains("set_scale")) {
                const json& s = a["set_scale"];
                const Vec3d c = center();
                Slic3r::Geometry::Transformation tr = volume->get_transformation();
                tr.set_scaling_factor(s.is_number() ? Vec3d::Constant(s.get<double>()) : vec3_arg(s));
                volume->set_transformation(tr);
                volume->set_offset(volume->get_offset() + c - center());
            }
            if (a.contains("scale")) {
                const json& s = a["scale"];
                const Vec3d f = s.is_number() ? Vec3d::Constant(s.get<double>()) : vec3_arg(s);
                const Transform3d V = volume->get_matrix();
                const Vec3d pivot_local = V.inverse() * center();
                volume->set_transformation(V * about(pivot_local, Slic3r::Geometry::scale_transform(f)));
            }
            if (a.contains("set_rotation")) {
                const Vec3d c = center();
                Slic3r::Geometry::Transformation tr = volume->get_transformation();
                tr.set_rotation(vec3_arg(a["set_rotation"]) * DEG);
                volume->set_transformation(tr);
                volume->set_offset(volume->get_offset() + c - center());
            }
            if (a.contains("rotate")) {
                const json& r = a["rotate"];
                const Vec3d axis = axis_arg(r["axis"], "rotate.axis");
                const double angle = r["angle_deg"].get<double>() * DEG;
                const bool origin = r.value("about", std::string("center")) == "origin";
                const Transform3d V = volume->get_matrix();
                if (local) {
                    const Vec3d pivot_local = origin ? Vec3d::Zero() : Vec3d(V.inverse() * center());
                    volume->set_transformation(V * about(pivot_local, rotation_about_axis(axis, angle)));
                } else {
                    const Vec3d pivot = origin ? volume->get_offset() : center();
                    volume->set_transformation(about(pivot, rotation_about_axis(axis, angle)) * V);
                }
            }
            if (a.contains("mirror")) {
                const Vec3d axis = axis_arg(a["mirror"], "mirror");
                const Transform3d mir = linear_only(Matrix3d::Identity() - 2. * axis * axis.transpose());
                const Transform3d V = volume->get_matrix();
                if (local)
                    volume->set_transformation(V * about(V.inverse() * center(), mir));
                else
                    volume->set_transformation(about(center(), mir) * V);
            }
            if (a.contains("translate")) {
                Vec3d d = vec3_arg(a["translate"]);
                if (local)
                    d = volume->get_matrix().linear() * d;
                volume->set_offset(volume->get_offset() + d);
            }
            object->invalidate_bounding_box();
            detail["id"] = Ids::of(*volume);
            const BoundingBoxf3 box = volume_box(*volume);
            detail["center_object"] = vec3_json(box.center());
            detail["size"] = vec3_json(box.size());
            details.push_back(std::move(detail));
            continue;
        }

        // ---------------- an instance: world matrix ----------------
        ModelInstance* instance = t.instance;
        const int      inst_idx = static_cast<int>(std::find(object->instances.begin(), object->instances.end(), instance) - object->instances.begin());
        auto           center = [&]() { return instance_box(*object, *instance).center(); };
        auto           set_matrix = [&](const Transform3d& M) { instance->set_transformation(Slic3r::Geometry::Transformation(M)); object->invalidate_bounding_box(); };
        auto           keep_center = [&](const Vec3d& c) { instance->set_offset(instance->get_offset() + c - center()); object->invalidate_bounding_box(); };
        const bool     local = frame == "local";

        if (a.contains("reset")) {
            const Vec3d c = center();
            Slic3r::Geometry::Transformation tr = instance->get_transformation();
            for (const auto& what : a["reset"]) {
                const std::string w = what.get<std::string>();
                if (w == "rotation") tr.set_rotation(Vec3d::Zero());
                if (w == "scale") tr.set_scaling_factor(Vec3d::Ones());
                if (w == "mirror") tr.set_mirror(Vec3d::Ones());
            }
            instance->set_transformation(tr);
            object->invalidate_bounding_box();
            keep_center(c);
        }
        if (a.contains("set_scale")) {
            const json& s = a["set_scale"];
            const Vec3d c = center();
            Slic3r::Geometry::Transformation tr = instance->get_transformation();
            tr.set_scaling_factor(s.is_number() ? Vec3d::Constant(s.get<double>()) : vec3_arg(s));
            instance->set_transformation(tr);
            object->invalidate_bounding_box();
            keep_center(c);
        }
        if (a.contains("set_size")) {
            const json& ss = a["set_size"];
            const Vec3d c = center();
            // Size in the instance's own axes: the parts' box in the object frame times the scale.
            BoundingBoxf3 local_box;
            for (const ModelVolume* v : object->volumes)
                if (v->is_model_part())
                    local_box.merge(volume_box(*v));
            const Vec3d raw = local_box.size();
            Slic3r::Geometry::Transformation tr = instance->get_transformation();
            Vec3d scale = tr.get_scaling_factor();
            const bool uniform = ss.value("uniform", true);
            const json& size = ss["size"];
            double factor = 0.;
            for (int i = 0; i < 3; ++i) {
                if (size[i].is_null())
                    continue;
                const double wanted = size[i].get<double>();
                if (wanted <= 0. || raw[i] <= 0.)
                    fail("invalid_argument", "`set_size.size` values must be greater than 0.");
                if (uniform && factor == 0.)
                    factor = wanted / (raw[i] * std::abs(scale[i]));
                if (!uniform)
                    scale[i] = wanted / raw[i];
            }
            if (uniform) {
                if (factor == 0.)
                    fail("invalid_argument", "`set_size.size` gives no axis.", "Give at least one number; null leaves an axis free.");
                scale *= factor;
            }
            tr.set_scaling_factor(scale);
            instance->set_transformation(tr);
            object->invalidate_bounding_box();
            keep_center(c);
        }
        if (a.contains("scale")) {
            const json& s = a["scale"];
            const Vec3d f = s.is_number() ? Vec3d::Constant(s.get<double>()) : vec3_arg(s);
            const Transform3d M = instance->get_matrix();
            set_matrix(M * about(M.inverse() * center(), Slic3r::Geometry::scale_transform(f)));
        }
        if (a.contains("set_rotation")) {
            const Vec3d c = center();
            Slic3r::Geometry::Transformation tr = instance->get_transformation();
            tr.set_rotation(vec3_arg(a["set_rotation"]) * DEG);
            instance->set_transformation(tr);
            object->invalidate_bounding_box();
            keep_center(c);
        }
        if (a.contains("rotate")) {
            const json& r = a["rotate"];
            const Vec3d axis = axis_arg(r["axis"], "rotate.axis");
            const double angle = r["angle_deg"].get<double>() * DEG;
            const bool origin = r.value("about", std::string("center")) == "origin";
            const Transform3d M = instance->get_matrix();
            if (local) {
                const Vec3d pivot_local = origin ? Vec3d::Zero() : Vec3d(M.inverse() * center());
                set_matrix(M * about(pivot_local, rotation_about_axis(axis, angle)));
            } else {
                const Vec3d pivot = origin ? instance->get_offset() : center();
                set_matrix(about(pivot, rotation_about_axis(axis, angle)) * M);
            }
        }
        if (a.contains("mirror")) {
            const Vec3d axis = axis_arg(a["mirror"], "mirror");
            const Transform3d mir = linear_only(Matrix3d::Identity() - 2. * axis * axis.transpose());
            const Transform3d M = instance->get_matrix();
            if (local)
                set_matrix(M * about(M.inverse() * center(), mir));
            else
                set_matrix(about(center(), mir) * M);
        }
        if (a.contains("lay_flat")) {
            const json& lf = a["lay_flat"];
            Vec3d       down_world;   // the direction that must end up pointing down (-Z)
            json        info = json::object();
            const Transform3d M = instance->get_matrix();
            if (lf.contains("facet")) {
                const ModelVolume* volume = nullptr;
                if (lf.contains("volume"))
                    volume = Ids::volume(lf["volume"].get<std::string>(), "lay_flat.volume").volume;
                else
                    for (const ModelVolume* v : object->volumes)
                        if (v->is_model_part())
                            volume = v;
                const auto&  its = volume->mesh().its;
                const auto&  f = its.indices[static_cast<size_t>(lf["facet"].get<long long>())];
                const Vec3d  p0 = its.vertices[f[0]].cast<double>(), p1 = its.vertices[f[1]].cast<double>(), p2 = its.vertices[f[2]].cast<double>();
                const Vec3d  n_mesh = (p1 - p0).cross(p2 - p0).normalized();
                const Matrix3d to_world = (M * volume->get_matrix()).linear();
                down_world = (to_world.inverse().transpose() * n_mesh).normalized();
                info["facet"] = lf["facet"];
                info["volume"] = Ids::of(*volume);
            } else if (lf.contains("normal")) {
                Vec3d n = vec3_arg(lf["normal"]);
                if (n.norm() < 1e-9)
                    fail("invalid_argument", "`lay_flat.normal` must not be the zero vector.");
                if (lf.value("frame", std::string("world")) == "object")
                    n = M.linear().inverse().transpose() * n;
                else if (lf.value("frame", std::string("world")) != "world" && lf.value("frame", std::string("world")) != "plate")
                    fail("invalid_argument", "`lay_flat.frame` must be \"world\" or \"object\".");
                down_world = n.normalized();
            } else {
                // A face of the convex hull, as mesh_query flat_faces lists them (largest first):
                // `face` is an index into that list, `auto` takes the rank-th largest.
                TriangleMesh mesh = object->raw_mesh();
                mesh.transform(M);
                const std::vector<Mcp::Geometry::FlatFace> faces = Mcp::Geometry::flat_faces(mesh);
                if (faces.empty())
                    fail("mesh_error", Ids::of(*object) + " has no flat face to lay on.");
                size_t index = 0;
                if (lf.contains("face")) {
                    index = lf["face"].get<size_t>();
                    if (index >= faces.size())
                        fail("invalid_argument", "`lay_flat.face` " + std::to_string(index) + " is out of range: " + Ids::of(*instance) + " has " +
                             std::to_string(faces.size()) + " flat faces.", "Face numbers come from mesh_query kind \"flat_faces\" for this instance.");
                } else
                    index = std::min<size_t>(static_cast<size_t>(std::max(1, lf.value("rank", 1)) - 1), faces.size() - 1);
                down_world = faces[index].normal;
                info["face"] = index;
                info["face_area_mm2"] = Scene::round4(faces[index].area);
                info["rank"] = static_cast<int>(index) + 1;
                json candidates = json::array();
                for (size_t k = 0; k < faces.size() && k < 5; ++k)
                    candidates.push_back(json::object({{"face", k}, {"normal_world", vec3_json(faces[k].normal)}, {"area_mm2", Scene::round4(faces[k].area)}}));
                info["candidates"] = candidates;
            }
            info["normal_world_before"] = vec3_json(down_world);
            const Matrix3d R = rotation_between(down_world, -Vec3d::UnitZ());
            set_matrix(about(center(), linear_only(R)) * M);
            detail["lay_flat"] = info;
        }
        if (a.contains("translate")) {
            Vec3d d = vec3_arg(a["translate"]);
            if (local)
                d = instance->get_transformation().get_rotation_matrix().linear() * d;
            instance->set_offset(instance->get_offset() + d);
            object->invalidate_bounding_box();
        }
        if (a.contains("move_to")) {
            const json& mv = a["move_to"];
            const int   current_plate = Ids::plate_of_instance(t.obj_idx, inst_idx);
            const int   target_plate = move_plate >= 0 ? move_plate : (current_plate >= 0 ? current_plate : 0);
            const Vec3d c = center();
            Vec3d       target_center = c;
            if (mv.contains("center")) {
                const Vec3d origin = frame == "world" ? Vec3d::Zero() : plate_origin(target_plate);
                target_center.x() = origin.x() + mv["center"][0].get<double>();
                target_center.y() = origin.y() + mv["center"][1].get<double>();
            } else if (move_plate >= 0) {
                if (current_plate >= 0) {
                    const Vec3d delta = plate_origin(move_plate) - plate_origin(current_plate);
                    target_center.x() += delta.x();
                    target_center.y() += delta.y();
                } else {
                    const Vec3d pc = plate_center(move_plate);
                    target_center.x() = pc.x();
                    target_center.y() = pc.y();
                }
            }
            instance->set_offset(instance->get_offset() + Vec3d(target_center.x() - c.x(), target_center.y() - c.y(), 0.));
            object->invalidate_bounding_box();
            if (mv.contains("z_min")) {
                const double z = mv["z_min"].get<double>();
                const double now = instance_box(*object, *instance).min.z();
                instance->set_offset(instance->get_offset() + Vec3d(0., 0., z - now));
                object->invalidate_bounding_box();
                if (z > 0.) {
                    object->auto_drop = false;
                    ctx.note(Ids::of(*object) + ": auto-drop switched off so it can float at z_min " + std::to_string(z) + ".");
                }
            }
        }
        // Put the lowest point on the bed unless the call placed it explicitly.
        const bool z_given = a.contains("move_to") && a["move_to"].contains("z_min");
        if (drop && !z_given) {
            const double now = instance_box(*object, *instance).min.z();
            if (std::abs(now) > 1e-9) {
                instance->set_offset(instance->get_offset() - Vec3d(0., 0., now));
                object->invalidate_bounding_box();
            }
        }
        detail["id"] = Ids::of(*instance);
        details.push_back(std::move(detail));
    }

    std::vector<int> idxs(changed.begin(), changed.end());
    // Recompute plate membership of every instance of the changed objects, then the targeted refresh.
    for (int o : idxs)
        for (int i = 0; i < static_cast<int>(m.objects[static_cast<size_t>(o)]->instances.size()); ++i)
            plates().notify_instance_update(o, i);
    Refresh::objects(idxs);
    ctx.set_slice_invalidated();

    json out = json::object();
    out["ok"] = true;
    out["changed"] = objects_json(idxs);
    bool any_detail = false;
    for (const json& d : details)
        any_detail = any_detail || d.contains("lay_flat") || d.contains("center_object");
    if (any_detail)
        out["details"] = details;
    (void) has_instances;
    return out;
}

// ---------------------------------------------------------------------------------------------
// object_duplicate
// ---------------------------------------------------------------------------------------------

json object_duplicate(ToolContext& ctx)
{
    Model&                     m = model();
    const std::vector<ItemRef> refs = Ids::items(ctx.args()["targets"], "targets");
    const int                  copies = ctx.arg<int>("copies", 1);
    const std::string          as = ctx.arg("as", "instances");
    const double               spacing = ctx.arg<double>("spacing", 5.);
    for (const ItemRef& ref : refs)
        if (ref.kind == IdKind::Volume)
            fail("invalid_argument", "`targets`: " + Ids::of(*ref.volume) + " is a part; duplicate objects or instances.", "Use the object id.");
    size_t total = 0;
    for (const ItemRef& ref : refs)
        total += static_cast<size_t>(copies) * (ref.kind == IdKind::Object ? ref.object->instances.size() : 1);
    if (total > 500)
        fail("limit", "That would create " + std::to_string(total) + " copies; at most 500 per call.", "Use fewer copies or targets.");

    std::vector<std::string> names;
    for (const ItemRef& ref : refs)
        names.push_back(ref.kind == IdKind::Object ? Ids::of(*ref.object) : Ids::of(*ref.instance));
    ctx.snapshot("duplicate " + ids_summary(names) + " x" + std::to_string(copies));

    // Copies are laid out in rows to the right of the source, `spacing` mm apart, wrapping at the
    // plate's edge; anything that does not fit stays outside the plate (object_arrange packs them).
    const Vec3d bed = plate_size();
    auto        place = [&](const BoundingBoxf3& box, int plate_idx, int k) {
        const Vec3d  origin = plate_origin(plate_idx >= 0 ? plate_idx : 0);
        const double step_x = box.size().x() + spacing, step_y = box.size().y() + spacing;
        const double start_x = box.min.x() - origin.x();
        const int    per_row = std::max(1, static_cast<int>((bed.x() - start_x - box.size().x()) / step_x) + 1);
        const int    col = k % per_row, row = k / per_row;
        const double x = box.min.x() + col * step_x;
        const double y = box.min.y() - row * step_y;
        return Vec3d(x - box.min.x(), y - box.min.y(), 0.);
    };

    std::set<int>    changed;
    std::vector<int> new_objects;
    json             created_instances = json::array();
    for (const ItemRef& ref : refs) {
        const int obj_idx = ref.obj_idx;
        ModelObject* object = m.objects[static_cast<size_t>(obj_idx)];
        std::vector<int> sources;
        if (ref.kind == IdKind::Instance)
            sources.push_back(ref.inst_idx);
        else
            for (int i = 0; i < static_cast<int>(object->instances.size()); ++i)
                sources.push_back(i);
        if (as == "instances") {
            for (int src : sources) {
                const ModelInstance* source = object->instances[static_cast<size_t>(src)];
                const BoundingBoxf3  box = object->instance_bounding_box(static_cast<size_t>(src));
                const int            plate_idx = Ids::plate_of_instance(obj_idx, src);
                for (int k = 1; k <= copies; ++k) {
                    ModelInstance* copy = object->add_instance(*source);
                    copy->set_offset(source->get_offset() + place(box, plate_idx, k));
                    created_instances.push_back(Ids::of(*copy));
                }
            }
            object->invalidate_bounding_box();
            changed.insert(obj_idx);
        } else {
            for (int src : sources) {
                const BoundingBoxf3 box = object->instance_bounding_box(static_cast<size_t>(src));
                const int           plate_idx = Ids::plate_of_instance(obj_idx, src);
                for (int k = 1; k <= copies; ++k) {
                    // `object` stays valid: add_object appends to the vector of pointers.
                    ModelObject* copy = m.add_object(*m.objects[static_cast<size_t>(obj_idx)]);
                    copy->clear_instances();
                    ModelInstance* inst = copy->add_instance(*m.objects[static_cast<size_t>(obj_idx)]->instances[static_cast<size_t>(src)]);
                    inst->set_offset(inst->get_offset() + place(box, plate_idx, k));
                    copy->invalidate_bounding_box();
                    new_objects.push_back(static_cast<int>(m.objects.size()) - 1);
                }
            }
        }
    }
    refresh_structure();
    ctx.set_slice_invalidated();

    json out = json::object();
    out["ok"] = true;
    if (as == "instances") {
        out["created"] = created_instances;
        out["changed"] = objects_json(std::vector<int>(changed.begin(), changed.end()));
    } else
        out["created"] = objects_json(new_objects);
    return out;
}

// ---------------------------------------------------------------------------------------------
// delete
// ---------------------------------------------------------------------------------------------

json delete_items(ToolContext& ctx)
{
    Model&                     m = model();
    const std::vector<ItemRef> refs = Ids::items(ctx.args()["targets"], "targets");
    std::set<int>                   whole;     // object indices to delete
    std::map<int, std::set<int>>    instances; // obj -> instance indices
    std::map<int, std::set<int>>    volumes;   // obj -> volume indices
    json removed = json::array();
    for (const ItemRef& ref : refs) {
        if (ref.kind == IdKind::Object)
            whole.insert(ref.obj_idx);
        else if (ref.kind == IdKind::Instance)
            instances[ref.obj_idx].insert(ref.inst_idx);
        else
            volumes[ref.obj_idx].insert(ref.vol_idx);
    }
    // All instances of an object = the object.
    for (auto& [o, set] : instances)
        if (set.size() == m.objects[static_cast<size_t>(o)]->instances.size())
            whole.insert(o);
    for (auto& [o, set] : volumes) {
        if (whole.count(o))
            continue;
        const ModelObject* object = m.objects[static_cast<size_t>(o)];
        int parts_left = 0;
        for (int v = 0; v < static_cast<int>(object->volumes.size()); ++v)
            if (object->volumes[static_cast<size_t>(v)]->is_model_part() && set.count(v) == 0)
                ++parts_left;
        if (parts_left == 0)
            fail("last_part", "Deleting " + std::to_string(set.size()) + " part(s) of " + Ids::of(*object) + " would leave it without a solid part.",
                 "Delete the object itself (" + Ids::of(*object) + "), or keep at least one part.");
        if (object->is_cut())
            for (int v : set)
                if (object->volumes[static_cast<size_t>(v)]->is_model_part() || object->volumes[static_cast<size_t>(v)]->is_negative_volume())
                    fail("invalid_argument", Ids::of(*object) + " is a cut object with connectors; its parts cannot be deleted one by one.",
                         "Delete the whole object, or delete its connector parts.");
    }
    std::vector<std::string> names;
    for (const ItemRef& ref : refs)
        names.push_back(ref.kind == IdKind::Object ? Ids::of(*ref.object) : ref.kind == IdKind::Instance ? Ids::of(*ref.instance) : Ids::of(*ref.volume));
    ctx.snapshot("delete " + ids_summary(names));

    for (auto& [o, set] : volumes) {
        if (whole.count(o))
            continue;
        ModelObject* object = m.objects[static_cast<size_t>(o)];
        for (auto it = set.rbegin(); it != set.rend(); ++it) {
            removed.push_back(Ids::of(*object->volumes[static_cast<size_t>(*it)]));
            object->delete_volume(static_cast<size_t>(*it));
        }
        // As the GUI: the settings of a last remaining volume move to the object.
        if (object->volumes.size() == 1 && !object->volumes[0]->config.empty()) {
            object->config.apply(object->volumes[0]->config);
            object->volumes[0]->config.reset();
        }
        object->invalidate_bounding_box();
    }
    for (auto& [o, set] : instances) {
        if (whole.count(o))
            continue;
        ModelObject* object = m.objects[static_cast<size_t>(o)];
        for (auto it = set.rbegin(); it != set.rend(); ++it) {
            removed.push_back(Ids::of(*object->instances[static_cast<size_t>(*it)]));
            object->delete_instance(static_cast<size_t>(*it));
        }
        object->invalidate_bounding_box();
    }
    for (auto it = whole.rbegin(); it != whole.rend(); ++it) {
        removed.push_back(Ids::of(*m.objects[static_cast<size_t>(*it)]));
        m.delete_object(static_cast<size_t>(*it));
    }
    refresh_structure();
    ctx.set_slice_invalidated();
    json out = json::object();
    out["ok"] = true;
    out["removed"] = removed;
    out["changed"] = Scene::summary_json();
    return out;
}

// ---------------------------------------------------------------------------------------------
// object_split
// ---------------------------------------------------------------------------------------------

json object_split(ToolContext& ctx)
{
    Model&            m = model();
    const ItemRef     ref = Ids::item(ctx.arg("target", ""), "target");
    const std::string into = ctx.arg("into", "objects");
    if (ref.kind == IdKind::Instance)
        fail("invalid_argument", "`target` is an instance; split an object or a part.", "Pass the object id (o...).");
    ModelObject* object = ref.object;
    const int    filament_count = std::max(1, Ids::filament_count());
    const bool   keep_painting = wxGetApp().app_config->get_bool("keep_painting");

    if (into == "parts") {
        ModelVolume* volume = ref.volume;
        if (volume == nullptr) {
            int parts = 0;
            for (ModelVolume* v : object->volumes)
                if (v->is_model_part()) {
                    ++parts;
                    volume = v;
                }
            if (parts != 1)
                fail("invalid_argument", Ids::of(*object) + " has " + std::to_string(parts) + " parts; say which one to split.",
                     "Pass the part's id (v...) as target.");
        }
        if (!volume->is_splittable())
            fail("not_splittable", Ids::of(*volume) + " is one connected piece and cannot be split into parts.",
                 "Only meshes made of several separate shells can be split.");
        ctx.snapshot("split " + Ids::of(*volume) + " to parts");
        volume->split(static_cast<unsigned int>(filament_count), keep_painting);
        object->input_file.clear();
        object->invalidate_bounding_box();
        refresh_structure();
        ctx.set_slice_invalidated();
        json out = json::object();
        out["ok"] = true;
        out["changed"] = objects_json({object_index_by_id(object->id().id)});
        return out;
    }

    if (ref.kind == IdKind::Volume)
        fail("invalid_argument", "Split into objects takes an object id.", "Pass the object id (o...), or into: \"parts\" for a part.");
    const std::string old_id = Ids::of(*object);
    {
        // Would the split produce more than one object? Try it on a copy, as the GUI does.
        Model        copy;
        ModelObject* trial = copy.add_object(*object);
        ModelObjectPtrs pieces;
        trial->split(&pieces, false); // the pieces belong to `copy` and go with it
        const size_t count = pieces.size();
        if (count <= 1)
            fail("not_splittable", Ids::of(*object) + " is one connected piece and cannot be split.", "Only meshes made of several separate shells can be split.");
    }
    // As Plater::priv::split_object(): split a copy, replace the object with the pieces. keep_z
    // answers the GUI's "keep floating parts at their height?" (auto-drop off for the pieces).
    const bool keep_z = ctx.arg<bool>("keep_z", false);
    ctx.snapshot("split " + Ids::of(*object) + " to objects");
    Model           scratch;
    ModelObject*    source = scratch.add_object(*object);
    ModelObjectPtrs pieces;
    source->split(&pieces, keep_painting);
    const std::set<size_t> before = object_id_set();
    const bool             auto_drop = object->auto_drop && !keep_z;
    m.delete_object(static_cast<size_t>(ref.obj_idx));
    for (ModelObject* piece : pieces) {
        ModelObject* added = m.add_object(*piece);
        added->auto_drop = auto_drop;
        added->invalidate_bounding_box();
        if (auto_drop)
            added->ensure_on_bed();
    }
    const std::vector<int> created = new_object_indices(before);
    refresh_structure();
    ctx.set_slice_invalidated();
    json out = json::object();
    out["ok"] = true;
    out["removed"] = json::array({old_id});
    out["created"] = objects_json(created);
    return out;
}

// ---------------------------------------------------------------------------------------------
// object_merge
// ---------------------------------------------------------------------------------------------

json object_merge(ToolContext& ctx)
{
    Model&                     m = model();
    const std::vector<ItemRef> refs = Ids::items(ctx.args()["targets"], "targets");
    // (object index, instance indices) taken into the assembly.
    std::map<int, std::set<int>> taken;
    for (const ItemRef& ref : refs) {
        if (ref.kind == IdKind::Volume)
            fail("invalid_argument", "`targets`: " + Ids::of(*ref.volume) + " is a part; merge objects or instances.", "Pass object ids.");
        if (ref.kind == IdKind::Instance)
            taken[ref.obj_idx].insert(ref.inst_idx);
        else
            for (int i = 0; i < static_cast<int>(ref.object->instances.size()); ++i)
                taken[ref.obj_idx].insert(i);
    }
    size_t pieces = 0;
    for (const auto& [o, set] : taken)
        pieces += set.size();
    if (pieces < 2)
        fail("invalid_argument", "Merging needs at least two objects or instances.", "Pass two or more ids.");

    std::vector<std::string> names;
    for (const ItemRef& ref : refs)
        names.push_back(ref.kind == IdKind::Object ? Ids::of(*ref.object) : Ids::of(*ref.instance));
    ctx.snapshot("merge " + ids_summary(names));

    // As ObjectList::merge(true): every volume keeps its world placement inside the new object.
    ModelObject* merged = m.add_object();
    merged->name = ctx.arg("name", "Assembly");
    merged->add_instance();
    ModelConfig& config = merged->config;
    bool first = true;
    for (const auto& [o, set] : taken) {
        const ModelObject* object = m.objects[static_cast<size_t>(o)];
        for (int i : set) {
            const Transform3d matrix = object->instances[static_cast<size_t>(i)]->get_matrix();
            for (const ModelVolume* volume : object->volumes) {
                ModelVolume* copy = merged->add_volume(*volume);
                copy->set_transformation(matrix * volume->get_matrix());
                if (object->volumes.size() > 1)
                    copy->config.assign_config(volume->config);
                if (!copy->config.option("extruder") && object->config.option("extruder"))
                    copy->config.set_key_value("extruder", new ConfigOptionInt(object->config.option("extruder")->getInt()));
            }
            if (!object->instances[static_cast<size_t>(i)]->printable) {
                merged->printable = false;
                merged->instances[0]->printable = false;
            }
        }
        for (const std::string& key : object->config.keys())
            if (!config.has(key))
                config.set_key_value(key, object->config.option(key)->clone());
        if (!object->auto_drop)
            merged->auto_drop = false;
        for (const auto& range : object->layer_config_ranges)
            merged->layer_config_ranges.emplace(range);
        first = false;
    }
    (void) first;
    merged->sort_volumes(true);
    merged->ensure_on_bed();
    merged->center_around_origin();
    merged->translate_instances(-merged->origin_translation);
    merged->origin_translation = Vec3d::Zero();
    merged->instances[0]->set_assemble_transformation(merged->instances[0]->get_transformation());
    const size_t merged_id = merged->id().id;

    // Remove what was merged: whole objects, or the instances taken from them.
    json removed = json::array();
    for (auto it = taken.rbegin(); it != taken.rend(); ++it) {
        ModelObject* object = m.objects[static_cast<size_t>(it->first)];
        if (it->second.size() == object->instances.size()) {
            removed.push_back(Ids::of(*object));
            m.delete_object(static_cast<size_t>(it->first));
        } else {
            for (auto i = it->second.rbegin(); i != it->second.rend(); ++i) {
                removed.push_back(Ids::of(*object->instances[static_cast<size_t>(*i)]));
                object->delete_instance(static_cast<size_t>(*i));
            }
            object->invalidate_bounding_box();
        }
    }
    refresh_structure();
    ctx.set_slice_invalidated();
    json out = json::object();
    out["ok"] = true;
    out["removed"] = removed;
    out["created"] = objects_json({object_index_by_id(merged_id)});
    return out;
}

// ---------------------------------------------------------------------------------------------
// object_cut
// ---------------------------------------------------------------------------------------------

json object_cut(ToolContext& ctx)
{
    Model&        m = model();
    const ItemRef ref = Ids::item(ctx.arg("target", ""), "target");
    if (ref.kind == IdKind::Volume)
        fail("invalid_argument", "`target` is a part; cut an object or an instance.", "Pass the object or instance id.");
    ModelObject* object = ref.object;
    const int    inst_idx = ref.kind == IdKind::Instance ? ref.inst_idx : 0;
    if (ref.kind == IdKind::Object && object->instances.size() > 1)
        ctx.note("The cut plane is placed relative to the first instance (" + Ids::of(*object->instances[0]) + "); all instances are cut alike.");
    ModelInstance* instance = object->instances[static_cast<size_t>(inst_idx)];
    const int      plate_idx = std::max(0, Ids::plate_of_instance(ref.obj_idx, inst_idx));
    const BoundingBoxf3 box = object->instance_bounding_box(static_cast<size_t>(inst_idx));

    // The plane: a height, or a point and a normal (plate / world / object frame).
    const json& plane = ctx.args()["plane"];
    Vec3d       point, normal;
    if (plane.contains("z")) {
        point = Vec3d(box.center().x(), box.center().y(), plane["z"].get<double>());
        normal = Vec3d::UnitZ();
    } else {
        const std::string frame = plane.value("frame", std::string("plate"));
        point = vec3_arg(plane["point"]);
        normal = vec3_arg(plane["normal"]);
        if (normal.norm() < 1e-9)
            fail("invalid_argument", "`plane.normal` must not be the zero vector.");
        if (frame == "plate")
            point += plate_origin(plate_idx);
        else if (frame == "object") {
            point = instance->get_matrix() * point;
            normal = instance->get_matrix().linear().inverse().transpose() * normal;
        }
        normal.normalize();
    }
    // The plane must cross the object.
    {
        TriangleMesh mesh = object->raw_mesh();
        mesh.transform(instance->get_matrix());
        double lo = 1e300, hi = -1e300;
        for (const auto& v : mesh.its.vertices) {
            const double d = normal.dot(v.cast<double>() - point);
            lo = std::min(lo, d);
            hi = std::max(hi, d);
        }
        if (!(lo < -1e-6 && hi > 1e-6))
            fail("invalid_argument", "The cut plane does not cross " + Ids::of(*object) + ".",
                 "Object bounding box (plate frame): z " + std::to_string(box.min.z()) + " to " + std::to_string(box.max.z()) + "; choose a plane inside it.",
                 json::object({{"bbox_world", json::object({{"min", vec3_json(box.min)}, {"max", vec3_json(box.max)}})}}));
    }
    const std::string keep = ctx.arg("keep", "both");
    ModelObjectCutAttributes attributes = ModelObjectCutAttribute::InvalidateCutInfo;
    if (keep != "lower") attributes = attributes | ModelObjectCutAttribute::KeepUpper;
    if (keep != "upper") attributes = attributes | ModelObjectCutAttribute::KeepLower;
    if (ctx.arg<bool>("as_parts", false)) {
        if (keep != "both")
            fail("invalid_argument", "`as_parts` keeps both halves as parts of one object; it cannot be combined with keep \"" + keep + "\".");
        attributes = attributes | ModelObjectCutAttribute::KeepAsParts;
    }
    if (ctx.arg<bool>("flip_upper", false)) attributes = attributes | ModelObjectCutAttribute::FlipUpper;
    if (ctx.arg<bool>("flip_lower", false)) attributes = attributes | ModelObjectCutAttribute::FlipLower;
    if (ctx.arg<bool>("place_on_cut_upper", false)) attributes = attributes | ModelObjectCutAttribute::PlaceOnCutUpper;
    if (ctx.arg<bool>("place_on_cut_lower", false)) attributes = attributes | ModelObjectCutAttribute::PlaceOnCutLower;
    if (wxGetApp().app_config->get_bool("keep_painting")) attributes = attributes | ModelObjectCutAttribute::KeepPaint;

    ctx.snapshot("cut " + Ids::of(*object));
    // Cut matrix as the Cut gizmo builds it: in world orientation, relative to the instance offset;
    // its +Z is the plane normal ("upper" side).
    Transform3d cut_matrix = Slic3r::Geometry::translation_transform(point - instance->get_offset());
    cut_matrix.linear() = rotation_between(Vec3d::UnitZ(), normal);
    const std::set<size_t> before = object_id_set();
    const std::string      old_id = Ids::of(*object);
    {
        Slic3r::Cut cut(object, inst_idx, cut_matrix, attributes);
        const ModelObjectPtrs& pieces = cut.perform_with_plane();
        if (pieces.empty())
            fail("mesh_error", "The cut produced nothing.", "Check the plane.");
        plater().apply_cut_object_to_model(static_cast<size_t>(ref.obj_idx), pieces);
    }
    const std::vector<int> created = new_object_indices(before);
    plates().reload_all_objects();
    Refresh::full();
    ctx.set_slice_invalidated();
    json out = json::object();
    out["ok"] = true;
    out["removed"] = json::array({old_id});
    out["created"] = objects_json(created);
    if (created.size() > 1)
        ctx.note("The pieces stay where they were cut, so they overlap on the plate (as after the GUI's Cut); object_arrange separates them.");
    ctx.note("The new objects are selected in the window, as after the GUI's Cut.");
    return out;
}

// ---------------------------------------------------------------------------------------------
// mesh_edit
// ---------------------------------------------------------------------------------------------

json mesh_edit(ToolContext& ctx)
{
    Model&                     m = model();
    const std::string          op = ctx.arg("op", "");
    const std::vector<ItemRef> refs = Ids::items(ctx.args()["targets"], "targets");
    // (obj_idx, vol_idx) for volume operations; object indices for object operations.
    std::vector<std::pair<int, int>> volumes;
    std::set<int>                    objects;
    for (const ItemRef& ref : refs) {
        objects.insert(ref.obj_idx);
        if (ref.kind == IdKind::Volume)
            volumes.push_back({ref.obj_idx, ref.vol_idx});
        else
            for (int v = 0; v < static_cast<int>(ref.object->volumes.size()); ++v)
                if (op != "repair" || ref.object->volumes[static_cast<size_t>(v)]->is_model_part())
                    volumes.push_back({ref.obj_idx, v});
    }
    std::sort(volumes.begin(), volumes.end());
    volumes.erase(std::unique(volumes.begin(), volumes.end()), volumes.end());

    if (op == "simplify" || op == "repair") {
        if (op == "simplify") {
            const double ratio = ctx.arg<double>("target_ratio", 0.5);
            if (ctx.has("target_ratio") && (ratio <= 0. || ratio > 1.))
                fail("invalid_argument", "`target_ratio` must be in (0, 1].", "0.5 keeps half of the facets.");
        }
        return start_mesh_job(ctx, op, volumes);
    }

    std::vector<std::string> names;
    for (const ItemRef& ref : refs)
        names.push_back(ref.kind == IdKind::Object ? Ids::of(*ref.object) : ref.kind == IdKind::Instance ? Ids::of(*ref.instance) : Ids::of(*ref.volume));

    if (op == "convert_units") {
        const std::string from = ctx.arg("from", "");
        double factor = 0.;
        if (from == "inch") factor = 25.4;
        else if (from == "m") factor = 1000.;
        else if (from == "cm") factor = 10.;
        else if (from == "to_inch") factor = 1. / 25.4;
        else if (from == "to_m") factor = 1. / 1000.;
        else fail("invalid_argument", "`from` must be inch, m, cm, to_inch or to_m.", "\"inch\" multiplies by 25.4; \"to_inch\" undoes it.");
        for (const ItemRef& ref : refs)
            if (ref.kind == IdKind::Instance)
                fail("invalid_argument", "convert_units changes the mesh of a whole object or part, not one instance.", "Pass the object or part id.");
        ctx.snapshot("convert units " + ids_summary(names));
        for (const ItemRef& ref : refs) {
            if (ref.kind == IdKind::Object) {
                ref.object->scale_mesh_after_creation(factor);
                for (ModelVolume* v : ref.object->volumes) {
                    v->source.is_converted_from_inches = from == "inch";
                    v->source.is_converted_from_meters = from == "m";
                }
            } else {
                ref.volume->scale_geometry_after_creation(Vec3f::Constant(static_cast<float>(factor)));
                ref.volume->set_offset(ref.volume->get_offset() * factor);
                ref.volume->source.is_converted_from_inches = from == "inch";
                ref.volume->source.is_converted_from_meters = from == "m";
            }
            ref.object->invalidate_bounding_box();
            ref.object->ensure_on_bed();
        }
    } else if (op == "scale_to_fit") {
        const double margin = ctx.arg<double>("margin", 0.);
        const Vec3d  bed = plate_size();
        const Vec3d  room(bed.x() - 2. * margin, bed.y() - 2. * margin, bed.z() > 0. ? bed.z() : 1e9);
        if (room.x() <= 0. || room.y() <= 0.)
            fail("invalid_argument", "`margin` leaves no room on the plate.");
        ctx.snapshot("scale to fit " + ids_summary(names));
        for (const ItemRef& ref : refs) {
            if (ref.kind == IdKind::Volume)
                fail("invalid_argument", "scale_to_fit applies to objects and instances.", "Pass the object id.");
            std::vector<ModelInstance*> list;
            if (ref.kind == IdKind::Instance)
                list.push_back(ref.instance);
            else
                list.assign(ref.object->instances.begin(), ref.object->instances.end());
            for (ModelInstance* instance : list) {
                const BoundingBoxf3 box = ref.object->instance_bounding_box(*instance);
                const Vec3d size = box.size();
                const double factor = std::min({room.x() / size.x(), room.y() / size.y(), room.z() / size.z()});
                const Vec3d c = box.center();
                Slic3r::Geometry::Transformation tr = instance->get_transformation();
                tr.set_scaling_factor(tr.get_scaling_factor() * factor);
                instance->set_transformation(tr);
                ref.object->invalidate_bounding_box();
                // Centre it on its plate.
                const int inst_idx = static_cast<int>(std::find(ref.object->instances.begin(), ref.object->instances.end(), instance) - ref.object->instances.begin());
                const Vec3d pc = plate_center(std::max(0, Ids::plate_of_instance(ref.obj_idx, inst_idx)));
                const Vec3d now = ref.object->instance_bounding_box(*instance).center();
                instance->set_offset(instance->get_offset() + Vec3d(pc.x() - now.x(), pc.y() - now.y(), 0.));
                ref.object->invalidate_bounding_box();
                (void) c;
            }
            ref.object->ensure_on_bed();
        }
    } else
        fail("invalid_argument", "Unknown `op` \"" + op + "\".", "Use simplify, repair, convert_units or scale_to_fit.");

    std::vector<int> idxs(objects.begin(), objects.end());
    for (int o : idxs) {
        plater().changed_mesh(o);
        for (int i = 0; i < static_cast<int>(m.objects[static_cast<size_t>(o)]->instances.size()); ++i)
            plates().notify_instance_update(o, i);
    }
    Refresh::objects(idxs);
    ctx.set_slice_invalidated();
    json out = json::object();
    out["ok"] = true;
    out["changed"] = objects_json(idxs);
    return out;
}

} // namespace

MCP_TOOL_GROUP(Objects)
{
    {
        ToolDef tool;
        tool.name = "object_add_primitive";
        tool.title = "Add a primitive or handy model";
        tool.description =
            "Add a new object: a primitive shape (cube [x,y,z], cylinder [diameter,height], cone [diameter,height], disc [diameter,height], "
            "sphere [diameter], torus [inner diameter, tube diameter], all mm; default size as the GUI's: a tenth of the bed) or one of "
            "the handy models the GUI ships (shape \"handy_model\", model = 3dbenchy, orca_cube, cali_cat, voron_cube, stanford_bunny, "
            "orca_badge, orca_tolerance_test, autodesk_fdm_test, orcasliced_combo, orca_string_hell). Placed on `plate` (default "
            "current) at `center` [x, y] in that plate's frame (default: plate centre). Parts and modifiers inside an object are "
            "modifier_add. One undo step. Returns the new object(s) in `created`.";
        tool.input_schema = Schema::parse(R"JSON({
            "type": "object",
            "properties": {
                "shape": {"enum": ["cube", "cylinder", "sphere", "cone", "disc", "torus", "handy_model"]},
                "size": {"type": "array", "items": {"type": "number", "exclusiveMinimum": 0}, "minItems": 1, "maxItems": 3},
                "model": {"type": "string", "description": "Key of a handy model (shape handy_model)."},
                "name": {"type": "string", "minLength": 1, "maxLength": 200},
                "plate": {"$ref": "#/$defs/PlateRef"},
                "center": {"$ref": "#/$defs/Vec2"}
            },
            "required": ["shape"],
            "additionalProperties": false})JSON", "object_add_primitive");
        tool.handler = object_add_primitive;
        registry.add(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "object_transform";
        tool.title = "Move, rotate, scale, mirror, lay flat";
        tool.description =
            "Transform objects (all their instances), instances, or parts. Steps run in this order: reset, set_scale, set_size, scale, "
            "set_rotation, rotate, mirror, lay_flat, translate, move_to, then drop to the bed. Absolute: set_rotation [x,y,z] deg, "
            "set_scale (factor or [x,y,z]), set_size {size: [x|null, y|null, z|null] mm, uniform}, move_to {center: [x,y], z_min, plate} "
            "(plate frame; `plate` moves to another plate keeping the position on the plate). Relative: translate [dx,dy,dz], rotate "
            "{axis: x|y|z|[vector], angle_deg, about: center|origin}, scale (factor or [x,y,z], in the item's own axes), mirror (axis). "
            "`frame`: \"plate\"/\"world\" (axes of the bed, default) or \"local\" (the item's own axes) for rotate, mirror, translate. "
            "lay_flat puts a face on the bed: {facet: n, volume?} (a mesh facet id), {normal: [x,y,z], frame: world|object} (the "
            "direction that should face down), {face: n} (a face number from mesh_query flat_faces), or {auto: true, rank?} (the "
            "largest flat face of the convex hull; rank 2 = second largest; candidates are returned). Parts are transformed in their object's frame (translate, rotate, scale, mirror, "
            "set_*). drop_to_bed (default true) puts each instance's lowest point at z = 0; move_to.z_min > 0 switches the object's "
            "auto-drop off. One undo step. Returns the changed objects with new boxes and plate assignments.";
        tool.input_schema = Schema::parse(R"JSON({
            "type": "object",
            "$defs": {
                "Axis": {"oneOf": [{"enum": ["x", "y", "z"]}, {"$ref": "#/$defs/Vec3"}]},
                "Factor": {"oneOf": [{"type": "number", "exclusiveMinimum": 0}, {"$ref": "#/$defs/Vec3"}]}
            },
            "properties": {
                "targets": {"type": "array", "items": {"$ref": "#/$defs/Id"}, "minItems": 1},
                "frame": {"enum": ["plate", "world", "local"], "default": "plate"},
                "translate": {"$ref": "#/$defs/Vec3"},
                "move_to": {"type": "object", "properties": {
                    "center": {"$ref": "#/$defs/Vec2"},
                    "z_min": {"type": "number"},
                    "plate": {"$ref": "#/$defs/PlateRef"}}, "additionalProperties": false},
                "rotate": {"type": "object", "properties": {
                    "axis": {"$ref": "#/$defs/Axis"},
                    "angle_deg": {"type": "number"},
                    "about": {"enum": ["center", "origin"], "default": "center"}},
                    "required": ["axis", "angle_deg"], "additionalProperties": false},
                "set_rotation": {"$ref": "#/$defs/Vec3"},
                "scale": {"$ref": "#/$defs/Factor"},
                "set_scale": {"$ref": "#/$defs/Factor"},
                "set_size": {"type": "object", "properties": {
                    "size": {"type": "array", "items": {"oneOf": [{"type": "number"}, {"type": "null"}]}, "minItems": 3, "maxItems": 3},
                    "uniform": {"type": "boolean", "default": true}}, "required": ["size"], "additionalProperties": false},
                "mirror": {"$ref": "#/$defs/Axis"},
                "reset": {"type": "array", "items": {"enum": ["rotation", "scale", "mirror"]}, "minItems": 1},
                "lay_flat": {"oneOf": [
                    {"type": "object", "properties": {"facet": {"type": "integer", "minimum": 0}, "volume": {"$ref": "#/$defs/Id"}}, "required": ["facet"], "additionalProperties": false},
                    {"type": "object", "properties": {"normal": {"$ref": "#/$defs/Vec3"}, "frame": {"enum": ["world", "object"]}}, "required": ["normal"], "additionalProperties": false},
                    {"type": "object", "properties": {"face": {"type": "integer", "minimum": 0}}, "required": ["face"], "additionalProperties": false},
                    {"type": "object", "properties": {"auto": {"const": true}, "rank": {"type": "integer", "minimum": 1, "maximum": 50}}, "required": ["auto"], "additionalProperties": false}]},
                "drop_to_bed": {"type": "boolean", "default": true}
            },
            "required": ["targets"],
            "additionalProperties": false})JSON", "object_transform");
        tool.handler = object_transform;
        registry.add(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "object_duplicate";
        tool.title = "Duplicate objects or instances";
        tool.description =
            "Copy objects or instances `copies` times, as extra instances of the same object (as: \"instances\", default; they share "
            "mesh and settings) or as independent objects (as: \"objects\"). Copies are laid out in a row next to the source, `spacing` "
            "mm apart (default 5), wrapping on the plate; copies that do not fit end up outside the plate (outside_plate: true), so "
            "call object_arrange to pack them. Removing instances is `delete` with instance ids. One undo step. Returns `created` "
            "(instance ids, or new objects).";
        tool.input_schema = Schema::parse(R"JSON({
            "type": "object",
            "properties": {
                "targets": {"type": "array", "items": {"$ref": "#/$defs/Id"}, "minItems": 1},
                "copies": {"type": "integer", "minimum": 1, "maximum": 200, "default": 1},
                "as": {"enum": ["instances", "objects"], "default": "instances"},
                "spacing": {"type": "number", "minimum": 0, "default": 5}
            },
            "required": ["targets"],
            "additionalProperties": false})JSON", "object_duplicate");
        tool.handler = object_duplicate;
        registry.add(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "delete";
        tool.title = "Delete objects, instances or parts";
        tool.description =
            "Delete objects, instances (deleting every instance deletes the object) or parts and modifiers (the last solid part of an "
            "object cannot be deleted: last_part). One undo step. Returns the removed ids and the scene summary.";
        tool.input_schema = Schema::parse(R"JSON({
            "type": "object",
            "properties": {"targets": {"type": "array", "items": {"$ref": "#/$defs/Id"}, "minItems": 1}},
            "required": ["targets"],
            "additionalProperties": false})JSON", "delete");
        tool.destructive = true;
        tool.handler = delete_items;
        registry.add(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "object_split";
        tool.title = "Split to objects or parts";
        tool.description =
            "Split an object made of separate shells into separate objects (into \"objects\"; keep_z: true keeps floating pieces at "
            "their height by switching auto-drop off, as the GUI offers), or a part into several parts of the same object (into "
            "\"parts\"; target the part, or the object when it has one part). not_splittable when the mesh is one connected shell. "
            "One undo step. Returns `created` objects (objects) or the changed object (parts).";
        tool.input_schema = Schema::parse(R"JSON({
            "type": "object",
            "properties": {
                "target": {"$ref": "#/$defs/Id"},
                "into": {"enum": ["objects", "parts"], "default": "objects"},
                "keep_z": {"type": "boolean", "default": false}
            },
            "required": ["target"],
            "additionalProperties": false})JSON", "object_split");
        tool.handler = object_split;
        registry.add(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "object_merge";
        tool.title = "Merge objects into one (assemble)";
        tool.description =
            "Assemble objects or instances into one new multi-part object, keeping where every part is (as the GUI's Assemble). "
            "Per-object settings are merged; per-part filaments are kept. `name` defaults to \"Assembly\". The sources are removed "
            "(only the given instances of an object with more). One undo step. Returns `created` and `removed`.";
        tool.input_schema = Schema::parse(R"JSON({
            "type": "object",
            "properties": {
                "targets": {"type": "array", "items": {"$ref": "#/$defs/Id"}, "minItems": 2},
                "name": {"type": "string", "minLength": 1, "maxLength": 200}
            },
            "required": ["targets"],
            "additionalProperties": false})JSON", "object_merge");
        tool.handler = object_merge;
        registry.add(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "object_cut";
        tool.title = "Cut an object with a plane";
        tool.description =
            "Cut an object (or an instance's object, positioned by that instance) with a plane: {z: height} (plate frame, horizontal) "
            "or {point: [x,y,z], normal: [x,y,z], frame: plate|world|object}; \"upper\" is the side the normal points to. keep: "
            "both|upper|lower; as_parts: keep both halves as two parts of one object; flip_upper / flip_lower turn a half upside down; "
            "place_on_cut_upper / place_on_cut_lower lay a half on its cut face. Connectors (dowels, plugs) are not available here. "
            "One undo step. Returns `created` objects and the `removed` source.";
        tool.input_schema = Schema::parse(R"JSON({
            "type": "object",
            "properties": {
                "target": {"$ref": "#/$defs/Id"},
                "plane": {"oneOf": [
                    {"type": "object", "properties": {"z": {"type": "number"}}, "required": ["z"], "additionalProperties": false},
                    {"type": "object", "properties": {"point": {"$ref": "#/$defs/Vec3"}, "normal": {"$ref": "#/$defs/Vec3"},
                                                      "frame": {"enum": ["plate", "world", "object"]}},
                     "required": ["point", "normal"], "additionalProperties": false}]},
                "keep": {"enum": ["both", "upper", "lower"], "default": "both"},
                "as_parts": {"type": "boolean", "default": false},
                "flip_upper": {"type": "boolean", "default": false},
                "flip_lower": {"type": "boolean", "default": false},
                "place_on_cut_upper": {"type": "boolean", "default": false},
                "place_on_cut_lower": {"type": "boolean", "default": false}
            },
            "required": ["target", "plane"],
            "additionalProperties": false})JSON", "object_cut");
        tool.destructive = true;
        tool.handler = object_cut;
        registry.add(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "object_arrange";
        tool.title = "Arrange objects on plates";
        tool.description =
            "Auto-arrange the objects of one plate (scope \"plate\", default the current plate) or of all plates (scope \"all\"; may "
            "add plates), with the arrange settings of the GUI for this call: spacing (mm, 0 = automatic), allow_rotation, "
            "multi_material_same_plate, avoid_calibration_region, align_to_y. Locked plates are left alone (plate_locked when the "
            "chosen plate is locked). Runs as a job: answers when done or after wait_s with {status: running, job}; poll job_status, "
            "stop with job_cancel. One undo step. The finished job's result lists every instance's plate, centre and rotation.";
        tool.input_schema = Schema::parse(R"JSON({
            "type": "object",
            "properties": {
                "scope": {"enum": ["plate", "all"], "default": "plate"},
                "plate": {"$ref": "#/$defs/PlateRef"},
                "spacing": {"type": "number", "minimum": 0, "maximum": 100},
                "allow_rotation": {"type": "boolean"},
                "multi_material_same_plate": {"type": "boolean"},
                "avoid_calibration_region": {"type": "boolean"},
                "align_to_y": {"type": "boolean"},
                "wait_s": {"type": "number", "minimum": 0, "maximum": 280, "default": 60}
            },
            "additionalProperties": false})JSON", "object_arrange");
        tool.handler = start_arrange;
        registry.add(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "object_orient";
        tool.title = "Auto-orient for printing";
        tool.description =
            "Rotate objects to the orientation the application's auto-orient finds best (goal \"min_support\", default: least support "
            "area, as the GUI; \"min_volume\": least support volume). `targets` (objects or instances) or every instance on `plate` "
            "(default current). Objects on locked plates are refused (plate_locked). Runs as a job on a worker thread: answers when "
            "done or after wait_s with {status: running, job}; job_status / job_cancel. One undo step; the finished result has the "
            "changed objects.";
        tool.input_schema = Schema::parse(R"JSON({
            "type": "object",
            "properties": {
                "targets": {"type": "array", "items": {"$ref": "#/$defs/Id"}, "minItems": 1},
                "plate": {"$ref": "#/$defs/PlateRef"},
                "goal": {"enum": ["min_support", "min_volume"], "default": "min_support"},
                "wait_s": {"type": "number", "minimum": 0, "maximum": 280, "default": 60}
            },
            "additionalProperties": false})JSON", "object_orient");
        tool.handler = start_orient;
        registry.add(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "mesh_edit";
        tool.title = "Simplify, repair, convert units, scale to fit";
        tool.description =
            "Change meshes. op \"simplify\": decimate every volume of the targets (or the given parts) to target_ratio of its facets "
            "(default 0.5), or target_facets, or until max_error_mm; \"repair\": close holes, fix orientation, remove "
            "self-intersections of solid parts (CGAL, available on every platform); both run as a job on a worker thread (wait_s, "
            "job_status, job_cancel) and give the volumes new ids (returned). Paint is remapped when the app's keep_painting "
            "preference is on, else removed (reported). \"convert_units\" scales meshes: from inch|cm|m, or to_inch|to_m to undo. "
            "\"scale_to_fit\" scales instances uniformly to fit the plate (minus `margin` mm) and centres them on their plate. "
            "One undo step.";
        tool.input_schema = Schema::parse(R"JSON({
            "type": "object",
            "properties": {
                "op": {"enum": ["simplify", "repair", "convert_units", "scale_to_fit"]},
                "targets": {"type": "array", "items": {"$ref": "#/$defs/Id"}, "minItems": 1},
                "target_ratio": {"type": "number", "exclusiveMinimum": 0, "maximum": 1},
                "target_facets": {"type": "integer", "minimum": 4},
                "max_error_mm": {"type": "number", "exclusiveMinimum": 0},
                "from": {"enum": ["inch", "cm", "m", "to_inch", "to_m"]},
                "margin": {"type": "number", "minimum": 0, "default": 0},
                "wait_s": {"type": "number", "minimum": 0, "maximum": 280, "default": 100}
            },
            "required": ["op", "targets"],
            "additionalProperties": false})JSON", "mesh_edit");
        tool.destructive = true;
        tool.handler = mesh_edit;
        registry.add(std::move(tool));
    }
}

}}} // namespace Slic3r::GUI::Mcp
