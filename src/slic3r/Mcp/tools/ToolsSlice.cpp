// Slice tools: plate_validate, slice, slice_status, slice_cancel, slice_result, slice_export, and
// the test-only debug_slice_fixture.
//
// Slicing goes through the Slice button's own path: the tool posts EVT_GLTOOLBAR_SLICE_PLATE /
// EVT_GLTOOLBAR_SLICE_ALL to the Plater (on_action_slice_plate/all -> Plater::reslice), so a watching
// user sees what a click would show (the window switches to the Preview tab). The progress and the
// outcome are followed by Slice::Tracker (tools/slice/SliceTracker.hpp). Results are read from the
// plate's own GCodeProcessorResult (tools/slice/SliceData.hpp).

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>

#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/BackgroundSlicingProcess.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GLToolbar.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Tab.hpp"
#include "libslic3r/PresetBundle.hpp"

#include "../McpFiles.hpp"
#include "../McpIds.hpp"
#include "../McpJobs.hpp"
#include "../McpRefresh.hpp"
#include "../McpSchema.hpp"
#include "../McpTool.hpp"
#include "slice/SliceAnalysisGlue.hpp"
#include "slice/SliceData.hpp"
#include "slice/SlicePreview.hpp"
#include "slice/SliceTracker.hpp"

namespace Slic3r { namespace GUI { namespace Mcp {

namespace {

using namespace Slice;

Plater& plater() { return *wxGetApp().plater(); }

// `plate` argument that may also be "all". Returns -1 for all.
int plate_or_all(const ToolContext& ctx, const char* key = "plate")
{
    if (ctx.has(key) && ctx.args()[key].is_string() && ctx.args()[key].get<std::string>() == "all")
        return -1;
    return Ids::plate(ctx.has(key) ? ctx.args()[key] : json(nullptr), key);
}

void refuse_while_slicing(int plate_idx)
{
    if (plate_idx < 0 ? slicing_now() : plate_busy(plate_idx))
        fail("busy_slicing", "The plate is being sliced right now.",
             "Wait with slice_status (it long-polls), or stop it with slice_cancel.",
             json::object({{"plate", bsp_plate_index() + 1}, {"percent", Tracker::instance().last_percent()}}));
}

void require_result(int plate_idx)
{
    refuse_while_slicing(plate_idx);
    if (!plate_has_result(plate_idx)) {
        json state = plate_state(plate_idx);
        const std::string s = state.value("state", "not_sliced");
        std::string hint = "Call slice first.";
        if (s == "invalidated")
            hint = "The result was invalidated by a change; call slice again.";
        else if (s == "empty")
            hint = "Put an object on the plate, then slice.";
        else if (s == "failed" || s == "invalid")
            hint = "Fix the problems plate_validate lists, then slice.";
        fail("not_sliced", "Plate " + std::to_string(plate_idx + 1) + " has no valid slice result (state: " + s + ").", hint, state);
    }
}

void install_summarizer()
{
    Tracker& t = Tracker::instance();
    t.ensure();
    if (!t.summarize)
        t.summarize = [](int plate_idx) {
            json out;
            try {
                out = compact_summary(plate_idx);
                // Keep the analysis of every new result, so analyze diff can compare with it after the next change.
                Analysis::remember(plate_idx);
            } catch (const std::exception& ex) {
                if (out.is_null())
                    out = json::object({{"plate", plate_idx + 1}});
                out["summary_error"] = ex.what();
            }
            return out;
        };
}

json status_json()
{
    std::shared_ptr<Job> job = Jobs::instance().latest("slice");
    json out = json::object();
    if (job) {
        out = job_json(*job);
    } else {
        out["status"] = slicing_now() ? "running" : "idle";
        if (slicing_now()) {
            out["percent"] = Tracker::instance().last_percent();
            out["text"] = Tracker::instance().last_text();
            out["started_by"] = "user";
        }
    }
    json plates = json::array();
    for (int p = 0; p < plate_count(); ++p)
        plates.push_back(plate_state(p));
    out["plates"] = std::move(plates);
    return out;
}

// Long-poll on the slice job; answers with status_json().
Pending wait_slice(std::shared_ptr<Job> job, double wait_s)
{
    Pending pending;
    pending.wait_s = wait_s;
    pending.poll = [job](ToolContext& ctx, json& result) {
        if (job->state == Job::State::Running) {
            ctx.progress(job->percent, job->text);
            return false;
        }
        result = job_json(*job);
        return true;
    };
    pending.on_deadline = [job](ToolContext&) {
        json out = job_json(*job);
        out["hint"] = "Still slicing: call slice_status (it long-polls with wait_s) or slice_cancel.";
        return out;
    };
    return pending;
}

// --------------------------------------------------------------------------------------------
json plate_validate(ToolContext& ctx)
{
    install_summarizer();
    const int plate_idx = Ids::plate(ctx.has("plate") ? ctx.args()["plate"] : json(nullptr));
    refuse_while_slicing(plate_idx);
    bool has_errors = false;
    json out = validation(plate_idx, true, has_errors);
    out["state"] = plate_state(plate_idx);
    return out;
}

json slice(ToolContext& ctx)
{
    install_summarizer();
    const int  target = plate_or_all(ctx);
    const bool all = target < 0;
    if (Tracker::instance().running_job() || slicing_now())
        fail("busy_slicing", "A slice is already running" + std::string(Tracker::instance().running_job() ? "." : " (started in the window)."),
             "Wait with slice_status, or stop it with slice_cancel.", json::object({{"plate", bsp_plate_index() + 1}}));

    std::vector<int> plates;
    if (all) {
        for (int p = 0; p < plate_count(); ++p)
            if (handles(p).plate->has_printable_instances())
                plates.push_back(p);
        if (plates.empty())
            fail("nothing_to_slice", "No plate has a printable object.", "Add or move objects onto a plate first.");
    } else {
        if (!handles(target).plate->has_printable_instances())
            fail("nothing_to_slice", "Plate " + std::to_string(target + 1) + " has no printable object.",
                 "Move an object onto the plate, or mark its objects printable (object_update).", plate_state(target));
        plates.push_back(target);
    }

    // Re-slice only what is invalid: the Slice button is disabled on a sliced plate too.
    bool all_valid = true;
    for (int p : plates)
        all_valid = all_valid && plate_has_result(p);
    if (all_valid) {
        json out = json::object();
        out["status"] = "finished";
        out["reused"] = true;
        json list = json::array();
        for (int p : plates)
            list.push_back(compact_summary(p));
        out["result"] = json::object({{"outcome", "finished"}, {"plates", std::move(list)}});
        ctx.note("The plate(s) already had a valid result; nothing was re-sliced.");
        return out;
    }

    if (!all) {
        // The Slice button works on the current plate.
        if (target != current_plate()) {
            plater().select_plate(target);
            ctx.note("Selected plate " + std::to_string(target + 1) + " in the window (slicing works on the current plate).");
        }
        // What disables the Slice button: validation errors. Refuse with the details instead of a silent no-op.
        bool has_errors = false;
        json v = validation(target, true, has_errors);
        if (has_errors)
            fail("validation_failed", "Plate " + std::to_string(target + 1) + " cannot be sliced: " + v["errors"][0].value("message", "") ,
                 "Fix the listed problems (each has a `fix`), then slice again. plate_validate re-checks without slicing.", v);
    }

    std::shared_ptr<Job> job = Tracker::instance().start_job("slice", plates, all);
    wxPostEvent(&plater(), SimpleEvent(all ? EVT_GLTOOLBAR_SLICE_ALL : EVT_GLTOOLBAR_SLICE_PLATE));
    ctx.defer(wait_slice(job, ctx.wait_s(100., 280.)));
    return nullptr;
}

json slice_status(ToolContext& ctx)
{
    install_summarizer();
    std::shared_ptr<Job> job = Tracker::instance().running_job();
    const double wait = ctx.wait_s(0., 280.);
    if (job && wait > 0.) {
        Pending pending = wait_slice(job, wait);
        auto inner_poll = pending.poll;
        pending.poll = [inner_poll](ToolContext& c, json& result) {
            if (!inner_poll(c, result))
                return false;
            result = status_json();
            return true;
        };
        pending.on_deadline = [](ToolContext&) { return status_json(); };
        ctx.defer(std::move(pending));
        return nullptr;
    }
    return status_json();
}

json slice_cancel(ToolContext& ctx)
{
    install_summarizer();
    std::shared_ptr<Job> job = Tracker::instance().running_job();
    if (!job && !slicing_now())
        fail("not_running", "No slice is running.", "slice_status shows the state of every plate.");
    const int plate = bsp_plate_index();
    if (job)
        job->cancel();
    else {
        Tracker::instance().mark_cancel_requested();
        if (!plater().background_process().idle())
            plater().background_process().stop();
    }
    json out = json::object();
    out["status"] = "cancelled";
    out["plate"] = plate + 1;
    if (job)
        out["job"] = job->id;
    if (!job)
        ctx.note("The slice had been started in the window, not by the agent.");
    return out;
}

json slice_result(ToolContext& ctx)
{
    install_summarizer();
    const int plate_idx = Ids::plate(ctx.has("plate") ? ctx.args()["plate"] : json(nullptr));
    require_result(plate_idx);
    std::set<std::string> sections = {"summary", "warnings"};
    if (ctx.has("sections")) {
        sections.clear();
        for (const json& s : ctx.args()["sections"])
            sections.insert(s.get<std::string>());
    }
    json out = result_json(plate_idx, sections, ctx.arg("time_mode", "normal"));
    out["state"] = plate_state(plate_idx)["state"];
    return out;
}

json slice_export(ToolContext& ctx)
{
    install_summarizer();
    const std::string kind = ctx.arg("kind", "gcode");
    const bool        overwrite = ctx.arg<bool>("overwrite", false);
    const int         target = plate_or_all(ctx);
    if (target < 0 && kind != "gcode_3mf")
        fail("invalid_argument", "`plate`: \"all\" is only possible with kind \"gcode_3mf\" (a .gcode file holds one plate).",
             "Export one plate per .gcode file, or use kind gcode_3mf for all plates in one file.");
    std::vector<int> plates;
    if (target < 0) {
        for (int p = 0; p < plate_count(); ++p)
            if (handles(p).plate->has_printable_instances())
                plates.push_back(p);
        if (plates.empty())
            fail("not_sliced", "No plate has a printable object.", "Add objects and slice first.");
    } else
        plates.push_back(target);
    for (int p : plates)
        require_result(p);

    const std::string path_arg = ctx.arg("path", "");
    if (kind == "gcode") {
        const boost::filesystem::path path = Files::check_write(path_arg, {".gcode"}, overwrite);
        PlateHandles h = handles(plates.front());
        const std::string source = h.plate->get_tmp_gcode_path();
        if (source.empty() || !boost::filesystem::exists(source))
            fail("export_failed", "The sliced G-code of plate " + std::to_string(plates.front() + 1) + " is not on disk.",
                 "Slice the plate again, then export.");
        std::string error;
        const CopyFileResult rc = copy_file(source, path.string(), error, false);
        if (rc != CopyFileResult::SUCCESS)
            fail("export_failed", "Writing \"" + path.string() + "\" failed: " + error, "Check the folder is writable, or pick another path.");
        json out = json::object();
        out["ok"] = true;
        out["kind"] = kind;
        out["path"] = path.generic_string();
        out["plate"] = plates.front() + 1;
        json info = gcode_file_info(path.string());
        out["bytes"] = info["bytes"];
        out["header"] = info["comments"];
        out["summary"] = compact_summary(plates.front());
        const DynamicPrintConfig& cfg = wxGetApp().preset_bundle->prints.get_edited_preset().config;
        if (cfg.has("post_process") && !cfg.option<ConfigOptionStrings>("post_process")->values.empty())
            ctx.note("Post-processing scripts of the print preset were not run: the file is the G-code as sliced.");
        return out;
    }

    // .gcode.3mf: the application's own writer (Plater::export_gcode_3mf without its file dialog).
    const boost::filesystem::path path = Files::check_write(path_arg, {".gcode.3mf"}, overwrite);
    SaveStrategy strategy = SaveStrategy::Silence | SaveStrategy::SplitModel | SaveStrategy::WithGcode | SaveStrategy::SkipModel;
    const int    rc = plater().export_3mf(path, strategy, target < 0 ? PLATE_ALL_IDX : target);
    if (rc < 0 || !boost::filesystem::exists(path))
        fail("export_failed", "Writing \"" + path.string() + "\" failed (code " + std::to_string(rc) + ").", "Check the folder, or slice again.");
    json out = json::object();
    out["ok"] = true;
    out["kind"] = kind;
    out["path"] = path.generic_string();
    out["bytes"] = static_cast<long long>(boost::filesystem::file_size(path));
    json list = json::array();
    for (int pl : plates)
        list.push_back(compact_summary(pl));
    out["plates"] = std::move(list);
    return out;
}

// --------------------------------------------------------------------------------------------
// Test fixture helper (debug tools only): scene set-ups the slice tests need before the edit and
// settings groups are merged.
json debug_slice_fixture(ToolContext& ctx)
{
    const std::string action = ctx.arg("action", "");
    json out = json::object({{"ok", true}, {"action", action}});
    if (action == "add_plate") {
        ctx.snapshot("debug add plate");
        out["plate"] = plater().add_plate() + 1;
    } else if (action == "delete_plate") {
        const int idx = Ids::plate(ctx.args()["plate"]);
        ctx.snapshot("debug delete plate");
        out["result"] = plater().delete_plate(idx);
    } else if (action == "move") {
        ObjectRef ref = Ids::object(ctx.arg("object", ""), "object");
        const int idx = Ids::plate(ctx.has("plate") ? ctx.args()["plate"] : json(nullptr));
        std::vector<double> offset = ctx.arg<std::vector<double>>("offset", {});
        PartPlate* plate = plater().get_partplate_list().get_plate(idx);
        ctx.snapshot("debug move");
        const BoundingBoxf3 box = plate->get_bounding_box(false);
        Vec3d target = Vec3d(box.center().x(), box.center().y(), 0.);
        if (offset.size() >= 2)
            target += Vec3d(offset[0], offset[1], 0.);
        for (ModelInstance* inst : ref.object->instances) {
            const BoundingBoxf3 ib = ref.object->instance_bounding_box(*inst);
            Vec3d d = target - Vec3d(ib.center().x(), ib.center().y(), 0.);
            inst->set_offset(inst->get_offset() + d);
        }
        ref.object->invalidate_bounding_box();
        plater().get_partplate_list().reload_all_objects();
        Refresh::full();
        ctx.set_slice_invalidated();
    } else if (action == "set") {
        const std::string type = ctx.arg("preset", "print");
        DynamicPrintConfig cfg;
        ConfigSubstitutionContext sub(ForwardCompatibilitySubstitutionRule::Disable);
        const json& values = ctx.args()["values"];
        for (auto it = values.begin(); it != values.end(); ++it)
            cfg.set_deserialize(it.key(), it.value().is_string() ? it.value().get<std::string>() : it.value().dump(), sub);
        Tab* tab = wxGetApp().get_tab(type == "printer" ? Preset::TYPE_PRINTER : type == "filament" ? Preset::TYPE_FILAMENT : Preset::TYPE_PRINT);
        if (tab == nullptr)
            fail("internal", "no tab for preset type " + type);
        tab->load_config(cfg);
        ctx.set_slice_invalidated();
    } else if (action == "load") {
        const boost::filesystem::path path = Files::check_read(ctx.arg("path", ""), Files::model_extensions());
        ctx.snapshot("debug load");
        std::vector<size_t> idx = plater().load_files({path}, LoadStrategy::LoadModel);
        json ids = json::array();
        for (size_t i : idx)
            if (i < plater().model().objects.size())
                ids.push_back(Ids::of(*plater().model().objects[i]));
        out["objects"] = ids;
    } else if (action == "delete_object") {
        ObjectRef ref = Ids::object(ctx.arg("object", ""), "object");
        ctx.snapshot("debug delete");
        plater().delete_object_from_model(static_cast<size_t>(ref.obj_idx));
        Refresh::full();
    } else if (action == "add_filament") {
        // The sidebar's "+" button.
        plater().sidebar().add_filament();
        out["filaments"] = Ids::filament_count();
    } else if (action == "viewer_state") {
        // The window's own G-code preview, to show that offscreen pictures leave it alone.
        GLCanvas3D* canvas = plater().get_preview_canvas3D();
        const auto& range = canvas->get_gcode_viewer().get_layers_z_range();
        out["layers_view_range"] = json::array({range[0], range[1]});
        out["preview_tab"] = plater().is_preview_shown();
        out["current_plate"] = current_plate() + 1;
    } else
        fail("invalid_argument", "unknown action " + action);
    return out;
}

} // namespace

MCP_TOOL_GROUP(Slice)
{
    // screenshot view "preview" (the screenshot tool, group View, registers after this group).
    Slice::register_preview_view();
    {
        ToolDef tool;
        tool.name = "plate_validate";
        tool.title = "Check a plate before slicing";
        tool.description =
            "Pre-slice check of a plate without slicing: objects partly outside the plate, empty plate, filament / nozzle / plate-type "
            "incompatibilities, object collisions, and the slicer's own configuration validation (Print::validate). Returns {ok, errors, "
            "warnings, state}; each error has `message`, `kind`, the `object` ({id, name}) or setting `key` it concerns, and a `fix`. "
            "Errors are what makes the window's Slice button unavailable; warnings do not block slicing. The current plate is checked fully; "
            "another plate gets the geometric checks and its last applied print (slice selects it and re-checks).";
        tool.input_schema = Schema::parse(R"JSON({
            "type": "object",
            "properties": {"plate": {"$ref": "#/$defs/PlateRef", "description": "Default: the current plate."}},
            "additionalProperties": false})JSON", "plate_validate");
        tool.read_only = true;
        tool.idempotent = true;
        tool.handler = plate_validate;
        registry.add(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "slice";
        tool.title = "Slice a plate or all plates";
        tool.description =
            "Slice one plate (default: the current one) or all plates, exactly as the Slice button does (the window switches to the Preview "
            "tab). Runs as a job: waits up to `wait_s` seconds and returns the finished job with a per-plate summary (time, filament, layers, "
            "warnings), or {status: running, job, percent, text}; then poll slice_status. Streams progress notifications when the client asks. "
            "A plate that already has a valid result is not re-sliced (`reused`). Fails before starting with validation_failed (the errors, "
            "each with a fix), nothing_to_slice or busy_slicing. A change to the scene or settings while slicing stops the slice (status "
            "cancelled, cancel_reason scene_changed_or_user): slice again. Results: slice_result, slice_layers, analyze; pictures: screenshot "
            "view=preview; file: slice_export.";
        tool.input_schema = Schema::parse(R"JSON({
            "type": "object",
            "properties": {
                "plate": {"oneOf": [{"$ref": "#/$defs/PlateRef"}, {"const": "all"}], "description": "Plate to slice, or \"all\". Default: current."},
                "wait_s": {"type": "number", "minimum": 0, "maximum": 280, "default": 100, "description": "Seconds to wait for the result before returning status running."}
            },
            "additionalProperties": false})JSON", "slice");
        tool.idempotent = true;
        tool.accepts_if_rev = false;
        tool.handler = slice;
        registry.add(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "slice_status";
        tool.title = "Slicing progress and plate states";
        tool.description =
            "Progress of the running slice and the state of every plate: empty, not_sliced, slicing, sliced, invalidated (a change after the "
            "last slice), invalid (validation errors), failed (with the error), cancelled. With `wait_s` it long-polls until the running slice "
            "ends or the time is up, streaming progress. Returns the latest slice job ({job, status, percent, text, elapsed_s, result}) plus "
            "`plates`.";
        tool.input_schema = Schema::parse(R"JSON({
            "type": "object",
            "properties": {"wait_s": {"type": "number", "minimum": 0, "maximum": 280, "default": 0, "description": "Wait up to this long for a running slice to end."}},
            "additionalProperties": false})JSON", "slice_status");
        tool.read_only = true;
        tool.idempotent = true;
        tool.handler = slice_status;
        registry.add(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "slice_cancel";
        tool.title = "Stop slicing";
        tool.description = "Stop the running slice (as the Cancel button of the slicing notification does). Fails with not_running when "
                           "nothing is being sliced. The plate keeps no result; slice again to get one.";
        tool.input_schema = Schema::parse(R"JSON({"type": "object", "properties": {}, "additionalProperties": false})JSON", "slice_cancel");
        tool.destructive = true;
        tool.idempotent = true;
        tool.requires_idle_ui = false;
        tool.accepts_if_rev = false;
        tool.handler = slice_cancel;
        registry.add(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "slice_result";
        tool.title = "Results of a sliced plate";
        tool.description =
            "Figures of a sliced plate, from the slicer's own statistics: `summary` (print time normal/silent, prepare and first-layer time, "
            "filament m / g / cost, layers and layer heights, height, filament and extruder changes, travel, support / prime tower / flush "
            "volume, toolpath box in plate coordinates), `filaments` (per slot: model, support, tower, flush volumes, length, weight, cost), "
            "`features` (per feature: time and share, path length, volume, filament), `objects` (per object instance: weight, volume, support "
            "weight, box), `warnings` (slicer warnings with the object, toolpaths outside, collisions, nozzle reach). Fails with not_sliced "
            "(state and reason in details, e.g. invalidated after a change) or busy_slicing. Per-layer data: slice_layers; checks: analyze.";
        tool.input_schema = Schema::parse(R"JSON({
            "type": "object",
            "properties": {
                "plate": {"$ref": "#/$defs/PlateRef"},
                "sections": {"type": "array", "items": {"enum": ["summary", "filaments", "features", "objects", "warnings"]}, "default": ["summary", "warnings"]},
                "time_mode": {"enum": ["normal", "silent"], "default": "normal", "description": "Time mode for per-feature times."}
            },
            "additionalProperties": false})JSON", "slice_result");
        tool.read_only = true;
        tool.idempotent = true;
        tool.handler = slice_result;
        registry.add(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "slice_export";
        tool.title = "Export sliced G-code";
        tool.description =
            "Write the sliced result of a plate to a file: kind \"gcode\" (one plate, .gcode, the G-code as sliced; post-processing scripts are "
            "not run) or \"gcode_3mf\" (a sliced project .gcode.3mf with one plate or `plate: \"all\"`). The path must be inside a folder "
            "app_status lists in write_roots; `overwrite` replaces an existing file. Returns {path, bytes, header (the statistics comments of "
            "the G-code: time, filament, layers), summary}. Fails with not_sliced, busy_slicing, path_denied, exists, export_failed. Nothing is "
            "ever sent to a printer.";
        tool.input_schema = Schema::parse(R"JSON({
            "type": "object",
            "properties": {
                "kind": {"enum": ["gcode", "gcode_3mf"], "default": "gcode"},
                "path": {"type": "string", "minLength": 1},
                "plate": {"oneOf": [{"$ref": "#/$defs/PlateRef"}, {"const": "all"}], "description": "Default: current. \"all\" only for gcode_3mf."},
                "overwrite": {"type": "boolean", "default": false}
            },
            "required": ["path"],
            "additionalProperties": false})JSON", "slice_export");
        tool.idempotent = true;
        tool.accepts_if_rev = false;
        tool.handler = slice_export;
        registry.add(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "debug_slice_fixture";
        tool.title = "Slice test fixtures";
        tool.description = "Test only. Scene set-ups for the slice tests: add_plate, delete_plate, move (object to plate, offset from its "
                           "centre), set (preset values), load (a model file), delete_object.";
        tool.input_schema = Schema::parse(R"JSON({
            "type": "object",
            "properties": {
                "action": {"enum": ["add_plate", "delete_plate", "move", "set", "load", "delete_object", "add_filament", "viewer_state"]},
                "object": {"$ref": "#/$defs/Id"},
                "plate": {"$ref": "#/$defs/PlateRef"},
                "offset": {"type": "array", "items": {"type": "number"}},
                "preset": {"enum": ["print", "printer", "filament"]},
                "values": {"type": "object"},
                "path": {"type": "string"}
            },
            "required": ["action"],
            "additionalProperties": false})JSON", "debug_slice_fixture");
        tool.debug_only = true;
        tool.handler = debug_slice_fixture;
        registry.add(std::move(tool));
    }
}

}}} // namespace Slic3r::GUI::Mcp
