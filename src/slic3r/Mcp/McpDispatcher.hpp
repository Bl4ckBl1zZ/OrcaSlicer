#ifndef slic3r_Mcp_Dispatcher_hpp_
#define slic3r_Mcp_Dispatcher_hpp_

// Runs tool calls on the wx main thread: one at a time, never re-entrantly, and only when the
// application is in a state where GUI code may run.
//
//   network thread:  submit(call) -> queue -> CallAfter(pump)
//   main thread:     pump(): pick the first call that may run now, run its handler, complete it
//
// CallAfter and timer events are delivered by whatever event loop is running, including the nested
// loop of a modal dialog and the wxYield inside a progress dialog. pump() therefore starts a call
// only when
//   1. no MCP call is on the stack;
//   2. no event loop is yielding (a function is in the middle of its work);
//   3. the active event loop is the application's main loop - or it is the loop of a modal dialog
//      the user opened, and the tool is read-only;
//   4. for mutating tools: the UI is idle (no user dialog, drag, UI job or project load).
// Otherwise the call waits in the queue and fails with ui_busy when its queue time runs out.

#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <wx/event.h>
#include <wx/timer.h>

#include "McpCall.hpp"
#include "McpDialogGuard.hpp"
#include "McpTool.hpp"

namespace Slic3r { namespace GUI { namespace Mcp {

class Dispatcher : public wxEvtHandler
{
public:
    explicit Dispatcher(DialogGuard& guard);
    ~Dispatcher() override;

    // Any thread. Queues the call and wakes the main thread.
    void submit(std::shared_ptr<Call> call);
    // Main thread. Fails everything queued or deferred; nothing runs afterwards.
    void shutdown();

    // For app_status and tests.
    struct Stats
    {
        size_t calls_run { 0 };
        size_t blocked_reentrant { 0 }; // pump found an MCP call on the stack
        size_t blocked_yielding { 0 };  // pump found an event loop yielding
        size_t blocked_nested { 0 };    // pump found a nested event loop that is not a user dialog
        size_t blocked_busy { 0 };      // a call had to wait for the UI
        std::string last_blocked_reason;
    };
    const Stats& stats() const { return m_stats; }
    bool         in_call() const { return m_depth > 0; }
    // Why a mutating call could not start right now; empty when the UI is idle.
    std::string  ui_busy_reason() const;

private:
    struct Deferred
    {
        std::unique_ptr<ToolContext>          context;
        std::chrono::steady_clock::time_point deadline;
    };
    enum class LoopState { TopLevel, UserDialog, Yielding, Nested, NotReady };

    void        wake();
    void        pump();
    void        on_timer(wxTimerEvent& event);
    LoopState   loop_state() const;
    // Empty when `tool` may start in `state`.
    std::string blocked_reason(const ToolDef& tool, LoopState state) const;
    void        run(const std::shared_ptr<Call>& call);
    void        poll_deferred();
    void        complete(ToolContext& context, json result);
    void        complete_error(ToolContext& context, const ToolError& error);
    void        prepare_ui(ToolContext& context);
    static void flush_pending_events();

    DialogGuard&                      m_guard;
    std::mutex                        m_mutex;
    std::deque<std::shared_ptr<Call>> m_queue; // guarded by m_mutex
    std::vector<Deferred>             m_deferred;
    wxTimer                           m_timer;
    int                               m_depth { 0 };
    bool                              m_shutdown { false };
    Stats                             m_stats;
};

}}} // namespace Slic3r::GUI::Mcp

#endif
