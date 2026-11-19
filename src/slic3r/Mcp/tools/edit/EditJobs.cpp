#include "EditJobs.hpp"

#include <algorithm>
#include <atomic>
#include <limits>
#include <optional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/MeshRepair.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Orient.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/QuadricEdgeCollapse.hpp"
#include "libslic3r/Thread.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/Jobs/Job.hpp"
#include "slic3r/GUI/Jobs/Worker.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"

#include "../../McpIds.hpp"
#include "../../McpJobs.hpp"
#include "../../McpRefresh.hpp"
#include "../../McpScene.hpp"
#include "EditCommon.hpp"

namespace Slic3r { namespace GUI { namespace Mcp { namespace Edit {

namespace {

// Where every instance of the given plates is now: {object, instance, plate, center, rotation}.
json placements(const std::set<int>& only_plates)
{
    json         out = json::array();
    const Model& m = model();
    for (int o = 0; o < static_cast<int>(m.objects.size()); ++o) {
        for (int i = 0; i < static_cast<int>(m.objects[static_cast<size_t>(o)]->instances.size()); ++i) {
            const int plate = Ids::plate_of_instance(o, i);
            if (!only_plates.empty() && only_plates.count(plate) == 0)
                continue;
            json inst = Scene::instance_json(o, i, Scene::Detail::Normal);
            json item = json::object();
            item["object"] = Ids::of(*m.objects[static_cast<size_t>(o)]);
            item["name"] = m.objects[static_cast<size_t>(o)]->name;
            item["instance"] = inst["id"];
            item["plate"] = inst["plate"];
            item["center"] = inst["center"];
            item["rotation"] = inst["rotation"];
            item["outside_plate"] = inst["outside_plate"];
            out.push_back(std::move(item));
            if (out.size() >= 200)
                return out;
        }
    }
    return out;
}

void find_instance(size_t object_id, size_t instance_id, ModelObject*& object, ModelInstance*& instance, int& obj_idx)
{
    object = nullptr;
    instance = nullptr;
    obj_idx = -1;
    Model& m = model();
    for (size_t o = 0; o < m.objects.size(); ++o) {
        if (m.objects[o]->id().id != object_id)
            continue;
        for (ModelInstance* inst : m.objects[o]->instances)
            if (inst->id().id == instance_id) {
                object = m.objects[o];
                instance = inst;
                obj_idx = static_cast<int>(o);
            }
    }
}

} // namespace

// ---------------------------------------------------------------------------------------------
// arrange
// ---------------------------------------------------------------------------------------------

json start_arrange(ToolContext& ctx)
{
    Plater&           p = plater();
    PartPlateList&    list = plates();
    const std::string scope = ctx.arg("scope", "plate");
    const int         plate_idx = Ids::plate(ctx.has("plate") ? ctx.args()["plate"] : json(), "plate");
    if (scope == "plate") {
        if (list.get_plate(plate_idx)->is_locked())
            fail("plate_locked", "Plate " + std::to_string(plate_idx + 1) + " is locked; arrange leaves locked plates alone.",
                 "Unlock it with plate_manage action \"unlock\" first.", json::object({{"plate", plate_idx + 1}}));
        if (list.get_plate(plate_idx)->empty())
            fail("nothing_to_arrange", "Plate " + std::to_string(plate_idx + 1) + " has no objects.", "Choose a plate with objects, or scope \"all\".");
    } else if (model().objects.empty())
        fail("nothing_to_arrange", "The project has no objects.", "Import or add objects first.");
    if (!p.get_ui_job_worker().is_idle())
        fail("ui_busy", "Another arrange or orient job is running.", "Wait for it (job_status) and retry.");

    GLCanvas3D::ArrangeSettings& settings = p.canvas3D()->get_arrange_settings();
    const GLCanvas3D::ArrangeSettings saved = settings;
    if (ctx.has("spacing"))
        settings.distance = static_cast<float>(ctx.arg<double>("spacing", 0.));
    if (ctx.has("allow_rotation"))
        settings.enable_rotation = ctx.arg<bool>("allow_rotation", false);
    if (ctx.has("multi_material_same_plate"))
        settings.allow_multi_materials_on_same_plate = ctx.arg<bool>("multi_material_same_plate", true);
    if (ctx.has("avoid_calibration_region"))
        settings.avoid_extrusion_cali_region = ctx.arg<bool>("avoid_calibration_region", true);
    if (ctx.has("align_to_y"))
        settings.align_to_y_axis = ctx.arg<bool>("align_to_y", false);
    else if (ctx.has("allow_rotation") && !ctx.arg<bool>("allow_rotation", false))
        settings.align_to_y_axis = false; // "no rotation" means no turn at all, also not the align-to-Y turn (on for BBL printers)
    const json settings_used = {{"spacing", settings.distance},
                                {"allow_rotation", settings.enable_rotation},
                                {"multi_material_same_plate", settings.allow_multi_materials_on_same_plate},
                                {"avoid_calibration_region", settings.avoid_extrusion_cali_region},
                                {"align_to_y", settings.align_to_y_axis}};

    const int prev_state = p.get_prepare_state();
    const int prev_plate = list.get_curr_plate_index();
    ctx.snapshot(scope == "plate" ? "arrange plate " + std::to_string(plate_idx + 1) : std::string("arrange all plates"));
    if (scope == "plate" && plate_idx != prev_plate)
        p.select_plate(plate_idx);
    p.set_prepare_state(scope == "plate" ? ::Slic3r::GUI::Job::JobPrepareState::PREPARE_STATE_MENU : ::Slic3r::GUI::Job::JobPrepareState::PREPARE_STATE_DEFAULT);
    p.arrange();
    ctx.set_slice_invalidated();

    auto cancelled = std::make_shared<bool>(false);
    auto job = Jobs::instance().start("arrange", [cancelled]() {
        *cancelled = true;
        plater().get_ui_job_worker().cancel();
    });
    Jobs::instance().update(job, 0., "Arranging");
    watch([job, cancelled, saved, settings_used, prev_state, prev_plate, plate_idx, scope]() {
        if (job->state != Job::State::Running)
            return true;
        Plater& pl = plater();
        if (!pl.get_ui_job_worker().is_idle())
            return false;
        pl.canvas3D()->get_arrange_settings() = saved;
        pl.set_prepare_state(prev_state);
        if (scope == "plate" && prev_plate != plate_idx && prev_plate < plates().get_plate_count()) {
            Plater::SuppressSnapshots suppress(&pl);
            pl.select_plate(prev_plate);
        }
        std::set<int> only;
        if (scope == "plate")
            only.insert(plate_idx);
        json result = json::object();
        result["scope"] = scope;
        if (scope == "plate")
            result["plate"] = plate_idx + 1;
        result["plates"] = plates().get_plate_count();
        result["settings_used"] = settings_used;
        result["placements"] = placements(only);
        Jobs::instance().finish(job, *cancelled ? Job::State::Cancelled : Job::State::Finished, result);
        return true;
    });
    ctx.defer(wait_for_job(job, ctx.wait_s(60., 280.)));
    return nullptr;
}

// ---------------------------------------------------------------------------------------------
// orient
// ---------------------------------------------------------------------------------------------

namespace {

void minimal_support_area_params(orientation::OrientParams& out)
{
    // Copied from OrientJob.cpp (setMinimalSupportAreaPrams), the GUI's default ("min_area").
    out.TAR_A = 0.015f; out.TAR_B = 0.177f; out.RELATIVE_F = 20; out.CONTOUR_F = 0.5f; out.BOTTOM_F = 2.5f;
    out.BOTTOM_HULL_F = 0.1f; out.TAR_C = 0.1f; out.TAR_D = 1; out.TAR_E = 0.0115f; out.FIRST_LAY_H = 0.2f;
    out.VECTOR_TOL = -0.00083f; out.NEGL_FACE_SIZE = 0.01f; out.ASCENT = -0.5f; out.PLAFOND_ADV = 0.0599f;
    out.CONTOUR_AMOUNT = 0.0182427f; out.OV_H = 2.574f; out.height_offset = 2.3728f; out.height_log = 0.041375f;
    out.height_log_k = 1.9325457f; out.LAF_MAX = 0.999f; out.LAF_MIN = 0.97f; out.TAR_LAF = 0.001f; out.TAR_PROJ_AREA = 0.1f;
    out.BOTTOM_MIN = 0.1f; out.BOTTOM_MAX = 2000; out.height_to_bottom_hull_ratio_MIN = 1; out.BOTTOM_HULL_MAX = 2000;
    out.APPERANCE_FACE_SUPP = 3; out.overhang_angle = 60.f; out.use_low_angle_face = true; out.min_volume = false;
    out.parallel = true;
}

struct OrientState
{
    std::vector<orientation::OrientMesh> meshes;
    std::vector<std::pair<size_t, size_t>> ids;        // (object id, instance id)
    std::vector<Transform3d>              matrices;   // instance matrix at start, to detect changes
    std::atomic<bool>                     cancel { false };
    std::atomic<bool>                     done { false };
    std::atomic<int>                      percent { 0 };
    std::string                           error;
    std::thread                           thread;
};

} // namespace

json start_orient(ToolContext& ctx)
{
    PartPlateList& list = plates();
    Model&         m = model();
    // (obj_idx, inst_idx) to orient.
    std::vector<std::pair<int, int>> targets;
    if (ctx.has("targets")) {
        std::set<std::pair<int, int>> seen;
        for (const ItemRef& ref : Ids::items(ctx.args()["targets"], "targets")) {
            if (ref.kind == IdKind::Volume)
                fail("invalid_argument", "`targets`: " + Ids::of(*ref.volume) + " is a part; orient works on objects and instances.", "Pass the object id.");
            if (ref.kind == IdKind::Instance)
                seen.insert({ref.obj_idx, ref.inst_idx});
            else
                for (int i = 0; i < static_cast<int>(ref.object->instances.size()); ++i)
                    seen.insert({ref.obj_idx, i});
        }
        targets.assign(seen.begin(), seen.end());
    } else {
        const int plate_idx = Ids::plate(ctx.has("plate") ? ctx.args()["plate"] : json(), "plate");
        for (int o = 0; o < static_cast<int>(m.objects.size()); ++o)
            for (int i = 0; i < static_cast<int>(m.objects[static_cast<size_t>(o)]->instances.size()); ++i)
                if (Ids::plate_of_instance(o, i) == plate_idx)
                    targets.push_back({o, i});
        if (targets.empty())
            fail("nothing_to_arrange", "Plate " + std::to_string(plate_idx + 1) + " has no objects to orient.", "Pass `targets` or another plate.");
    }
    for (const auto& [o, i] : targets) {
        const int plate = Ids::plate_of_instance(o, i);
        if (plate >= 0 && list.get_plate(plate)->is_locked())
            fail("plate_locked", Ids::of(*m.objects[static_cast<size_t>(o)]->instances[static_cast<size_t>(i)]) + " is on locked plate " +
                 std::to_string(plate + 1) + ".", "Unlock the plate with plate_manage, or leave that object out.", json::object({{"plate", plate + 1}}));
    }
    if (!plater().get_ui_job_worker().is_idle())
        fail("ui_busy", "An arrange or orient job is running.", "Wait for it (job_status) and retry.");

    auto state = std::make_shared<OrientState>();
    const DynamicPrintConfig& config = wxGetApp().preset_bundle->full_config();
    const bool min_area = ctx.arg("goal", "min_support") == "min_support";
    for (const auto& [o, i] : targets) {
        ModelObject*   object = m.objects[static_cast<size_t>(o)];
        ModelInstance* instance = object->instances[static_cast<size_t>(i)];
        orientation::OrientMesh om;
        om.name = object->name;
        // The mesh as the instance shows it (rotation, scale, mirror; no translation), so the result
        // is the rotation to apply on top of the instance's current one.
        om.mesh = object->raw_mesh();
        Transform3d linear = instance->get_matrix();
        linear.translation() = Vec3d::Zero();
        om.mesh.transform(linear);
        om.overhang_angle = object->config.has("support_threshold_angle") ? object->config.opt_int("support_threshold_angle") :
                                                                             config.opt_int("support_threshold_angle");
        if (om.overhang_angle <= 0)
            om.overhang_angle = 30;
        state->meshes.push_back(std::move(om));
        state->ids.push_back({object->id().id, instance->id().id});
        state->matrices.push_back(instance->get_matrix());
    }

    std::vector<std::string> names;
    for (const auto& [o, i] : targets)
        names.push_back(Ids::of(*m.objects[static_cast<size_t>(o)]->instances[static_cast<size_t>(i)]));
    ctx.snapshot("orient " + ids_summary(names));
    ctx.set_slice_invalidated();

    auto job = Jobs::instance().start("orient", [state]() { state->cancel = true; });
    Jobs::instance().update(job, 0., "Orienting");
    state->thread = std::thread([state, min_area]() {
        set_current_thread_name("mcp_orient");
        try {
            orientation::OrientParams params;
            if (min_area)
                minimal_support_area_params(params);
            else
                params.min_volume = true;
            params.stopcondition = [state]() { return state->cancel.load(); };
            const unsigned count = static_cast<unsigned>(state->meshes.size());
            params.progressind = [state, count](unsigned step, std::string) {
                state->percent = count ? static_cast<int>(100. * step / count) : 0;
            };
            orientation::orient(state->meshes, {}, params);
        } catch (const std::exception& ex) {
            state->error = ex.what();
        }
        state->done = true;
    });

    watch([state, job]() {
        if (!state->done) {
            Jobs::instance().update(job, state->percent, "Orienting");
            return false;
        }
        if (state->thread.joinable())
            state->thread.join();
        if (state->cancel) {
            Jobs::instance().finish(job, Job::State::Cancelled, json::object({{"changed", json::array()}}));
            return true;
        }
        if (!state->error.empty()) {
            Jobs::instance().finish(job, Job::State::Failed,
                                    json::object({{"error", json::object({{"code", "internal"}, {"message", "Orienting failed: " + state->error}})}}));
            return true;
        }
        // Commit: every target must still exist with the transformation it had.
        std::vector<ModelInstance*> instances;
        std::set<int>               obj_idxs;
        for (size_t k = 0; k < state->ids.size(); ++k) {
            ModelObject*   object = nullptr;
            ModelInstance* instance = nullptr;
            int            obj_idx = -1;
            find_instance(state->ids[k].first, state->ids[k].second, object, instance, obj_idx);
            if (instance == nullptr || !instance->get_matrix().isApprox(state->matrices[k])) {
                Jobs::instance().finish(job, Job::State::Failed,
                                        json::object({{"error", json::object({{"code", "conflict"},
                                                                              {"message", "An object changed or was removed while it was being oriented; nothing was applied."},
                                                                              {"hint", "Run object_orient again."}})}}));
                return true;
            }
            instances.push_back(instance);
            obj_idxs.insert(obj_idx);
        }
        for (size_t k = 0; k < instances.size(); ++k) {
            const orientation::OrientMesh& om = state->meshes[k];
            instances[k]->rotate(om.rotation_matrix);
            instances[k]->rotate(om.rotation_matrix_vertical);
            instances[k]->get_object()->invalidate_bounding_box();
            instances[k]->get_object()->ensure_on_bed();
        }
        std::vector<int> idxs(obj_idxs.begin(), obj_idxs.end());
        Refresh::objects(idxs);
        json result = json::object();
        result["changed"] = objects_json(idxs);
        Jobs::instance().finish(job, Job::State::Finished, result);
        return true;
    });
    ctx.defer(wait_for_job(job, ctx.wait_s(60., 280.)));
    return nullptr;
}

// ---------------------------------------------------------------------------------------------
// simplify / repair
// ---------------------------------------------------------------------------------------------

namespace {

struct MeshTask
{
    size_t                              object_id { 0 };
    size_t                              volume_id { 0 };
    std::string                         label;
    std::shared_ptr<const TriangleMesh> source;
    TriangleMesh                        result;
    size_t                              facets_before { 0 };
    bool                                modified { false };
    MeshRepairStats                     stats;
    std::string                         error;
};

struct MeshState
{
    std::string           op;
    std::vector<MeshTask> tasks;
    // simplify parameters
    double                ratio { 0.5 };
    double                max_error { -1. };
    size_t                target_facets { 0 };
    std::atomic<bool>     cancel { false };
    std::atomic<bool>     done { false };
    std::atomic<int>      percent { 0 };
    std::thread           thread;
};

struct Cancelled {};

} // namespace

json start_mesh_job(ToolContext& ctx, const std::string& op, const std::vector<std::pair<int, int>>& volumes)
{
    Model& m = model();
    auto   state = std::make_shared<MeshState>();
    state->op = op;
    if (op == "simplify") {
        state->ratio = ctx.arg<double>("target_ratio", -1.);
        state->max_error = ctx.arg<double>("max_error_mm", -1.);
        state->target_facets = ctx.arg<size_t>("target_facets", 0);
        if (state->ratio < 0. && state->max_error < 0. && state->target_facets == 0)
            state->ratio = 0.5;
    }
    std::vector<std::string> names;
    for (const auto& [o, v] : volumes) {
        ModelObject* object = m.objects[static_cast<size_t>(o)];
        ModelVolume* volume = object->volumes[static_cast<size_t>(v)];
        MeshTask     task;
        task.object_id = object->id().id;
        task.volume_id = volume->id().id;
        task.label = Ids::of(*volume);
        task.source = volume->get_mesh_shared_ptr();
        task.facets_before = volume->mesh().facets_count();
        state->tasks.push_back(std::move(task));
        names.push_back(Ids::of(*volume));
    }
    ctx.snapshot(op + " " + ids_summary(names));
    ctx.set_slice_invalidated();

    auto job = Jobs::instance().start(op, [state]() { state->cancel = true; });
    Jobs::instance().update(job, 0., op == "simplify" ? "Simplifying" : "Repairing");
    state->thread = std::thread([state]() {
        set_current_thread_name("mcp_mesh");
        const size_t count = state->tasks.size();
        for (size_t k = 0; k < count && !state->cancel; ++k) {
            MeshTask& task = state->tasks[k];
            auto      report = [state, k, count](int pct) {
                if (state->cancel)
                    throw Cancelled();
                state->percent = static_cast<int>((100. * k + std::clamp(pct, 0, 100)) / count);
            };
            try {
                if (state->op == "simplify") {
                    indexed_triangle_set its = task.source->its;
                    uint32_t             wanted = 0;
                    if (state->target_facets > 0)
                        wanted = static_cast<uint32_t>(state->target_facets);
                    else if (state->ratio >= 0.)
                        wanted = static_cast<uint32_t>(std::max<double>(4., state->ratio * its.indices.size()));
                    float  max_error = state->max_error >= 0. ? static_cast<float>(state->max_error) : std::numeric_limits<float>::max();
                    its_quadric_edge_collapse(its, wanted, &max_error, [state]() { if (state->cancel) throw Cancelled(); },
                                              [&report](int pct) { report(pct); });
                    task.modified = its.indices.size() != task.source->its.indices.size();
                    task.result = TriangleMesh(std::move(its));
                } else {
                    TriangleMesh mesh = *task.source;
                    std::string  error;
                    if (!repair_mesh(mesh, &task.stats, &error, report))
                        task.error = error.empty() ? std::string("no solid could be built") : error;
                    task.modified = task.stats.modified;
                    if (task.modified)
                        task.result = std::move(mesh);
                }
            } catch (const Cancelled&) {
                state->cancel = true;
            } catch (const std::exception& ex) {
                task.error = ex.what();
            }
        }
        state->done = true;
    });

    watch([state, job]() {
        if (!state->done) {
            Jobs::instance().update(job, state->percent, state->op == "simplify" ? "Simplifying" : "Repairing");
            return false;
        }
        if (state->thread.joinable())
            state->thread.join();
        if (state->cancel) {
            Jobs::instance().finish(job, Job::State::Cancelled, json::object({{"changed", json::array()}}));
            return true;
        }
        // Commit on the main thread; every volume must still hold the mesh the job started from.
        Model&                                m = model();
        std::vector<std::pair<int, ModelVolume*>> found;
        for (const MeshTask& task : state->tasks) {
            const int    o = object_index_by_id(task.object_id);
            ModelVolume* volume = nullptr;
            if (o >= 0)
                for (ModelVolume* v : m.objects[static_cast<size_t>(o)]->volumes)
                    if (v->id().id == task.volume_id)
                        volume = v;
            if (volume == nullptr || volume->get_mesh_shared_ptr() != task.source) {
                Jobs::instance().finish(job, Job::State::Failed,
                                        json::object({{"error", json::object({{"code", "conflict"},
                                                                              {"message", task.label + " changed or was removed while the job ran; nothing was applied."},
                                                                              {"hint", "Read the scene and run mesh_edit again."}})}}));
                return true;
            }
            found.push_back({o, volume});
        }
        const bool    keep_painting = wxGetApp().app_config->get_bool("keep_painting");
        std::set<int> changed;
        json          volumes = json::array();
        for (size_t k = 0; k < state->tasks.size(); ++k) {
            MeshTask&    task = state->tasks[k];
            ModelVolume* volume = found[k].second;
            json         item = json::object();
            item["old_id"] = task.label;
            item["facets_before"] = task.facets_before;
            if (!task.error.empty())
                item["error"] = task.error;
            if (task.modified && task.error.empty()) {
                const bool painted = volume->is_any_painted();
                std::optional<TriangleSelector::SavedPainting> saved = keep_painting ? volume->save_painting() : std::nullopt;
                if (!keep_painting) {
                    volume->supported_facets.reset();
                    volume->seam_facets.reset();
                    volume->mmu_segmentation_facets.reset();
                    volume->fuzzy_skin_facets.reset();
                }
                volume->set_mesh(std::move(task.result));
                volume->restore_painting(saved);
                volume->calculate_convex_hull();
                volume->invalidate_convex_hull_2d();
                volume->set_new_unique_id();
                volume->get_object()->invalidate_bounding_box();
                volume->get_object()->ensure_on_bed();
                changed.insert(found[k].first);
                if (painted)
                    item["paint"] = keep_painting ? "remapped" : "removed";
            }
            item["id"] = Ids::of(*volume);
            item["facets"] = volume->mesh().facets_count();
            item["modified"] = task.modified && task.error.empty();
            if (state->op == "repair") {
                item["repair"] = json::object({{"welded_vertices", task.stats.welded_vertices},
                                               {"removed_triangles", task.stats.removed_triangles},
                                               {"reversed_shells", task.stats.reversed_shells},
                                               {"filled_holes", task.stats.filled_holes},
                                               {"removed_shells", task.stats.removed_shells},
                                               {"self_intersections", task.stats.self_intersections}});
            }
            volumes.push_back(std::move(item));
        }
        for (int o : changed) {
            plater().changed_mesh(o);
            Refresh::object(o);
            wxGetApp().obj_list()->update_item_error_icon(o, -1);
        }
        json result = json::object();
        result["volumes"] = volumes;
        result["changed"] = objects_json(std::vector<int>(changed.begin(), changed.end()));
        Jobs::instance().finish(job, Job::State::Finished, result);
        return true;
    });
    ctx.defer(wait_for_job(job, ctx.wait_s(100., 280.)));
    return nullptr;
}

}}}} // namespace Slic3r::GUI::Mcp::Edit
