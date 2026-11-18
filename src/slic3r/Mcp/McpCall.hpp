#ifndef slic3r_Mcp_Call_hpp_
#define slic3r_Mcp_Call_hpp_

// One tools/call in flight. Created on the network thread, executed on the main thread, completed
// from either. The only object both threads share; everything in it is either immutable after
// creation or atomic / mutex-guarded.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "McpError.hpp"
#include "McpJson.hpp"
#include "McpTool.hpp"

namespace Slic3r { namespace GUI { namespace Mcp {

struct CallResult
{
    std::string        text;     // the JSON document returned as the text block
    std::vector<Image> images;
    bool               is_error { false };
    std::string        outcome;  // "ok" or the error code, for the activity log
};

inline CallResult make_error_result(const ToolError& error)
{
    CallResult result;
    result.text = error.to_json().dump(-1, ' ', false, json::error_handler_t::replace);
    result.is_error = true;
    result.outcome = error.code();
    return result;
}

class Call
{
public:
    using Clock = std::chrono::steady_clock;

    Call(const ToolDef& tool, json args) : m_tool(tool), m_args(std::move(args)), m_created(Clock::now())
    {
        set_hard_deadline(m_created + std::chrono::milliseconds(tool.queue_timeout_ms + 3000));
    }

    const ToolDef&    tool() const { return m_tool; }
    const json&       args() const { return m_args; }
    Clock::time_point created() const { return m_created; }

    // Set by the transport before the call is queued; invoked at most once, from any thread.
    std::function<void(CallResult)>                 on_complete;
    // Set by the transport when the client asked for progress; invoked from the main thread.
    std::function<void(double, const std::string&)> on_progress;

    // First caller wins; later calls are ignored. Returns whether this call delivered the result.
    bool complete(CallResult result)
    {
        if (m_finished.exchange(true))
            return false;
        if (on_complete)
            on_complete(std::move(result));
        return true;
    }
    bool finished() const { return m_finished.load(); }

    void cancel() { m_cancelled.store(true); }
    bool cancelled() const { return m_cancelled.load(); }

    void mark_started() { m_started.store(true); }
    bool started() const { return m_started.load(); }

    // Moment after which the network thread answers on its own because the main thread did not:
    // set at creation (queue time), at start (run time) and on deferral (wait time).
    void set_hard_deadline(Clock::time_point when)
    {
        m_hard_deadline.store(std::chrono::duration_cast<std::chrono::milliseconds>(when.time_since_epoch()).count());
    }
    bool hard_deadline_passed(Clock::time_point now) const
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() > m_hard_deadline.load();
    }

    // Why the dispatcher has not started the call yet ("a modal dialog is open: Preferences").
    void set_blocked_reason(const std::string& reason)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_blocked_reason = reason;
    }
    std::string blocked_reason() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_blocked_reason;
    }

private:
    const ToolDef&          m_tool;
    const json              m_args;
    const Clock::time_point m_created;
    std::atomic<bool>       m_finished { false };
    std::atomic<bool>       m_cancelled { false };
    std::atomic<bool>       m_started { false };
    std::atomic<int64_t>    m_hard_deadline { 0 };
    mutable std::mutex      m_mutex;
    std::string             m_blocked_reason;
};

}}} // namespace Slic3r::GUI::Mcp

#endif
