// Core tools: app_status, history, job_status, job_cancel.

#include <algorithm>
#include <string>
#include <vector>

#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/libslic3r.h"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Gizmos/GLGizmosManager.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/Utils/UndoRedo.hpp"

#include "../McpDialogGuard.hpp"
#include "../McpDispatcher.hpp"
#include "../McpFiles.hpp"
#include "../McpJobs.hpp"
#include "../McpJournal.hpp"
#include "../McpScene.hpp"
#include "../McpSchema.hpp"
#include "../McpService.hpp"
#include "../McpTool.hpp"
#include "printers/Printers.hpp"
#ifdef SLIC3R_MCP_TEST_PRINTERS
#include "../testing/FakePrinters.hpp"
#endif

namespace Slic3r { namespace GUI { namespace Mcp {

namespace {

// Names of the undo steps (most recent first) and of the redo steps (next first), as the Edit
// menu lists them.
void undo_redo_names(json& undo, json& redo)
{
    undo = json::array();
    redo = json::array();
    Plater*     plater = wxGetApp().plater();
    const char* text = nullptr;
    for (int i = 0; i < 200 && plater->undo_redo_string_getter(true, i, &text); ++i)
        undo.push_back(text != nullptr ? std::string(text) : std::string());
    for (int i = 0; i < 200 && plater->undo_redo_string_getter(false, i, &text); ++i)
        redo.push_back(text != nullptr ? std::string(text) : std::string());
}

json app_status(ToolContext&)
{
    Plater*  plater = wxGetApp().plater();
    Service& service = Service::instance();

    json out = json::object();
    out["app"] = SLIC3R_APP_NAME;
    out["version"] = SoftFever_VERSION;
    out["technology"] = plater->printer_technology() == ptSLA ? "SLA" : "FFF";
    out["project"] = Scene::project_json();
    out["printer"] = Scene::printer_json();
    out["process"] = Scene::process_json();
    out["filaments"] = Scene::filaments_json();

    json busy = json::object();
    busy["slicing"] = plater->is_background_process_slicing();
    json jobs = json::array();
    for (const std::shared_ptr<Job>& job : Jobs::instance().all())
        if (job->state == Job::State::Running)
            jobs.push_back(job->id);
    busy["jobs"] = std::move(jobs);
    if (DialogGuard* guard = service.dialog_guard(); guard != nullptr && guard->user_dialogs_open() > 0)
        busy["modal_dialog"] = guard->user_dialog_title();
    else
        busy["modal_dialog"] = nullptr;
    if (GLCanvas3D* canvas = plater->get_view3D_canvas3D(); canvas != nullptr && canvas->get_gizmos_manager().get_current_type() != GLGizmosManager::Undefined)
        busy["gizmo"] = get_name_from_gizmo_etype(canvas->get_gizmos_manager().get_current_type());
    else
        busy["gizmo"] = nullptr;
    // What would make a changing call wait right now; null when changes can run.
    const std::string reason = service.dispatcher() != nullptr ? service.dispatcher()->ui_busy_reason() : std::string();
    if (reason.empty())
        busy["blocks_changes"] = nullptr;
    else
        busy["blocks_changes"] = reason;
    out["busy"] = std::move(busy);

    json undo, redo;
    undo_redo_names(undo, redo);
    out["undo"] = undo.empty() ? json(nullptr) : undo.front();
    out["redo"] = redo.empty() ? json(nullptr) : redo.front();

    out["read_roots"] = Files::read_roots();
    out["write_roots"] = Files::write_roots();

    json conventions = json::object();
    conventions["units"] = "mm, degrees, seconds, grams";
    conventions["axes"] = "Z up; the bed is the plane z = 0";
    conventions["positions"] = "objects and instances: plate frame (world minus the plate origin); center = bounding-box centre in XY, z_min = lowest point";
    conventions["volumes"] = "parts and modifiers: object frame; center_rel is 0..1 across the object's bounding box";
    conventions["ids"] = "o<N> object, i<N> instance, v<N> volume, pl<N> plate; plates also by 1-based number; filament slots are 1-based";
    conventions["camera"] = "azimuth 0 looks from the front (-Y towards +Y), counter-clockwise seen from above; elevation 90 looks straight down";
    out["conventions"] = std::move(conventions);

    // Printer access as the user set it in Preferences (no tool can change it).
    json printers = json::object({{"read_allowed", Printers::read_allowed()}, {"send_allowed", Printers::send_allowed()},
                                  {"sends_in_flight", Printers::sends_in_flight()}});
#ifdef SLIC3R_MCP_TEST_PRINTERS
    // MCP: test builds only; never set in a release build.
    if (Testing::fake_printers_active())
        printers["fake_printers"] = true;
#endif
    out["printers"] = std::move(printers);

    json server = json::object();
    server["tools"] = service.registry() != nullptr ? service.registry()->tools().size() : 0;
    server["debug_tools"] = service.debug_tools();
    out["server"] = std::move(server);
    return out;
}

json history(ToolContext& ctx)
{
    Plater*           plater = wxGetApp().plater();
    const std::string action = ctx.arg("action", "list");
    const int         steps = ctx.arg<int>("steps", 1);

    json result = json::object();
    if (action == "undo" || action == "redo") {
        const bool is_undo = action == "undo";
        int        done = 0;
        json       names = json::array();
        for (int i = 0; i < steps; ++i) {
            const char* text = nullptr;
            if (!plater->undo_redo_string_getter(is_undo, 0, &text))
                break;
            const std::string name = text != nullptr ? std::string(text) : std::string();
            const size_t      before = Scene::rev();
            if (is_undo)
                plater->undo();
            else
                plater->redo();
            if (Scene::rev() == before)
                break; // the application refused (a job is running, assemble view ...)
            names.push_back(name);
            ++done;
        }
        if (done == 0)
            fail(is_undo ? "nothing_to_undo" : "nothing_to_redo", is_undo ? "There is nothing to undo." : "There is nothing to redo.",
                 "history with action \"list\" shows the undo and redo steps.");
        result["ok"] = true;
        result[is_undo ? "undone" : "redone"] = std::move(names);
        if (done < steps)
            ctx.note("Only " + std::to_string(done) + " of " + std::to_string(steps) + " steps were available.");
        result["scene"] = Scene::summary_json();
    }

    if (action == "revert") {
        if (!ctx.has("journal_seq"))
            fail("invalid_argument", "action \"revert\" needs `journal_seq`: the sequence numbers of the journal entries to revert.",
                 "history with action \"list\" shows the journal.");
        json reverted = json::array();
        // Newest first, so that several changes of the same value unwind in order.
        std::vector<size_t> seqs = ctx.args()["journal_seq"].get<std::vector<size_t>>();
        std::sort(seqs.begin(), seqs.end(), [](size_t a, size_t b) { return a > b; });
        for (size_t seq : seqs) {
            Journal::instance().revert(seq);
            reverted.push_back(seq);
        }
        result["ok"] = true;
        result["reverted"] = std::move(reverted);
        result["scene"] = Scene::summary_json();
    }

    json undo, redo;
    undo_redo_names(undo, redo);
    result["undo"] = std::move(undo);
    result["redo"] = std::move(redo);
    // Changes outside the undo stack (presets, filaments ...), newest first.
    json         journal = json::array();
    const auto&  entries = Journal::instance().entries();
    const size_t limit = 50;
    for (auto it = entries.rbegin(); it != entries.rend() && journal.size() < limit; ++it)
        journal.push_back(Journal::to_json(*it));
    result["journal"] = std::move(journal);
    result["rev"] = Scene::rev();
    return result;
}

std::shared_ptr<Job> job_from_args(ToolContext& ctx)
{
    std::shared_ptr<Job> job;
    if (ctx.has("job")) {
        const std::string id = ctx.arg("job", "");
        job = Jobs::instance().find(id);
        if (!job)
            fail("stale_id", "There is no job " + id + ".", "Jobs are forgotten some time after they finish. job_status without `job` reports the most recent one.");
    } else {
        job = Jobs::instance().latest(ctx.arg("kind", ""));
    }
    return job;
}

json job_status(ToolContext& ctx)
{
    std::shared_ptr<Job> job = job_from_args(ctx);
    if (!job) {
        json out = json::object();
        out["status"] = "idle";
        out["jobs"] = json::array();
        return out;
    }
    const double wait_s = ctx.wait_s(0., 280.);
    if (job->state == Job::State::Running && wait_s > 0.) {
        ctx.defer(wait_for_job(job, wait_s));
        return nullptr;
    }
    return job_json(*job);
}

json job_cancel(ToolContext& ctx)
{
    std::shared_ptr<Job> job = job_from_args(ctx);
    if (!job || job->state != Job::State::Running)
        fail("not_running", "No job is running.", "job_status shows the most recent job.");
    if (!job->cancel)
        fail("not_cancellable", "The " + job->kind + " job cannot be stopped.", "Wait for it with job_status.");
    job->cancel();
    // The operation stops on its own schedule; report what is known now.
    return job_json(*job);
}

} // namespace

MCP_TOOL_GROUP(Core)
{
    {
        ToolDef tool;
        tool.name = "app_status";
        tool.title = "Application status";
        tool.description =
            "Status of the OrcaSlicer window this server drives: application version, printer (preset, bed size, nozzles, plate type) and process presets in use, project name and "
            "unsaved state, filaments, what is busy right now (slicing, running jobs, an open dialog or gizmo, and whether changes are blocked), "
            "the next undo and redo step, the folders files may be read from and written to, and the coordinate and id conventions. "
            "Call it first in a session, and again when a call fails with ui_busy.";
        tool.read_only = true;
        tool.idempotent = true;
        tool.handler = app_status;
        registry.add(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "history";
        tool.title = "Undo / redo history";
        tool.description =
            "Undo, redo and the change journal. action \"list\" (default) returns the names of the undo steps (most recent first), the redo "
            "steps, and the journal: changes the undo stack does not cover (preset edits, filament list and colours), newest first. Every "
            "change made through this server is one undo step named \"MCP: ...\", mixed with the user's own steps. action \"undo\" / \"redo\" "
            "moves `steps` steps (default 1); action \"revert\" reverts the journal entries in `journal_seq`. Both return a scene summary. "
            "Undo also reverts the user's steps if they are on top: check the names first.";
        tool.input_schema = Schema::parse(R"JSON({
            "type": "object",
            "properties": {
                "action": {"enum": ["list", "undo", "redo", "revert"], "default": "list"},
                "steps": {"type": "integer", "minimum": 1, "maximum": 50, "default": 1, "description": "How many steps to undo or redo."},
                "journal_seq": {"type": "array", "items": {"type": "integer", "minimum": 1}, "minItems": 1,
                                "description": "For action \"revert\": seq numbers of journal entries."}
            },
            "additionalProperties": false})JSON", "history");
        tool.destructive = true;
        tool.handler = history;
        registry.add(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "job_status";
        tool.title = "Status of a long operation";
        tool.description =
            "Progress and result of a long-running operation started by another tool (slice, arrange, orient, mesh operations): status "
            "running | finished | failed | cancelled, percent, status text and, when finished, the result. Without `job` it reports the most "
            "recent job (optionally of one `kind`). With wait_s > 0 the call waits until the job ends or the time is up, so one call replaces a polling loop.";
        tool.input_schema = Schema::parse(R"JSON({
            "type": "object",
            "properties": {
                "job": {"type": "string", "pattern": "^job[0-9]+$", "description": "a job id such as job3, as returned by the tool that started it"},
                "kind": {"type": "string", "description": "Without `job`: only consider jobs of this kind, e.g. \"slice\"."},
                "wait_s": {"type": "number", "minimum": 0, "maximum": 280, "default": 0, "description": "Wait up to this many seconds for the job to end."}
            },
            "additionalProperties": false})JSON", "job_status");
        tool.read_only = true;
        tool.idempotent = true;
        tool.handler = job_status;
        registry.add(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "job_cancel";
        tool.title = "Stop a long operation";
        tool.description =
            "Stop a running long operation (see job_status). Without `job` it stops the most recent running job. The operation stops shortly "
            "after; call job_status to see the final state. Work already applied to the scene is not rolled back; use history to undo.";
        tool.input_schema = Schema::parse(R"JSON({
            "type": "object",
            "properties": {
                "job": {"type": "string", "pattern": "^job[0-9]+$", "description": "a job id such as job3"},
                "kind": {"type": "string"}
            },
            "additionalProperties": false})JSON", "job_cancel");
        tool.destructive = true;
        tool.idempotent = true;
        tool.requires_idle_ui = false;
        tool.accepts_if_rev = false;
        tool.handler = job_cancel;
        registry.add(std::move(tool));
    }
}

}}} // namespace Slic3r::GUI::Mcp
