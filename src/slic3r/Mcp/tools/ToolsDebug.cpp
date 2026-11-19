// Debug tools for the test-suite. Registered only when the server runs with ORCA_MCP_DEBUG_TOOLS=1;
// a normal session never sees them. They exist to provoke the situations the core must handle:
// long jobs, modal dialogs, yields, a blocked main thread, oversized results.

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <wx/app.h>
#include <wx/dialog.h>
#include <wx/scrolwin.h>
#include <wx/window.h>
#include <wx/msgdlg.h>
#include <wx/timer.h>
#include <wx/utils.h>

#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/Plater.hpp"

#include "../McpDispatcher.hpp"
#include "../McpJobs.hpp"
#include "../McpSchema.hpp"
#include "../McpService.hpp"
#include "../McpTool.hpp"

namespace Slic3r { namespace GUI { namespace Mcp {

namespace {

using Clock = std::chrono::steady_clock;

// A job that does nothing for a while: progress from a timer, then finished, failed or cancelled.
class TimerJob : public wxTimer
{
public:
    TimerJob(std::shared_ptr<Job> job, std::shared_ptr<bool> cancel_requested, double duration_s, bool fail_at_end)
        : m_job(std::move(job)), m_cancel_requested(std::move(cancel_requested)), m_duration_s(duration_s), m_fail(fail_at_end), m_started(Clock::now())
    {}

    void Notify() override
    {
        const double elapsed = std::chrono::duration<double>(Clock::now() - m_started).count();
        if (*m_cancel_requested) {
            Jobs::instance().finish(m_job, Job::State::Cancelled);
        } else if (elapsed >= m_duration_s) {
            if (m_fail)
                Jobs::instance().finish(m_job, Job::State::Failed, json::object({{"error", json::object({{"code", "debug_failure"}, {"message", "The debug job was asked to fail."}})}}));
            else
                Jobs::instance().finish(m_job, Job::State::Finished, json::object({{"waited_s", m_duration_s}}));
        } else {
            Jobs::instance().update(m_job, 100. * elapsed / m_duration_s, "debug job working");
            return;
        }
        Stop();
        delete this;
    }

private:
    std::shared_ptr<Job>  m_job;
    std::shared_ptr<bool> m_cancel_requested;
    double                m_duration_s;
    bool                  m_fail;
    Clock::time_point     m_started;
};

// Runs `fn` from the event loop a moment after the current call has ended. (CallAfter would not
// do: the dispatcher processes pending events before a call ends.)
class RunLater : public wxTimer
{
public:
    static void start(std::function<void()> fn) { (new RunLater(std::move(fn)))->StartOnce(150); }

    void Notify() override
    {
        std::function<void()> fn = std::move(m_fn);
        delete this;
        fn();
    }

private:
    explicit RunLater(std::function<void()> fn) : m_fn(std::move(fn)) {}
    std::function<void()> m_fn;
};

json debug_job(ToolContext& ctx)
{
    const double duration_s = ctx.arg<double>("duration_s", 2.);
    auto         cancel_requested = std::make_shared<bool>(false);
    auto         job = Jobs::instance().start("debug", [cancel_requested]() { *cancel_requested = true; });
    (new TimerJob(job, cancel_requested, duration_s, ctx.arg<bool>("fail", false)))->Start(100);
    ctx.defer(wait_for_job(job, ctx.wait_s(10., 280.)));
    return nullptr;
}

DialogAnswer answer_from(const std::string& name)
{
    if (name == "ok") return DialogAnswer::Ok;
    if (name == "cancel") return DialogAnswer::Cancel;
    if (name == "yes") return DialogAnswer::Yes;
    if (name == "no") return DialogAnswer::No;
    if (name == "close") return DialogAnswer::Close;
    return DialogAnswer::Default;
}

std::string id_name(int id)
{
    switch (id) {
    case wxID_OK: return "ok";
    case wxID_CANCEL: return "cancel";
    case wxID_YES: return "yes";
    case wxID_NO: return "no";
    case wxID_CLOSE: return "close";
    default: return std::to_string(id);
    }
}

int show_test_dialog(const std::string& kind, const wxString& text)
{
    wxWindow* parent = wxGetApp().mainframe;
    if (kind == "info")
        return MessageDialog(parent, text, "MCP test info", wxOK | wxICON_INFORMATION).ShowModal();
    if (kind == "error") {
        show_error(parent, text);
        return wxID_OK;
    }
    if (kind == "native")
        return wxMessageDialog(parent, text, "MCP test native", wxYES_NO | wxCANCEL | wxICON_QUESTION).ShowModal();
    return MessageDialog(parent, text, "MCP test question", wxYES_NO | wxCANCEL | wxICON_QUESTION).ShowModal();
}

// Opens a dialog from inside the call, the way GUI code called by a tool would.
json debug_dialog(ToolContext& ctx)
{
    const std::string kind = ctx.arg("kind", "question");
    const wxString    text = wxString::FromUTF8(ctx.arg("text", "Convert the model?\nSecond line."));
    if (ctx.has("answer")) {
        DialogRule rule;
        rule.title_contains = "MCP test";
        rule.answer = answer_from(ctx.arg("answer", "default"));
        rule.informational = ctx.arg<bool>("informational", true);
        ctx.on_dialog(rule);
    }
    if (ctx.arg<bool>("snapshot", false))
        ctx.snapshot("debug dialog");
    const int returned = show_test_dialog(kind, text);
    json out = json::object();
    out["returned"] = id_name(returned);
    return out;
}

// Puts the window into a state the dispatcher must respect, from outside any tool call, and
// returns at once.
json debug_ui(ToolContext& ctx)
{
    const std::string action = ctx.arg("action", "yield");
    const int         ms = ctx.arg<int>("ms", 1000);
    if (action == "yield") {
        // A function in the middle of its work that keeps the UI alive with wxYield, like an
        // import with a progress dialog.
        RunLater::start([ms]() {
            const auto end = Clock::now() + std::chrono::milliseconds(ms);
            while (Clock::now() < end) {
                wxYield();
                wxMilliSleep(5);
            }
        });
    } else {
        // A modal dialog the user opened; closes itself after `ms`.
        const std::string kind = action == "native_modal" ? "native" : "question";
        RunLater::start([ms, kind]() {
            if (kind == "native") {
                wxMessageDialog(wxGetApp().mainframe, "A native dialog of the user's. Close it by hand.", "MCP test user dialog", wxOK).ShowModal();
                return;
            }
            MessageDialog dialog(wxGetApp().mainframe, "A dialog of the user's.", "MCP test user dialog", wxOK);
            wxTimer       closer;
            closer.Bind(wxEVT_TIMER, [&dialog](wxTimerEvent&) {
                if (dialog.IsModal())
                    dialog.EndModal(wxID_OK);
            });
            closer.StartOnce(ms);
            dialog.ShowModal();
        });
    }
    json out = json::object();
    out["scheduled"] = action;
    out["ms"] = ms;
    return out;
}

json debug_sleep(ToolContext& ctx)
{
    wxMilliSleep(static_cast<unsigned long>(ctx.arg<int>("ms", 1000)));
    json out = json::object();
    out["slept_ms"] = ctx.arg<int>("ms", 1000);
    return out;
}

json debug_echo(ToolContext& ctx)
{
    json out = json::object();
    if (ctx.has("payload"))
        out["payload"] = ctx.args()["payload"];
    if (const int size = ctx.arg<int>("size", 0); size > 0)
        out["filler"] = std::string(static_cast<size_t>(size), 'x');
    if (ctx.arg<bool>("image", false)) {
        // A 1x1 red PNG.
        static const unsigned char png[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00,
                                            0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x77, 0x53, 0xDE, 0x00, 0x00, 0x00, 0x0C, 0x49,
                                            0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0xF8, 0xCF, 0xC0, 0x00, 0x00, 0x03, 0x01, 0x01, 0x00, 0xC9, 0xFE, 0x92, 0xEF,
                                            0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};
        ctx.add_image("image/png", std::string(reinterpret_cast<const char*>(png), sizeof(png)));
    }
    if (ctx.has("note"))
        ctx.note(ctx.arg("note", ""));
    return out;
}

json debug_stats(ToolContext&)
{
    json out = json::object();
    if (Dispatcher* dispatcher = Service::instance().dispatcher(); dispatcher != nullptr) {
        const Dispatcher::Stats& stats = dispatcher->stats();
        out["calls_run"] = stats.calls_run;
        out["blocked_reentrant"] = stats.blocked_reentrant;
        out["blocked_yielding"] = stats.blocked_yielding;
        out["blocked_nested"] = stats.blocked_nested;
        out["blocked_busy"] = stats.blocked_busy;
        out["last_blocked_reason"] = stats.last_blocked_reason;
    }
    return out;
}

// A mutating tool that changes nothing in the project: for the UI-busy rules, the undo wrapper
// and the journal (it keeps one string of its own to journal and revert).
std::string s_debug_value = "initial";

json debug_touch(ToolContext& ctx)
{
    if (ctx.arg<bool>("snapshot", false))
        ctx.snapshot("debug touch");
    if (ctx.has("set_value")) {
        const std::string before = s_debug_value;
        s_debug_value = ctx.arg("set_value", "");
        ctx.journal("debug", "value", before, s_debug_value, [before]() { s_debug_value = before; });
    }
    if (ctx.arg<bool>("yield_inside", false)) {
        // An event loop turn inside the handler: queued calls must not start here.
        const auto end = Clock::now() + std::chrono::milliseconds(ctx.arg<int>("ms", 500));
        while (Clock::now() < end) {
            wxYield();
            wxMilliSleep(5);
        }
    }
    json out = json::object();
    out["ok"] = true;
    out["value"] = s_debug_value;
    return out;
}

// The window as the user has it: minimised or not, which tab, and the user's camera. Lets a test
// check that offscreen rendering works in every window state and leaves the user's view alone.
json debug_window(ToolContext& ctx)
{
    MainFrame*        frame = wxGetApp().mainframe;
    Plater*           plater = wxGetApp().plater();
    const std::string action = ctx.arg("action", "state");
    if (action == "minimize")
        frame->Iconize(true);
    else if (action == "restore")
        frame->Iconize(false);
    else if (action == "preview")
        frame->select_tab(wxString(TAB_ID_PREVIEW));
    else if (action == "prepare")
        frame->select_tab(wxString(TAB_ID_PREPARE));
    else if (action == "preferences")
        // As the user would: a modal dialog opened from the event loop, outside any call.
        RunLater::start([]() { wxGetApp().open_preferences(); });
    else if (action == "scroll_dialog" || action == "close_dialogs") {
        for (wxWindow* window : wxTopLevelWindows) {
            auto* dialog = dynamic_cast<wxDialog*>(window);
            if (dialog == nullptr || !dialog->IsModal())
                continue;
            if (action == "close_dialogs") {
                dialog->EndModal(wxID_CANCEL);
                continue;
            }
            // Bring the end of the dialog's scrolled page into view (the MCP section is last).
            std::vector<wxWindow*> pending { dialog };
            while (!pending.empty()) {
                wxWindow* current = pending.back();
                pending.pop_back();
                if (auto* scrolled = dynamic_cast<wxScrolledWindow*>(current)) {
                    scrolled->Scroll(0, 100000);
                    break;
                }
                for (wxWindow* child : current->GetChildren())
                    pending.push_back(child);
            }
        }
    }

    json out = json::object();
    out["minimized"] = frame->IsIconized();
    out["shown"] = frame->IsShown();
    out["active"] = frame->IsActive();
    out["preview_tab"] = plater->is_preview_shown();
    const Camera& camera = plater->get_camera();
    json          matrix = json::array();
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            matrix.push_back(camera.get_view_matrix().matrix()(r, c));
    out["user_camera"] = json::object({{"view_matrix", std::move(matrix)}, {"zoom", camera.get_zoom()}});
    return out;
}

} // namespace

MCP_TOOL_GROUP(Debug)
{
    {
        ToolDef tool;
        tool.name = "debug_window";
        tool.description = "Test only. Reports the window state and the user's camera; can minimise or restore the window and switch between the Prepare and Preview tabs.";
        tool.input_schema = Schema::parse(R"JSON({"type": "object", "properties": {
            "action": {"enum": ["state", "minimize", "restore", "preview", "prepare", "preferences", "scroll_dialog", "close_dialogs"], "default": "state"}},
            "additionalProperties": false})JSON", "debug_window");
        tool.requires_idle_ui = false;
        tool.accepts_if_rev = false;
        tool.debug_only = true;
        tool.handler = debug_window;
        registry.add(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "debug_job";
        tool.description = "Test only. Starts a job that finishes (or fails) after duration_s and waits for it up to wait_s.";
        tool.input_schema = Schema::parse(R"JSON({"type": "object", "properties": {
            "duration_s": {"type": "number", "minimum": 0, "maximum": 120, "default": 2},
            "fail": {"type": "boolean", "default": false},
            "wait_s": {"type": "number", "minimum": 0, "maximum": 280, "default": 10}},
            "additionalProperties": false})JSON", "debug_job");
        tool.requires_idle_ui = false;
        tool.accepts_if_rev = false;
        tool.debug_only = true;
        tool.handler = debug_job;
        registry.add(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "debug_dialog";
        tool.description = "Test only. Opens a modal dialog from inside the call and reports what ShowModal returned.";
        tool.input_schema = Schema::parse(R"JSON({"type": "object", "properties": {
            "kind": {"enum": ["question", "info", "error", "native"], "default": "question"},
            "text": {"type": "string"},
            "answer": {"enum": ["default", "ok", "cancel", "yes", "no", "close"]},
            "informational": {"type": "boolean", "default": true},
            "snapshot": {"type": "boolean", "default": false}},
            "additionalProperties": false})JSON", "debug_dialog");
        tool.debug_only = true;
        tool.handler = debug_dialog;
        registry.add(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "debug_ui";
        tool.description = "Test only. Schedules a UI state outside any call: a yielding loop or a user modal dialog lasting ms.";
        tool.input_schema = Schema::parse(R"JSON({"type": "object", "properties": {
            "action": {"enum": ["yield", "modal", "native_modal"], "default": "yield"},
            "ms": {"type": "integer", "minimum": 0, "maximum": 60000, "default": 1000}},
            "additionalProperties": false})JSON", "debug_ui");
        tool.requires_idle_ui = false;
        tool.accepts_if_rev = false;
        tool.debug_only = true;
        tool.handler = debug_ui;
        registry.add(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "debug_sleep";
        tool.description = "Test only. Blocks the main thread for ms. Its run timeout is 1.5 s.";
        tool.input_schema = Schema::parse(R"JSON({"type": "object", "properties": {
            "ms": {"type": "integer", "minimum": 0, "maximum": 60000, "default": 1000}},
            "additionalProperties": false})JSON", "debug_sleep");
        tool.read_only = true;
        tool.run_timeout_ms = 1500;
        tool.debug_only = true;
        tool.handler = debug_sleep;
        registry.add(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "debug_echo";
        tool.description = "Test only. Returns its payload, optionally with a filler string of `size` characters, a 1x1 PNG and a note. Its queue timeout is 0.5 s.";
        tool.input_schema = Schema::parse(R"JSON({"type": "object", "properties": {
            "payload": {},
            "size": {"type": "integer", "minimum": 0, "maximum": 10000000},
            "image": {"type": "boolean", "default": false},
            "note": {"type": "string"}},
            "additionalProperties": false})JSON", "debug_echo");
        tool.read_only = true;
        tool.idempotent = true;
        tool.queue_timeout_ms = 500;
        tool.debug_only = true;
        tool.handler = debug_echo;
        registry.add(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "debug_stats";
        tool.description = "Test only. Dispatcher counters: calls run, and how often a call had to wait because of re-entrancy, a yield, a nested loop or a busy UI.";
        tool.read_only = true;
        tool.idempotent = true;
        tool.debug_only = true;
        tool.handler = debug_stats;
        registry.add(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "debug_touch";
        tool.description = "Test only. A mutating tool that changes nothing; optionally takes an undo snapshot or yields inside the handler. Its queue timeout is 1.5 s.";
        tool.input_schema = Schema::parse(R"JSON({"type": "object", "properties": {
            "snapshot": {"type": "boolean", "default": false},
            "set_value": {"type": "string"},
            "yield_inside": {"type": "boolean", "default": false},
            "ms": {"type": "integer", "minimum": 0, "maximum": 60000, "default": 500}},
            "additionalProperties": false})JSON", "debug_touch");
        tool.queue_timeout_ms = 1500;
        tool.debug_only = true;
        tool.handler = debug_touch;
        registry.add(std::move(tool));
    }
}

}}} // namespace Slic3r::GUI::Mcp
