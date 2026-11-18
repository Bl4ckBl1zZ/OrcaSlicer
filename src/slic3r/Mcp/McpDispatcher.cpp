#include "McpDispatcher.hpp"

#include <algorithm>
#include <exception>
#include <utility>

#include <boost/log/trivial.hpp>

#include <wx/app.h>
#include <wx/evtloop.h>

#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Gizmos/GLGizmosManager.hpp"
#include "slic3r/GUI/Jobs/Worker.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/Utils/UndoRedo.hpp"

#include "McpJournal.hpp"
#include "McpUtil.hpp"

namespace Slic3r { namespace GUI { namespace Mcp {

namespace {

using Clock = std::chrono::steady_clock;

const int    PUMP_RETRY_MS = 50;       // queue not runnable yet
const int    POLL_INTERVAL_MS = 100;   // deferred calls
const size_t MAX_RESULT_CHARS = 400000; // hard stop; tools page their output far below this

size_t scene_rev()
{
    Plater* plater = wxGetApp().plater();
    return plater != nullptr ? plater->undo_redo_stack_main().active_snapshot_time() : 0;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// ToolContext
// ---------------------------------------------------------------------------------------------

struct ToolContext::SnapshotHolder
{
    SnapshotHolder(Plater* plater, const std::string& name) : snapshot(plater, name) {}
    Plater::TakeSnapshot snapshot;
};

ToolContext::ToolContext(std::shared_ptr<Call> call) : m_call(std::move(call)) {}
ToolContext::~ToolContext() = default;

const json&    ToolContext::args() const { return m_call->args(); }
bool           ToolContext::has(const char* key) const { return m_call->args().contains(key) && !m_call->args()[key].is_null(); }
const ToolDef& ToolContext::tool() const { return m_call->tool(); }

void ToolContext::note(std::string text) { m_notes.push_back(std::move(text)); }

void ToolContext::add_image(std::string mime_type, std::string bytes)
{
    m_images.push_back(Image { std::move(mime_type), std::move(bytes) });
}

void ToolContext::snapshot(const std::string& summary)
{
    if (m_snapshot)
        return;
    Plater* plater = wxGetApp().plater();
    if (plater == nullptr)
        fail("internal", "The application is not ready.");
    m_snapshot_name = "MCP: " + summary;
    m_snapshot = std::make_unique<SnapshotHolder>(plater, m_snapshot_name);
}

size_t ToolContext::journal(json scope, std::string key, json before, json after, std::function<void()> revert)
{
    const size_t seq = Journal::instance().add(tool().name, std::move(scope), std::move(key), std::move(before), std::move(after), std::move(revert));
    m_journal.push_back(seq);
    return seq;
}

bool ToolContext::snapshot_taken() const { return m_snapshot != nullptr || !m_snapshot_name.empty(); }

void ToolContext::on_dialog(DialogRule rule) { m_dialog_rules.push_back(std::move(rule)); }

const std::vector<DialogRecord>& ToolContext::dialogs() const { return m_dialogs; }

void ToolContext::accept_dialogs()
{
    for (DialogRecord& record : m_dialogs)
        record.blocked = false;
}

void ToolContext::progress(double percent, const std::string& text)
{
    if (m_call->on_progress)
        m_call->on_progress(percent, text);
}

bool ToolContext::cancelled() const { return m_call->cancelled(); }

void ToolContext::defer(Pending pending)
{
    if (!pending.poll)
        fail("internal", "A deferred call needs a poll function.");
    m_pending = std::make_unique<Pending>(std::move(pending));
}

double ToolContext::wait_s(double default_s, double max_s) const
{
    const double value = arg<double>("wait_s", default_s);
    return std::max(0., std::min(max_s, value));
}

// ---------------------------------------------------------------------------------------------
// Dispatcher
// ---------------------------------------------------------------------------------------------

Dispatcher::Dispatcher(DialogGuard& guard) : m_guard(guard), m_timer(this)
{
    Bind(wxEVT_TIMER, &Dispatcher::on_timer, this, m_timer.GetId());
}

Dispatcher::~Dispatcher()
{
    shutdown();
}

void Dispatcher::submit(std::shared_ptr<Call> call)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.push_back(std::move(call));
    }
    wake();
}

void Dispatcher::wake()
{
    // CallAfter is thread-safe: it queues an event for the main thread and wakes its loop.
    CallAfter([this]() { pump(); });
}

void Dispatcher::shutdown()
{
    if (m_shutdown)
        return;
    m_shutdown = true;
    m_timer.Stop();
    std::deque<std::shared_ptr<Call>> queue;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        queue.swap(m_queue);
    }
    const ToolError closing("cancelled", "OrcaSlicer is closing or the MCP server was switched off.");
    for (const std::shared_ptr<Call>& call : queue)
        call->complete(make_error_result(closing));
    for (Deferred& deferred : m_deferred)
        deferred.context->m_call->complete(make_error_result(closing));
    m_deferred.clear();
}

void Dispatcher::on_timer(wxTimerEvent&)
{
    pump();
}

Dispatcher::LoopState Dispatcher::loop_state() const
{
    if (wxTheApp == nullptr || wxGetApp().is_closing() || !wxGetApp().initialized() || wxGetApp().plater() == nullptr || wxGetApp().mainframe == nullptr)
        return LoopState::NotReady;
    wxEventLoopBase* main_loop = wxTheApp->GetMainLoop();
    wxEventLoopBase* active = wxEventLoopBase::GetActive();
    if (main_loop == nullptr || active == nullptr)
        return LoopState::NotReady;
    // A yield means some function is half-way through its work (a progress dialog, a long import).
    if (active->IsYielding() || main_loop->IsYielding())
        return LoopState::Yielding;
    if (active != main_loop)
        // A nested loop we know about is a modal dialog of the user's; anything else is unknown
        // territory (a popup menu loop, a drag-and-drop loop) and nothing runs.
        return m_guard.user_dialogs_open() > 0 ? LoopState::UserDialog : LoopState::Nested;
    // Native message boxes run their own loop without telling wx; the guard still saw them open.
    return m_guard.user_dialogs_open() > 0 ? LoopState::UserDialog : LoopState::TopLevel;
}

std::string Dispatcher::ui_busy_reason() const
{
    if (m_guard.user_dialogs_open() > 0)
        return "a dialog is open in OrcaSlicer: " + m_guard.user_dialog_title();
    Plater* plater = wxGetApp().plater();
    if (plater == nullptr)
        return "OrcaSlicer is starting";
    if (plater->is_loading_project())
        return "a project is loading";
    if (!plater->get_ui_job_worker().is_idle())
        return "an arrange, orient or similar job is running";
    if (GLCanvas3D* canvas = plater->get_view3D_canvas3D(); canvas != nullptr && canvas->is_dragging())
        return "the user is dragging in the 3D view";
    return {};
}

std::string Dispatcher::blocked_reason(const ToolDef& tool, LoopState state) const
{
    switch (state) {
    case LoopState::NotReady: return "OrcaSlicer is starting or closing";
    case LoopState::Yielding: return "OrcaSlicer is in the middle of an operation";
    case LoopState::Nested: return "a menu or drag operation is in progress";
    case LoopState::UserDialog:
        // Looking is allowed while a dialog is up; changing things under it is not.
        if (tool.read_only)
            return {};
        return "a dialog is open in OrcaSlicer: " + m_guard.user_dialog_title();
    case LoopState::TopLevel: break;
    }
    if (!tool.read_only && tool.requires_idle_ui)
        return ui_busy_reason();
    return {};
}

void Dispatcher::pump()
{
    if (m_shutdown)
        return;

    bool retry = false;
    if (m_depth > 0) {
        // Delivered by an event loop running inside a tool call (a dialog we answered would not
        // spin one, but a yield in the called GUI function does). Try again later.
        ++m_stats.blocked_reentrant;
        retry = true;
    } else {
        const LoopState state = loop_state();
        if (state == LoopState::Yielding)
            ++m_stats.blocked_yielding;
        else if (state == LoopState::Nested)
            ++m_stats.blocked_nested;

        // First call in the queue that may run now. Calls that cannot run stay queued until their
        // queue time is up.
        std::shared_ptr<Call> next;
        std::vector<std::pair<std::shared_ptr<Call>, std::string>> expired;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const auto                  now = Clock::now();
            for (auto it = m_queue.begin(); it != m_queue.end();) {
                const std::shared_ptr<Call>& call = *it;
                if (call->finished() || call->cancelled()) {
                    it = m_queue.erase(it);
                    continue;
                }
                const std::string reason = blocked_reason(call->tool(), state);
                if (reason.empty()) {
                    next = call;
                    m_queue.erase(it);
                    break;
                }
                call->set_blocked_reason(reason);
                m_stats.last_blocked_reason = reason;
                if (now - call->created() > std::chrono::milliseconds(call->tool().queue_timeout_ms)) {
                    expired.emplace_back(call, reason);
                    it = m_queue.erase(it);
                    continue;
                }
                ++m_stats.blocked_busy;
                ++it;
            }
            retry = !m_queue.empty();
        }

        for (const auto& [call, reason] : expired)
            call->complete(make_error_result(ToolError("ui_busy", "OrcaSlicer is busy: " + reason + ".",
                                                         "Retry shortly. app_status shows what is busy.", json::object({{"reason", reason}}))));

        if (next) {
            run(next);
            // One call per event: let the window repaint and the user's input through in between.
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_queue.empty())
                wake();
        }

        if (!m_deferred.empty() && state != LoopState::Yielding && state != LoopState::NotReady && state != LoopState::Nested)
            poll_deferred();
    }

    if (retry || !m_deferred.empty()) {
        if (!m_timer.IsRunning())
            m_timer.StartOnce(m_deferred.empty() ? PUMP_RETRY_MS : std::min(PUMP_RETRY_MS, POLL_INTERVAL_MS));
    }
}

void Dispatcher::prepare_ui(ToolContext& context)
{
    const ToolDef& tool = context.tool();
    if (tool.read_only)
        return;

    if (tool.accepts_if_rev && context.has("if_rev")) {
        const size_t expected = context.arg<size_t>("if_rev", 0);
        const size_t actual = scene_rev();
        if (expected != actual)
            fail("conflict", "The scene changed since revision " + std::to_string(expected) + " (it is now " + std::to_string(actual) + ").",
                 "Read the scene again (scene_get) and repeat the call with the new rev, or without if_rev.",
                 json::object({{"rev", actual}, {"if_rev", expected}}));
    }

    if (!tool.requires_idle_ui)
        return;
    // A gizmo with its own editing state (paint, cut, emboss ...) caches model data; close it so it
    // cannot write stale data back. Strokes already made were committed on mouse-up.
    Plater* plater = wxGetApp().plater();
    if (GLCanvas3D* canvas = plater != nullptr ? plater->get_view3D_canvas3D() : nullptr; canvas != nullptr) {
        const GLGizmosManager::EType type = canvas->get_gizmos_manager().get_current_type();
        if (type != GLGizmosManager::Undefined && type > GLGizmosManager::Scale) {
            const std::string name = get_name_from_gizmo_etype(type);
            canvas->exit_gizmo();
            context.note("Closed the open " + (name.empty() ? std::string("gizmo") : name + " gizmo") + " before making the change.");
        }
    }
}

void Dispatcher::flush_pending_events()
{
    if (wxTheApp != nullptr && wxTheApp->HasPendingEvents())
        wxTheApp->ProcessPendingEvents();
}

void Dispatcher::run(const std::shared_ptr<Call>& call)
{
    const ToolDef& tool = call->tool();
    call->mark_started();
    call->set_hard_deadline(Clock::now() + std::chrono::milliseconds(tool.run_timeout_ms));
    ++m_stats.calls_run;

    auto context = std::make_unique<ToolContext>(call);
    ++m_depth;
    m_guard.begin_call(&context->m_dialog_rules, &tool.dialog_rules, &context->m_dialogs);
    try {
        prepare_ui(*context);
        json result = tool.handler(*context);
        // GUI code often defers its dialogs and refreshes with CallAfter (show_error() does). Run
        // what the handler queued now, while the call still owns the dialog guard and the undo
        // snapshot; otherwise an error box would pop up on screen after the call has returned.
        // Our own queued pump events are harmless here: they see the call on the stack and retry.
        flush_pending_events();
        // The snapshot scope ends with the handler: GUI code that runs later takes its own.
        context->m_snapshot.reset();
        m_guard.end_call();
        if (context->m_pending) {
            const double wait_s = std::max(0., context->m_pending->wait_s);
            const auto   deadline = Clock::now() + std::chrono::milliseconds(static_cast<long long>(wait_s * 1000.));
            call->set_hard_deadline(deadline + std::chrono::seconds(20));
            m_deferred.push_back(Deferred { std::move(context), deadline });
        } else
            complete(*context, std::move(result));
    } catch (const ToolError& error) {
        context->m_snapshot.reset();
        m_guard.end_call();
        complete_error(*context, error);
    } catch (const std::exception& e) {
        context->m_snapshot.reset();
        m_guard.end_call();
        BOOST_LOG_TRIVIAL(error) << "MCP: tool " << tool.name << " threw: " << e.what();
        complete_error(*context, ToolError("internal", std::string("The tool failed unexpectedly: ") + e.what(),
                                           "This is a bug in the MCP server. The OrcaSlicer log has the details."));
    } catch (...) {
        context->m_snapshot.reset();
        m_guard.end_call();
        BOOST_LOG_TRIVIAL(error) << "MCP: tool " << tool.name << " threw an unknown exception";
        complete_error(*context, ToolError("internal", "The tool failed unexpectedly.", "This is a bug in the MCP server. The OrcaSlicer log has the details."));
    }
    --m_depth;
}

void Dispatcher::poll_deferred()
{
    // Index loop: a poll callback may start another deferred call only through a new tool call,
    // which cannot happen while m_depth > 0, so the vector is stable during the loop body.
    for (size_t i = 0; i < m_deferred.size();) {
        Deferred&    deferred = m_deferred[i];
        ToolContext& context = *deferred.context;
        Call&        call = *context.m_call;
        bool         done = false;

        ++m_depth;
        m_guard.begin_call(&context.m_dialog_rules, &call.tool().dialog_rules, &context.m_dialogs);
        try {
            if (call.finished()) {
                done = true; // answered by the network thread (hard deadline) or the client is gone
            } else if (call.cancelled()) {
                if (context.m_pending->on_cancel)
                    context.m_pending->on_cancel(context);
                m_guard.end_call();
                complete_error(context, ToolError("cancelled", "The client stopped waiting for this call."));
                done = true;
            } else {
                json result;
                if (context.m_pending->poll(context, result)) {
                    m_guard.end_call();
                    complete(context, std::move(result));
                    done = true;
                } else if (Clock::now() >= deferred.deadline) {
                    if (!context.m_pending->on_deadline)
                        fail("timeout", "The operation did not finish within the time the call was asked to wait.");
                    result = context.m_pending->on_deadline(context);
                    m_guard.end_call();
                    complete(context, std::move(result));
                    done = true;
                }
            }
        } catch (const ToolError& error) {
            m_guard.end_call();
            complete_error(context, error);
            done = true;
        } catch (const std::exception& e) {
            m_guard.end_call();
            BOOST_LOG_TRIVIAL(error) << "MCP: deferred tool " << call.tool().name << " threw: " << e.what();
            complete_error(context, ToolError("internal", std::string("The tool failed unexpectedly: ") + e.what()));
            done = true;
        }
        m_guard.end_call();
        --m_depth;

        if (done)
            m_deferred.erase(m_deferred.begin() + static_cast<std::ptrdiff_t>(i));
        else
            ++i;
    }
}

void Dispatcher::complete(ToolContext& context, json result)
{
    const ToolDef& tool = context.tool();

    // A dialog nobody answered on purpose means the application asked something the call did not
    // cover: report it instead of pretending the call went through.
    json blocked = json::array();
    json dialogs = json::array();
    for (const DialogRecord& record : context.m_dialogs) {
        dialogs.push_back(to_json(record));
        if (record.blocked)
            blocked.push_back(to_json(record));
    }
    if (!blocked.empty()) {
        std::string first = blocked.front().value("message", std::string());
        if (first.size() > 300)
            first = first.substr(0, 300) + "...";
        ToolError error("dialog_blocked",
                        "OrcaSlicer raised a dialog this call did not expect: \"" + blocked.front().value("title", std::string()) + "\": " + first,
                        "The dialog was dismissed with its non-destructive button. Check the state with scene_get; if the tool has an argument for this question, repeat the call with it.",
                        json::object({{"dialogs", dialogs}}));
        complete_error(context, error);
        return;
    }

    if (!result.is_object()) {
        json wrapped = json::object();
        wrapped["result"] = std::move(result);
        result = std::move(wrapped);
    }
    if (!tool.read_only) {
        result["rev"] = scene_rev();
        if (!context.m_snapshot_name.empty())
            result["undo"] = context.m_snapshot_name;
        if (!context.m_journal.empty())
            result["journal"] = context.m_journal;
        if (context.m_slice_invalidated)
            result["slice_invalidated"] = true;
    }
    if (!dialogs.empty())
        result["dialogs"] = std::move(dialogs);
    if (!context.m_notes.empty())
        result["notes"] = context.m_notes;

    CallResult out;
    out.text = dump_json(result);
    if (out.text.size() > MAX_RESULT_CHARS) {
        complete_error(context, ToolError("too_large", "The result has " + std::to_string(out.text.size()) + " characters, more than a client can take.",
                                          "Narrow the query: use limit/offset, fewer ids, or a lower detail level."));
        return;
    }
    out.images = std::move(context.m_images);
    out.outcome = "ok";
    BOOST_LOG_TRIVIAL(debug) << "MCP: " << tool.name << " ok, " << out.text.size() << " chars, " << out.images.size() << " image(s)";
    context.m_call->complete(std::move(out));
}

void Dispatcher::complete_error(ToolContext& context, const ToolError& error)
{
    json body = error.to_json();
    if (!context.m_dialogs.empty() && error.code() != "dialog_blocked") {
        json dialogs = json::array();
        for (const DialogRecord& record : context.m_dialogs)
            dialogs.push_back(to_json(record));
        body["dialogs"] = std::move(dialogs);
    }
    if (!context.m_notes.empty())
        body["notes"] = context.m_notes;
    if (!context.m_snapshot_name.empty()) {
        // The undo step exists even though the call failed part-way.
        body["undo"] = context.m_snapshot_name;
        body["rev"] = scene_rev();
    }
    if (!context.m_journal.empty())
        body["journal"] = context.m_journal; // changes made before the call failed
    CallResult out;
    out.text = dump_json(body);
    out.is_error = true;
    out.outcome = error.code();
    BOOST_LOG_TRIVIAL(info) << "MCP: " << context.tool().name << " failed: " << error.code() << ": " << error.message();
    context.m_call->complete(std::move(out));
}

}}} // namespace Slic3r::GUI::Mcp
