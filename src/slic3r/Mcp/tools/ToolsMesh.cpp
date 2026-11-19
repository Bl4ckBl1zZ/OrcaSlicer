// Mesh tools: mesh_query (stats, overhangs, flat_faces, raycast, distance). Read-only.

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"

#include "../McpCamera.hpp"
#include "../McpGeometry.hpp"
#include "../McpIds.hpp"
#include "../McpScene.hpp"
#include "../McpSchema.hpp"
#include "../McpTool.hpp"

namespace Slic3r { namespace GUI { namespace Mcp {

namespace {

// What a target id stands for: one instance of an object and the volumes to look at.
struct MeshTarget
{
    std::string               id;
    ModelObject*              object { nullptr };
    int                       obj_idx { -1 };
    int                       inst_idx { 0 };
    std::vector<ModelVolume*> volumes;
    Vec3d                     plate_origin { Vec3d::Zero() };
};

Vec3d origin_of_plate(int plate_idx)
{
    PartPlateList& plates = wxGetApp().plater()->get_partplate_list();
    if (plate_idx < 0 || plate_idx >= plates.get_plate_count())
        return Vec3d::Zero();
    return plates.get_plate(plate_idx)->get_origin();
}

MeshTarget make_target(const ItemRef& ref, const std::string& id)
{
    MeshTarget target;
    target.id = id;
    target.object = ref.object;
    target.obj_idx = ref.obj_idx;
    target.inst_idx = ref.kind == IdKind::Instance ? ref.inst_idx : 0;
    if (ref.kind == IdKind::Volume)
        target.volumes.push_back(ref.volume);
    else
        for (ModelVolume* volume : ref.object->volumes)
            if (volume->is_model_part())
                target.volumes.push_back(volume);
    if (ref.object->instances.empty())
        fail("invalid_argument", id + " has no instance.");
    target.plate_origin = origin_of_plate(Ids::plate_of_instance(target.obj_idx, target.inst_idx));
    return target;
}

std::vector<MeshTarget> targets_from(ToolContext& ctx)
{
    if (!ctx.has("targets"))
        fail("invalid_argument", "This kind of query needs `targets`: object, instance or volume ids.");
    std::vector<MeshTarget> out;
    const json&             ids = ctx.args()["targets"];
    const std::vector<ItemRef> refs = Ids::items(ids, "targets");
    for (size_t i = 0; i < refs.size(); ++i)
        out.push_back(make_target(refs[i], ids[i].get<std::string>()));
    return out;
}

Transform3d world_of(const MeshTarget& target, const ModelVolume& volume)
{
    return target.object->instances[static_cast<size_t>(target.inst_idx)]->get_matrix() * volume.get_matrix();
}

json box_json(const BoundingBoxf3& box, const Vec3d& origin)
{
    json out = json::object();
    out["min"] = Scene::vec3(box.min.x() - origin.x(), box.min.y() - origin.y(), box.min.z() - origin.z());
    out["max"] = Scene::vec3(box.max.x() - origin.x(), box.max.y() - origin.y(), box.max.z() - origin.z());
    return out;
}

Vec3d read_vec3(const json& value)
{
    return Vec3d(value[0].get<double>(), value[1].get<double>(), value[2].get<double>());
}

json query_stats(ToolContext& ctx)
{
    json list = json::array();
    for (const MeshTarget& target : targets_from(ctx)) {
        size_t        facets = 0;
        double        volume_mm3 = 0., area = 0.;
        int           open_edges = 0, parts = 0;
        BoundingBoxf3 box;
        for (const ModelVolume* volume : target.volumes) {
            const Transform3d        world = world_of(target, *volume);
            const TriangleMeshStats& stats = volume->mesh().stats();
            facets += stats.number_of_facets;
            volume_mm3 += static_cast<double>(stats.volume) * std::abs(world.linear().determinant());
            area += Geometry::surface_area(volume->mesh().its, world);
            open_edges += stats.open_edges;
            parts += stats.number_of_parts;
            box.merge(volume->mesh().transformed_bounding_box(world));
        }
        json item = json::object();
        item["id"] = target.id;
        item["facets"] = facets;
        item["volume_mm3"] = Scene::round4(volume_mm3);
        item["area_mm2"] = Scene::round4(area);
        item["manifold"] = open_edges == 0;
        item["open_edges"] = open_edges;
        item["shells"] = parts;
        if (box.defined) {
            const Vec3d size = box.size();
            item["size"] = Scene::vec3(size.x(), size.y(), size.z());
            item["bbox"] = box_json(box, target.plate_origin);
        }
        list.push_back(std::move(item));
    }
    json out = json::object();
    out["stats"] = std::move(list);
    return out;
}

json query_overhangs(ToolContext& ctx)
{
    const double threshold = ctx.arg<double>("threshold_deg", 45.);
    const size_t limit = ctx.arg<size_t>("limit", 10);
    json         list = json::array();
    for (const MeshTarget& target : targets_from(ctx)) {
        // The object rests on its lowest point; what touches the bed needs no support.
        const double bed_z = target.object->instance_bounding_box(static_cast<size_t>(target.inst_idx)).min.z();
        double       area = 0., total_area = 0., lowest = 0.;
        size_t       facets = 0;
        bool         any = false;
        struct Entry
        {
            Geometry::OverhangCluster cluster;
            std::string               volume;
        };
        std::vector<Entry> clusters;
        for (const ModelVolume* volume : target.volumes) {
            const Transform3d              world = world_of(target, *volume);
            const Geometry::OverhangReport report = Geometry::overhangs(volume->mesh().its, world, threshold, bed_z);
            total_area += Geometry::surface_area(volume->mesh().its, world);
            area += report.area;
            facets += report.facets;
            if (report.facets > 0) {
                lowest = any ? std::min(lowest, report.lowest_z) : report.lowest_z;
                any = true;
            }
            for (const Geometry::OverhangCluster& cluster : report.clusters)
                clusters.push_back(Entry { cluster, Ids::of(*volume) });
        }
        std::sort(clusters.begin(), clusters.end(), [](const Entry& a, const Entry& b) { return a.cluster.area > b.cluster.area; });

        json item = json::object();
        item["id"] = target.id;
        item["threshold_deg"] = threshold;
        item["area_mm2"] = Scene::round4(area);
        item["facets"] = facets;
        item["share_of_surface"] = total_area > 0. ? Scene::round4(area / total_area) : 0.;
        if (any)
            item["lowest_z"] = Scene::round4(lowest - target.plate_origin.z());
        json cluster_list = json::array();
        for (size_t i = 0; i < clusters.size() && i < limit; ++i) {
            json entry = json::object();
            entry["volume"] = clusters[i].volume;
            entry["area_mm2"] = Scene::round4(clusters[i].cluster.area);
            entry["facets"] = clusters[i].cluster.facets;
            entry["box"] = box_json(clusters[i].cluster.box, target.plate_origin);
            cluster_list.push_back(std::move(entry));
        }
        item["clusters"] = std::move(cluster_list);
        item["cluster_count"] = clusters.size();
        list.push_back(std::move(item));
    }
    json out = json::object();
    out["overhangs"] = std::move(list);
    return out;
}

json query_flat_faces(ToolContext& ctx)
{
    json list = json::array();
    for (const MeshTarget& target : targets_from(ctx)) {
        // The whole object as this instance stands in the world (position does not matter here).
        TriangleMesh mesh = target.object->raw_mesh();
        mesh.transform(target.object->instances[static_cast<size_t>(target.inst_idx)]->get_matrix());
        json faces = json::array();
        const std::vector<Geometry::FlatFace> found = Geometry::flat_faces(mesh);
        for (size_t i = 0; i < found.size(); ++i) {
            json face = json::object();
            face["face"] = i;
            face["normal"] = Scene::vec3(found[i].normal.x(), found[i].normal.y(), found[i].normal.z());
            face["area_mm2"] = Scene::round4(found[i].area);
            if (found[i].normal.z() < -0.9998)
                face["on_bed"] = true; // this face points down already
            faces.push_back(std::move(face));
        }
        json item = json::object();
        item["id"] = target.id;
        item["faces"] = std::move(faces);
        list.push_back(std::move(item));
    }
    json out = json::object();
    out["flat_faces"] = std::move(list);
    return out;
}

json query_raycast(ToolContext& ctx)
{
    const json& args = ctx.args();
    Vec3d       origin, direction;
    Vec3d       frame_origin = Vec3d::Zero(); // plate origin the result is expressed against
    if (ctx.has("pixel")) {
        if (!ctx.has("camera"))
            fail("invalid_argument", "`pixel` needs `camera`: the camera object a screenshot returned.");
        CameraView camera;
        if (!CameraView::from_json(args["camera"], camera))
            fail("invalid_argument", "`camera` is not a camera object as screenshot returns it.",
                 "Pass the \"camera\" member of a screenshot result unchanged.");
        const double u = args["pixel"][0].get<double>(), v = args["pixel"][1].get<double>();
        if (u < 0 || v < 0 || u > camera.width || v > camera.height)
            fail("invalid_argument", "`pixel` is outside the picture (" + std::to_string(camera.width) + " x " + std::to_string(camera.height) + ").");
        camera.ray(Vec2d(u, v), origin, direction);
        frame_origin = origin_of_plate(Ids::plate(args.contains("plate") ? args["plate"] : json(nullptr)));
    } else if (ctx.has("origin") && ctx.has("direction")) {
        frame_origin = origin_of_plate(Ids::plate(args.contains("plate") ? args["plate"] : json(nullptr)));
        origin = frame_origin + read_vec3(args["origin"]);
        direction = read_vec3(args["direction"]);
        if (direction.norm() < 1e-9)
            fail("invalid_argument", "`direction` must not be the zero vector.");
        direction.normalize();
    } else
        fail("invalid_argument", "A raycast needs either `pixel` with `camera`, or `origin` with `direction`.");

    // Candidates: the targets, or every object.
    struct Candidate
    {
        ModelObject* object;
        int          obj_idx;
        int          inst_idx;
        ModelVolume* volume;
    };
    std::vector<Candidate> candidates;
    Model&                 model = wxGetApp().plater()->model();
    if (ctx.has("targets")) {
        for (const ItemRef& ref : Ids::items(args["targets"], "targets")) {
            for (int i = 0; i < static_cast<int>(ref.object->instances.size()); ++i) {
                if (ref.kind == IdKind::Instance && i != ref.inst_idx)
                    continue;
                for (ModelVolume* volume : ref.object->volumes)
                    if (ref.kind == IdKind::Volume ? volume == ref.volume : volume->is_model_part())
                        candidates.push_back(Candidate { ref.object, ref.obj_idx, i, volume });
            }
        }
    } else {
        for (int o = 0; o < static_cast<int>(model.objects.size()); ++o)
            for (int i = 0; i < static_cast<int>(model.objects[static_cast<size_t>(o)]->instances.size()); ++i)
                for (ModelVolume* volume : model.objects[static_cast<size_t>(o)]->volumes)
                    if (volume->is_model_part())
                        candidates.push_back(Candidate { model.objects[static_cast<size_t>(o)], o, i, volume });
    }

    Geometry::RayHit best;
    const Candidate* best_candidate = nullptr;
    for (const Candidate& candidate : candidates) {
        const Transform3d world = candidate.object->instances[static_cast<size_t>(candidate.inst_idx)]->get_matrix() * candidate.volume->get_matrix();
        const Geometry::RayHit hit = Geometry::ray_mesh(candidate.volume->mesh().its, world, origin, direction);
        if (hit.hit && (!best.hit || hit.distance < best.distance)) {
            best = hit;
            best_candidate = &candidate;
        }
    }

    json out = json::object();
    json ray = json::object();
    ray["origin"] = Scene::vec3(origin.x() - frame_origin.x(), origin.y() - frame_origin.y(), origin.z() - frame_origin.z());
    ray["direction"] = Scene::vec3(direction.x(), direction.y(), direction.z());
    out["ray"] = std::move(ray);
    if (!best.hit || best_candidate == nullptr) {
        out["hit"] = nullptr;
        return out;
    }
    const ModelInstance& instance = *best_candidate->object->instances[static_cast<size_t>(best_candidate->inst_idx)];
    const Vec3d          plate_origin = origin_of_plate(Ids::plate_of_instance(best_candidate->obj_idx, best_candidate->inst_idx));
    const Vec3d          in_object = instance.get_matrix().inverse() * best.point;
    json hit = json::object();
    hit["object"] = Ids::of(*best_candidate->object);
    hit["instance"] = Ids::of(instance);
    hit["volume"] = Ids::of(*best_candidate->volume);
    hit["facet"] = best.facet;
    hit["point"] = Scene::vec3(best.point.x() - plate_origin.x(), best.point.y() - plate_origin.y(), best.point.z() - plate_origin.z());
    hit["point_object"] = Scene::vec3(in_object.x(), in_object.y(), in_object.z());
    hit["normal"] = Scene::vec3(best.normal.x(), best.normal.y(), best.normal.z());
    hit["distance"] = Scene::round4(best.distance);
    out["hit"] = std::move(hit);
    return out;
}

json query_distance(ToolContext& ctx)
{
    if (!ctx.has("a") || !ctx.has("b"))
        fail("invalid_argument", "A distance query needs the points `a` and `b`.");
    const Vec3d a = read_vec3(ctx.args()["a"]);
    const Vec3d b = read_vec3(ctx.args()["b"]);
    const Vec3d delta = b - a;
    json out = json::object();
    out["distance"] = Scene::round4(delta.norm());
    out["delta"] = Scene::vec3(delta.x(), delta.y(), delta.z());
    out["horizontal"] = Scene::round4(std::hypot(delta.x(), delta.y()));
    return out;
}

json mesh_query(ToolContext& ctx)
{
    const std::string kind = ctx.arg("kind", "stats");
    if (kind == "stats") return query_stats(ctx);
    if (kind == "overhangs") return query_overhangs(ctx);
    if (kind == "flat_faces") return query_flat_faces(ctx);
    if (kind == "raycast") return query_raycast(ctx);
    return query_distance(ctx);
}

} // namespace

MCP_TOOL_GROUP(Mesh)
{
    ToolDef tool;
    tool.name = "mesh_query";
    tool.title = "Measure and inspect geometry";
    tool.description =
        "Measure and inspect model geometry without changing it. kind \"stats\": facets, volume, surface area, whether the mesh is closed "
        "(manifold), size and bounding box of objects or parts. \"overhangs\": the area that faces downward within threshold_deg of "
        "straight down (the support painting gizmo's test), with the largest connected regions and their boxes - use it to judge "
        "whether and where supports are needed before slicing. \"flat_faces\": the faces an object could be laid on (planes of its "
        "convex hull, largest first) with their normals, for choosing an orientation. \"raycast\": what a ray hits - give a `pixel` of a "
        "screenshot together with that screenshot's `camera`, or an `origin` and `direction` - returning the object, part, facet, "
        "point and surface normal; this turns \"the thing I see at pixel (520, 300)\" into a point on the model. \"distance\": between two "
        "points. Positions and boxes are in plate coordinates (mm).";
    tool.input_schema = Schema::parse(R"JSON({
        "type": "object",
        "properties": {
            "kind": {"enum": ["stats", "overhangs", "flat_faces", "raycast", "distance"]},
            "targets": {"type": "array", "items": {"$ref": "#/$defs/Id"}, "minItems": 1,
                        "description": "Object, instance or volume ids. Required except for raycast (default: every object) and distance."},
            "threshold_deg": {"type": "number", "minimum": 0, "maximum": 90, "default": 45,
                              "description": "overhangs: a facet counts when its normal is within this angle of straight down."},
            "limit": {"type": "integer", "minimum": 1, "maximum": 100, "default": 10, "description": "overhangs: how many regions to list."},
            "pixel": {"$ref": "#/$defs/Vec2", "description": "raycast: [u, v] in a screenshot, from its top-left corner."},
            "camera": {"type": "object", "description": "raycast: the \"camera\" object of the screenshot the pixel is from, unchanged."},
            "origin": {"$ref": "#/$defs/Vec3", "description": "raycast: start of the ray, plate coordinates."},
            "direction": {"$ref": "#/$defs/Vec3", "description": "raycast: direction of the ray."},
            "plate": {"$ref": "#/$defs/PlateRef", "description": "raycast: the plate whose coordinates origin and the reported ray use (default: current)."},
            "a": {"$ref": "#/$defs/Vec3"},
            "b": {"$ref": "#/$defs/Vec3"}
        },
        "required": ["kind"],
        "additionalProperties": false})JSON", "mesh_query");
    tool.read_only = true;
    tool.idempotent = true;
    tool.handler = mesh_query;
    registry.add(std::move(tool));
}

}}} // namespace Slic3r::GUI::Mcp
