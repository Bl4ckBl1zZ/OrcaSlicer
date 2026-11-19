// Project tools: project_new, project_open, project_save, model_import, export_file.
//
// Every question the GUI asks on these paths is turned into an argument and answered by a dialog
// rule (docs/mcp/tools-project.md lists them); a question no argument covers fails the call with
// dialog_blocked, quoting it.

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Format/OBJ.hpp"
#include "libslic3r/Format/ProjectStrategy.hpp"
#include "libslic3r/Format/STL.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"

#include "../McpFiles.hpp"
#include "../McpIds.hpp"
#include "../McpRefresh.hpp"
#include "../McpScene.hpp"
#include "../McpSchema.hpp"
#include "../McpTool.hpp"
#include "../McpUtil.hpp"
#include "edit/EditCommon.hpp"

namespace fs = boost::filesystem;

namespace Slic3r { namespace GUI { namespace Mcp {

namespace {

using namespace Edit;

std::string current_project_path() { return into_u8(plater().get_project_filename(".3mf")); }

uintmax_t file_bytes(const fs::path& path)
{
    boost::system::error_code ec;
    const uintmax_t size = fs::file_size(path, ec);
    return ec ? 0 : size;
}

// The non-dialog part of Plater::save_project(): write the 3MF, adopt the path as the project's
// file, mark the project saved. Fails with export_failed.
void save_project_to(const fs::path& path)
{
    Plater& p = plater();
    auto strategy = SaveStrategy::SplitModel | SaveStrategy::ShareMesh;
    if (wxGetApp().app_config->get_bool("export_sources_full_pathnames"))
        strategy = strategy | SaveStrategy::FullPathSources;
    if (p.export_3mf(path, strategy) < 0)
        fail("export_failed", "Saving the project to " + path.generic_string() + " failed.",
             "Check that the folder exists and that no other program holds the file open.");
    Slic3r::remove_backup(p.model(), false);
    p.set_project_filename(from_path(path));
    p.up_to_date(true, false);
    p.up_to_date(true, true);
    wxGetApp().update_saved_preset_from_current_preset();
    p.reset_project_dirty_after_save();
    p.update_title_dirty_status();
}

// Applies on_unsaved before an operation that replaces the project. Returns a note or "".
std::string handle_unsaved(ToolContext& ctx, const char* operation)
{
    Plater&           p = plater();
    const std::string policy = ctx.arg("on_unsaved", "fail");
    if (!p.is_project_dirty())
        return {};
    if (policy == "fail")
        fail("unsaved_changes", std::string("The current project has unsaved changes; ") + operation + " would discard them.",
             "Pass on_unsaved: \"save\" (with save_path when the project has no file yet) or \"discard\", or save first with project_save.",
             json::object({{"project", Scene::project_json()}}));
    if (policy == "save") {
        std::string path = ctx.arg("save_path", "");
        if (path.empty())
            path = current_project_path();
        if (path.empty())
            fail("no_path", "on_unsaved is \"save\" but the project has never been saved and no save_path was given.",
                 "Pass save_path (a .3mf inside a write root, see app_status.write_roots), or on_unsaved: \"discard\".");
        const bool same = !current_project_path().empty() && path == current_project_path();
        const fs::path target = Files::check_write(path, {".3mf"}, same || ctx.arg<bool>("overwrite", false), "save_path");
        save_project_to(target);
        return "Saved the previous project to " + target.generic_string() + ".";
    }
    // discard: the save question is answered "No" by a dialog rule; nothing else to do.
    return "Discarded the unsaved changes of the previous project.";
}

void add_unsaved_rules(ToolContext& ctx)
{
    // close_with_confirm(): "The current project has unsaved changes. Would you like to save ...?"
    // handle_unsaved() already saved or refused, so the remaining answer is always "No".
    ctx.on_dialog({"", "unsaved changes", DialogAnswer::No, true});
}

// ---------------------------------------------------------------------------------------------
// project_new
// ---------------------------------------------------------------------------------------------

json project_new(ToolContext& ctx)
{
    const std::string note = handle_unsaved(ctx, "starting a new project");
    add_unsaved_rules(ctx);
    // new_project(skip_confirm = true): no save question; silent: no tab switch, no zoom of the user's view.
    plater().new_project(true, true, ctx.has("name") ? from_u8(ctx.arg("name", "")) : wxString());
    if (!note.empty())
        ctx.note(note);
    ctx.set_slice_invalidated();
    json out = json::object();
    out["ok"] = true;
    out["changed"] = Scene::summary_json();
    return out;
}

// ---------------------------------------------------------------------------------------------
// project_open
// ---------------------------------------------------------------------------------------------

void add_3mf_rules(ToolContext& ctx, bool trust_gcode)
{
    // Embedded presets with custom G-code: only with trust_embedded_gcode.
    ctx.on_dialog({"Modified G-code", "", trust_gcode ? DialogAnswer::Yes : DialogAnswer::Default, trust_gcode});
    ctx.on_dialog({"Customized Preset", "", trust_gcode ? DialogAnswer::Yes : DialogAnswer::Default, trust_gcode});
    // Version notices and "loading geometry data only" information.
    ctx.on_dialog({"Newer 3MF version", "", DialogAnswer::Ok, true});
    ctx.on_dialog({"Load 3MF", "", DialogAnswer::Ok, true});
    // Old infill rotation templates: the fix the application proposes.
    ctx.on_dialog({"", "rotation template", DialogAnswer::Yes, true});
}

json project_open(ToolContext& ctx)
{
    const fs::path path = Files::check_read(ctx.arg("path", ""), {".3mf"}, "path");
    const std::string note = handle_unsaved(ctx, "opening a project");
    add_unsaved_rules(ctx);
    add_3mf_rules(ctx, ctx.arg<bool>("trust_embedded_gcode", false));
    ctx.on_dialog({"Attention", "UTF-8", DialogAnswer::Ok, true});

    // "<loadall>" skips the "open as project or import geometry" question.
    plater().load_project(from_path(path), "<loadall>");
    flush_pending_events();
    const std::vector<std::string> errors = error_dialog_texts(ctx);
    if (!errors.empty()) {
        ctx.accept_dialogs();
        fail("load_failed", "Opening " + path.generic_string() + " failed: " + errors.front(), "Check that the file is a valid 3MF project.",
             json::object({{"errors", errors}}));
    }
    if (current_project_path().empty() && model().objects.empty())
        fail("load_failed", "Opening " + path.generic_string() + " loaded nothing.",
             "The file may be damaged or not an OrcaSlicer/Bambu project; try model_import to take its geometry.");
    // The 3MF reader keeps one instance per object and plate (its plate map is keyed by object):
    // further instances of an object come back on no plate. Put those on the plate they lie on.
    int reassigned = 0;
    for (int o = 0; o < static_cast<int>(model().objects.size()); ++o)
        for (int i = 0; i < static_cast<int>(model().objects[static_cast<size_t>(o)]->instances.size()); ++i)
            if (Ids::plate_of_instance(o, i) < 0) {
                plates().notify_instance_update(o, i);
                if (Ids::plate_of_instance(o, i) >= 0)
                    ++reassigned;
            }
    if (reassigned > 0)
        Refresh::full();
    if (!note.empty())
        ctx.note(note);
    ctx.set_slice_invalidated();
    json out = json::object();
    out["ok"] = true;
    out["changed"] = Scene::summary_json();
    return out;
}

// ---------------------------------------------------------------------------------------------
// project_save
// ---------------------------------------------------------------------------------------------

json project_save(ToolContext& ctx)
{
    const bool        copy = ctx.arg<bool>("copy", false);
    const std::string current = current_project_path();
    std::string       path = ctx.arg("path", "");
    if (path.empty()) {
        if (copy)
            fail("invalid_argument", "`copy: true` needs a `path`.", "Pass the path of the copy.");
        if (current.empty())
            fail("no_path", "The project has never been saved, so there is no file to save to.",
                 "Pass `path`: a .3mf file inside one of app_status.write_roots.");
        path = current;
    }
    const bool     same = !current.empty() && fs::path(path) == fs::path(current);
    const fs::path target = Files::check_write(path, {".3mf"}, same || ctx.arg<bool>("overwrite", false), "path");
    if (copy) {
        if (!plater().export_3mf_copy(target))
            fail("export_failed", "Saving a copy to " + target.generic_string() + " failed.", "Check that the folder exists and the file is not open elsewhere.");
    } else
        save_project_to(target);
    flush_pending_events();
    json out = json::object();
    out["ok"] = true;
    out["path"] = target.generic_string();
    out["bytes"] = file_bytes(target);
    out["copy"] = copy;
    out["project"] = Scene::project_json();
    return out;
}

// ---------------------------------------------------------------------------------------------
// model_import
// ---------------------------------------------------------------------------------------------

json model_import(ToolContext& ctx)
{
    const json& paths_arg = ctx.args()["paths"];
    std::vector<fs::path> paths;
    for (size_t i = 0; i < paths_arg.size(); ++i) {
        const std::string label = "paths[" + std::to_string(i) + "]";
        paths.push_back(Files::check_read(paths_arg[i].get<std::string>(), Files::model_extensions(), label.c_str()));
    }
    const std::string as        = ctx.arg("as", "objects");
    const std::string units     = ctx.arg("units", "auto");
    const std::string multipart = ctx.arg("multipart", "auto");
    const std::string too_large = ctx.arg("too_large", "scale_down");
    const int         plate_idx = Ids::plate(ctx.has("plate") ? ctx.args()["plate"] : json(), "plate");
    if (plates().get_plate(plate_idx)->is_locked())
        fail("plate_locked", "Plate " + std::to_string(plate_idx + 1) + " is locked; nothing can be added to it.",
             "Unlock it with plate_manage action \"unlock\", or import onto another plate.");

    // Answers to the questions load_files() may ask.
    if (units == "auto")
        ctx.on_dialog({"Object too small", "", DialogAnswer::Yes, true});
    else
        ctx.on_dialog({"Object too small", "", DialogAnswer::No, true});
    ctx.on_dialog({"Multi-part object detected", "", multipart == "keep_separate" ? DialogAnswer::No : DialogAnswer::Yes, true});
    ctx.on_dialog({"Object too large", "", too_large == "keep" ? DialogAnswer::No : DialogAnswer::Yes, true});
    ctx.on_dialog({"The volume of the object is zero", "", DialogAnswer::Ok, true});
    ctx.on_dialog({"Attention", "UTF-8", DialogAnswer::Ok, true});
    ctx.on_dialog({"Texture Import Warning", "", DialogAnswer::Ok, true});
    add_3mf_rules(ctx, false);

    // STEP tessellation: the mesh-quality dialog is switched off and its values come from arguments.
    ScopedAppConfig app_config;
    app_config.set("enable_step_mesh_setting", "false");
    if (ctx.has("step")) {
        const json& step = ctx.args()["step"];
        if (step.contains("linear_deflection"))
            app_config.set("linear_deflection", std::to_string(step["linear_deflection"].get<double>()));
        if (step.contains("angle_deflection"))
            app_config.set("angle_deflection", std::to_string(step["angle_deflection"].get<double>()));
        if (step.contains("split_compound"))
            app_config.set("is_split_compound", step["split_compound"].get<bool>() ? "true" : "false");
    }

    ctx.snapshot("import " + paths.front().filename().string() + (paths.size() > 1 ? " and " + std::to_string(paths.size() - 1) + " more" : ""));

    // Objects land on the current plate; select the target plate for the import and put the user's
    // current plate back afterwards.
    PartPlateList& list = plates();
    const int      previous_plate = list.get_curr_plate_index();
    if (plate_idx != previous_plate)
        plater().select_plate(plate_idx);

    LoadStrategy strategy = LoadStrategy::LoadModel;
    if (units == "inch")
        strategy = strategy | LoadStrategy::ImperialUnits;

    const std::set<size_t> before = object_id_set();
    const int              filaments_before = Ids::filament_count();
    json                   failed = json::array();
    // One file at a time: each file keeps its own objects and the multi-file "single object?"
    // question is never asked; `as: single_object` merges afterwards.
    for (const fs::path& path : paths) {
        const size_t dialogs_before = ctx.dialogs().size();
        plater().load_files({path}, strategy, false);
        flush_pending_events();
        // Load errors arrive as deferred error boxes.
        for (size_t d = dialogs_before; d < ctx.dialogs().size(); ++d) {
            const DialogRecord& record = ctx.dialogs()[d];
            const std::string   text = record.title + "\n" + record.message;
            if (record.blocked && (contains_ci(text, "fail") || contains_ci(text, "error") || contains_ci(text, "invalid")))
                failed.push_back(json::object({{"path", path.generic_string()}, {"error", record.message.empty() ? record.title : record.message}}));
        }
    }
    std::vector<int> created = new_object_indices(before);

    if (units == "m") {
        for (int idx : created) {
            ModelObject* object = model().objects[static_cast<size_t>(idx)];
            object->scale_mesh_after_creation(1000.);
            for (ModelVolume* volume : object->volumes)
                volume->source.is_converted_from_meters = true;
            object->invalidate_bounding_box();
            object->ensure_on_bed();
        }
    }

    if (as == "single_object" && created.size() > 1) {
        // Assemble the imported objects into one, keeping their relative positions.
        Model&       m = model();
        ModelObject* merged = m.add_object();
        merged->name = m.objects[static_cast<size_t>(created.front())]->name;
        merged->add_instance();
        for (int idx : created) {
            const ModelObject* object = m.objects[static_cast<size_t>(idx)];
            for (const ModelInstance* instance : object->instances) {
                const Transform3d matrix = instance->get_matrix();
                for (const ModelVolume* volume : object->volumes) {
                    ModelVolume* copy = merged->add_volume(*volume);
                    copy->set_transformation(matrix * volume->get_matrix());
                    if (!copy->config.has("extruder") && object->config.has("extruder"))
                        copy->config.set_key_value("extruder", new ConfigOptionInt(object->config.extruder()));
                }
            }
        }
        merged->sort_volumes(true);
        merged->ensure_on_bed();
        merged->center_around_origin();
        merged->translate_instances(-merged->origin_translation);
        merged->origin_translation = Vec3d::Zero();
        merged->instances[0]->set_assemble_transformation(merged->instances[0]->get_transformation());
        const size_t merged_id = merged->id().id;
        std::vector<int> sorted = created;
        std::sort(sorted.rbegin(), sorted.rend());
        for (int idx : sorted)
            m.delete_object(static_cast<size_t>(idx));
        created = {object_index_by_id(merged_id)};
        refresh_structure();
    } else if (units == "m") {
        Refresh::objects(created);
    }

    if (plate_idx != previous_plate && previous_plate < list.get_plate_count())
        plater().select_plate(previous_plate);

    // Decide the outcome: blocked dialogs that were load errors become load_failed.
    if (!failed.empty())
        ctx.accept_dialogs();
    if (created.empty()) {
        if (!failed.empty())
            fail("load_failed", "Nothing was imported: " + failed[0]["error"].get<std::string>(),
                 "Check the file; for STEP try other step.linear_deflection / angle_deflection values.", json::object({{"failed", failed}}));
        fail("load_failed", "Nothing was imported from the given file(s).",
             "The file may be empty, contain only zero-volume objects, or the import was cancelled by a question (see dialogs).");
    }
    // A multi-colour file adds filament slots (as in the GUI); that is outside the undo stack.
    const int filaments_after = Ids::filament_count();
    if (filaments_after > filaments_before) {
        ctx.journal("filaments", "count", filaments_before, filaments_after, {});
        ctx.note("The imported model uses " + std::to_string(filaments_after) + " filaments; the project's filament list grew from " +
                 std::to_string(filaments_before) + " to " + std::to_string(filaments_after) +
                 " slots. Undoing the import does not remove them; filament_edit can.");
    }
    ctx.set_slice_invalidated();
    json out = json::object();
    out["ok"] = true;
    out["imported"] = objects_json(created);
    if (!failed.empty())
        out["failed"] = failed;
    return out;
}

// ---------------------------------------------------------------------------------------------
// export_file
// ---------------------------------------------------------------------------------------------

std::vector<int> export_objects(ToolContext& ctx)
{
    std::vector<int> idxs;
    if (ctx.has("objects")) {
        std::set<int> seen;
        for (const ItemRef& ref : Ids::items(ctx.args()["objects"], "objects"))
            if (seen.insert(ref.obj_idx).second)
                idxs.push_back(ref.obj_idx);
    } else {
        for (int i = 0; i < static_cast<int>(model().objects.size()); ++i)
            idxs.push_back(i);
    }
    if (idxs.empty())
        fail("invalid_argument", "There are no objects to export.", "Import or add objects first.");
    return idxs;
}

TriangleMesh object_world_mesh(const ModelObject& object)
{
    TriangleMesh mesh;
    for (size_t i = 0; i < object.instances.size(); ++i)
        mesh.merge(Plater::combine_mesh_fff(object, static_cast<int>(i)));
    return mesh;
}

std::string safe_file_part(std::string name)
{
    for (char& ch : name)
        if (std::string("<>:\"/\\|?* ").find(ch) != std::string::npos || static_cast<unsigned char>(ch) < 32)
            ch = '_';
    return name.empty() ? std::string("object") : name;
}

json export_file(ToolContext& ctx)
{
    const std::string kind = ctx.arg("kind", "");
    const bool        overwrite = ctx.arg<bool>("overwrite", false);
    const std::string path_arg = ctx.arg("path", "");
    json              files = json::array();
    auto              record = [&files](const fs::path& p) {
        files.push_back(json::object({{"path", p.generic_string()}, {"bytes", file_bytes(p)}}));
    };

    if (kind == "stl" || kind == "obj") {
        const std::string ext = "." + kind;
        const std::vector<int> idxs = export_objects(ctx);
        const bool merge = ctx.arg<bool>("merge", true);
        const fs::path target = Files::check_write(path_arg, {ext}, overwrite, "path");
        auto write = [&](const fs::path& p, TriangleMesh& mesh) {
            const bool ok = kind == "stl" ? store_stl(p.string().c_str(), &mesh, true) : store_obj(p.string().c_str(), &mesh);
            if (!ok)
                fail("export_failed", "Writing " + p.generic_string() + " failed.", "Check that the folder exists and the file is not open elsewhere.");
            record(p);
        };
        if (merge || idxs.size() == 1) {
            TriangleMesh mesh;
            for (int idx : idxs)
                mesh.merge(object_world_mesh(*model().objects[static_cast<size_t>(idx)]));
            write(target, mesh);
        } else {
            // One file per object: <stem>_<object name>.<ext> next to `path`.
            std::vector<fs::path> targets;
            std::set<std::string> used;
            for (int idx : idxs) {
                std::string stem = target.stem().string() + "_" + safe_file_part(model().objects[static_cast<size_t>(idx)]->name);
                std::string name = stem;
                for (int n = 2; used.count(name); ++n)
                    name = stem + "_" + std::to_string(n);
                used.insert(name);
                targets.push_back(Files::check_write((target.parent_path() / (name + ext)).string(), {ext}, overwrite, "path"));
            }
            for (size_t i = 0; i < idxs.size(); ++i) {
                TriangleMesh mesh = object_world_mesh(*model().objects[static_cast<size_t>(idxs[i])]);
                write(targets[i], mesh);
            }
        }
    } else if (kind == "3mf") {
        const fs::path target = Files::check_write(path_arg, {".3mf"}, overwrite, "path");
        if (!plater().export_3mf_copy(target))
            fail("export_failed", "Writing " + target.generic_string() + " failed.", "Check that the folder exists and the file is not open elsewhere.");
        record(target);
    } else if (kind == "gcode" || kind == "gcode_3mf") {
        const bool all = ctx.has("plate") && ctx.args()["plate"].is_string() && ctx.args()["plate"].get<std::string>() == "all";
        if (all && kind == "gcode")
            fail("invalid_argument", "`plate: \"all\"` is only for kind \"gcode_3mf\"; a .gcode file holds one plate.", "Export each plate separately.");
        const int plate_idx = all ? -1 : Ids::plate(ctx.has("plate") ? ctx.args()["plate"] : json(), "plate");
        PartPlateList& list = plates();
        std::vector<int> check;
        if (all) {
            for (int p = 0; p < list.get_plate_count(); ++p)
                if (!list.get_plate(p)->empty())
                    check.push_back(p);
        } else
            check.push_back(plate_idx);
        for (int p : check) {
            PartPlate* plate = list.get_plate(p);
            if (!plate->is_slice_result_valid() || !plate->is_slice_result_ready_for_print())
                fail("not_sliced", "Plate " + std::to_string(p + 1) + " has no valid slice result to export.",
                     "Slice the plate first (slice), then export.", json::object({{"plate", p + 1}}));
        }
        if (plater().is_background_process_slicing())
            fail("busy_slicing", "Slicing is running; the result is not final yet.", "Wait for the slice to finish (slice_status), then export.");
        if (kind == "gcode") {
            const fs::path target = Files::check_write(path_arg, {".gcode"}, overwrite, "path");
            const std::string source = list.get_plate(plate_idx)->get_tmp_gcode_path();
            boost::system::error_code ec;
            if (source.empty() || !fs::exists(source, ec))
                fail("not_sliced", "The G-code of plate " + std::to_string(plate_idx + 1) + " is not available.", "Slice the plate again, then export.");
            fs::copy_file(source, target, fs::copy_options::overwrite_existing, ec);
            if (ec)
                fail("export_failed", "Copying the G-code to " + target.generic_string() + " failed: " + ec.message(), "Check the folder.");
            record(target);
        } else {
            if (!boost::iends_with(path_arg, ".gcode.3mf"))
                fail("invalid_argument", "`path` must end in .gcode.3mf for kind \"gcode_3mf\".", "Name the file like plate1.gcode.3mf.");
            const fs::path target = Files::check_write(path_arg, {".3mf"}, overwrite, "path");
            SaveStrategy strategy = SaveStrategy::Silence | SaveStrategy::SplitModel | SaveStrategy::WithGcode | SaveStrategy::SkipModel;
            if (wxGetApp().app_config->get_bool("export_statistics"))
                strategy = strategy | SaveStrategy::WithStatistics;
            if (plater().export_3mf(target, strategy, all ? PLATE_ALL_IDX : plate_idx) < 0)
                fail("export_failed", "Writing " + target.generic_string() + " failed.", "Check the folder; a post-processing script may have failed.");
            record(target);
        }
    } else
        fail("invalid_argument", "Unknown `kind` \"" + kind + "\".", "Use stl, obj, 3mf, gcode or gcode_3mf.");

    flush_pending_events();
    json out = json::object();
    out["ok"] = true;
    out["files"] = files;
    if (files.size() == 1) {
        out["path"] = files[0]["path"];
        out["bytes"] = files[0]["bytes"];
    }
    return out;
}

} // namespace

MCP_TOOL_GROUP(Project)
{
    {
        ToolDef tool;
        tool.name = "project_new";
        tool.title = "New project";
        tool.description =
            "Start a new, empty project (objects, plates and project settings are cleared; the selected printer, process and filament "
            "presets stay). When the current project has unsaved changes, `on_unsaved` decides: \"fail\" (default) returns unsaved_changes, "
            "\"discard\" drops them, \"save\" saves first (to the current file, or to `save_path`). Not undoable: starting a project "
            "clears the undo history. Returns the scene summary.";
        tool.input_schema = Schema::parse(R"JSON({
            "type": "object",
            "properties": {
                "on_unsaved": {"enum": ["fail", "discard", "save"], "default": "fail"},
                "save_path": {"type": "string", "description": "Where to save the old project with on_unsaved \"save\" when it has no file yet (.3mf in a write root)."},
                "overwrite": {"type": "boolean", "default": false, "description": "Allow save_path to replace an existing file."},
                "name": {"type": "string", "minLength": 1, "maxLength": 200, "description": "Project name (default \"Untitled\")."}
            },
            "additionalProperties": false})JSON", "project_new");
        tool.destructive = true;
        tool.accepts_if_rev = false;
        tool.handler = project_new;
        registry.add(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "project_open";
        tool.title = "Open a 3MF project";
        tool.description =
            "Open a 3MF project, replacing the current one: objects, plates, per-plate settings and the presets stored in the project. "
            "`on_unsaved` as in project_new. Custom G-code embedded in the project's presets is accepted only with "
            "trust_embedded_gcode: true (otherwise the call fails with dialog_blocked quoting the G-code question). Version notices are "
            "returned in `dialogs`. To add only the geometry of a 3MF to the current project use model_import. Ids are new after this. "
            "Not undoable. Returns the scene summary.";
        tool.input_schema = Schema::parse(R"JSON({
            "type": "object",
            "properties": {
                "path": {"type": "string", "description": "Absolute path of a .3mf inside a read root (app_status.read_roots)."},
                "on_unsaved": {"enum": ["fail", "discard", "save"], "default": "fail"},
                "save_path": {"type": "string"},
                "overwrite": {"type": "boolean", "default": false},
                "trust_embedded_gcode": {"type": "boolean", "default": false}
            },
            "required": ["path"],
            "additionalProperties": false})JSON", "project_open");
        tool.destructive = true;
        tool.accepts_if_rev = false;
        tool.run_timeout_ms = 280000;
        tool.handler = project_open;
        registry.add(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "project_save";
        tool.title = "Save the project";
        tool.description =
            "Save the project as a 3MF. Without `path` it saves to the project's current file (no_path when it has none). With `path` it "
            "saves there and the project now uses that file (Save As), unless copy: true, which writes a copy and leaves the project's "
            "file and unsaved state alone. An existing file other than the project's own is replaced only with overwrite: true. "
            "Returns {path, bytes, project}.";
        tool.input_schema = Schema::parse(R"JSON({
            "type": "object",
            "properties": {
                "path": {"type": "string", "description": ".3mf inside a write root (app_status.write_roots)."},
                "copy": {"type": "boolean", "default": false},
                "overwrite": {"type": "boolean", "default": false}
            },
            "additionalProperties": false})JSON", "project_save");
        tool.idempotent = true;
        tool.accepts_if_rev = false;
        tool.handler = project_save;
        registry.add(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "model_import";
        tool.title = "Import model files";
        tool.description =
            "Import model files into the current project as new objects: STL, OBJ, 3MF (geometry only; use project_open for a whole "
            "project), STEP/STP, AMF, DRC, SVG. Each file becomes its own object(s); as: \"single_object\" assembles everything imported "
            "into one multi-part object. The import questions are arguments: `units` (auto: tiny models that look like metres or "
            "inches are scaled up as the GUI proposes; mm: never; inch / m: convert everything), `multipart` (a file whose objects sit at "
            "several heights: auto/merge = one object with parts, keep_separate = separate objects), `too_large` (scale_down or keep), "
            "`step` tessellation (linear_deflection mm, angle_deflection rad, split_compound). Objects are placed on `plate` (default "
            "current) as the GUI places them; call object_arrange afterwards to pack them. One undo step. Returns `imported` objects in "
            "scene_get shape, `failed` files, and the questions answered in `dialogs`.";
        tool.input_schema = Schema::parse(R"JSON({
            "type": "object",
            "properties": {
                "paths": {"type": "array", "items": {"type": "string"}, "minItems": 1, "maxItems": 50},
                "as": {"enum": ["objects", "single_object"], "default": "objects"},
                "units": {"enum": ["auto", "mm", "inch", "m"], "default": "auto"},
                "multipart": {"enum": ["auto", "merge", "keep_separate"], "default": "auto"},
                "too_large": {"enum": ["scale_down", "keep"], "default": "scale_down"},
                "plate": {"$ref": "#/$defs/PlateRef"},
                "step": {"type": "object", "properties": {
                    "linear_deflection": {"type": "number", "exclusiveMinimum": 0, "maximum": 1, "description": "mm; smaller = finer (default 0.003)."},
                    "angle_deflection": {"type": "number", "exclusiveMinimum": 0, "maximum": 1, "description": "radians; smaller = finer (default 0.5)."},
                    "split_compound": {"type": "boolean", "description": "Split STEP compounds into separate objects."}},
                    "additionalProperties": false}
            },
            "required": ["paths"],
            "additionalProperties": false})JSON", "model_import");
        tool.run_timeout_ms = 280000;
        tool.handler = model_import;
        registry.add(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "export_file";
        tool.title = "Export STL, OBJ, 3MF or G-code";
        tool.description =
            "Write files: kind \"stl\" / \"obj\" = meshes of objects as placed on the plates (all instances; `objects` to choose, "
            "merge: false for one file per object named <stem>_<object>.<ext>), \"3mf\" = a copy of the project (project_save copy), "
            "\"gcode\" = the sliced G-code of one plate, \"gcode_3mf\" = sliced plate(s) as .gcode.3mf (plate \"all\" for every "
            "sliced plate). G-code needs a valid slice (not_sliced otherwise); the user's post-processing scripts are not run for "
            "\"gcode\". Paths must be inside a write root; existing files need overwrite: true. Returns {files: [{path, bytes}]}.";
        tool.input_schema = Schema::parse(R"JSON({
            "type": "object",
            "properties": {
                "kind": {"enum": ["stl", "obj", "3mf", "gcode", "gcode_3mf"]},
                "path": {"type": "string"},
                "objects": {"type": "array", "items": {"$ref": "#/$defs/Id"}, "minItems": 1},
                "merge": {"type": "boolean", "default": true},
                "plate": {"oneOf": [{"$ref": "#/$defs/PlateRef"}, {"const": "all"}]},
                "overwrite": {"type": "boolean", "default": false}
            },
            "required": ["kind", "path"],
            "additionalProperties": false})JSON", "export_file");
        tool.read_only = false;
        tool.idempotent = true;
        tool.accepts_if_rev = false;
        tool.handler = export_file;
        registry.add(std::move(tool));
    }
}

}}} // namespace Slic3r::GUI::Mcp
