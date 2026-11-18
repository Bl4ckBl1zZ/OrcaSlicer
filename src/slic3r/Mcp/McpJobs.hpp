#ifndef slic3r_Mcp_Jobs_hpp_
#define slic3r_Mcp_Jobs_hpp_

// Long-running operations started by tools: a slice, an arrange, a worker-thread computation.
//
// The tool that starts one creates a Job, wires the application's own progress and completion
// events to Jobs::update() / Jobs::finish(), and returns through wait_for_job(): the request stays
// open for `wait_s` seconds and answers with the finished job, or with status "running" and the
// job id. The agent then long-polls with the job_status tool (or a group's own status tool built on
// wait_for_job) and may stop it with job_cancel.
//
// Main thread only. Nothing here blocks or spins an event loop.

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "McpDialogGuard.hpp"
#include "McpJson.hpp"
#include "McpTool.hpp"

namespace Slic3r { namespace GUI { namespace Mcp {

struct Job
{
    enum class State { Running, Finished, Failed, Cancelled };

    std::string id;   // "job<N>"
    std::string kind; // "slice", "arrange", ... chosen by the group
    State       state { State::Running };
    double      percent { 0. };
    std::string text; // current status line
    // What the operation produced (state Finished), or {"error": {...}} details (Failed). Returned
    // as "result" by job_status. May be filled lazily by `collect`.
    json        result;
    // Called once by job_status when the job has finished and `result` is null: lets a group build
    // the result from application state at the time it is asked for.
    std::function<json(const Job&)> collect;
    // Asks the operation to stop. Empty when it cannot be stopped. Must not block.
    std::function<void()>           cancel;
    // Non-null when dialogs that open while the job runs should be answered from these rules.
    std::shared_ptr<DialogScope>    dialogs;

    std::chrono::steady_clock::time_point started;
    std::chrono::steady_clock::time_point ended;
};

class Jobs
{
public:
    // The registry belongs to the running MCP service.
    static Jobs& instance();

    // Registers a running job. `dialog_rules`: dialogs to answer while it runs (see DialogGuard).
    std::shared_ptr<Job> start(const std::string& kind, std::function<void()> cancel = {}, std::vector<DialogRule> dialog_rules = {});
    void update(const std::shared_ptr<Job>& job, double percent, const std::string& text);
    void finish(const std::shared_ptr<Job>& job, Job::State state, json result = nullptr);

    std::shared_ptr<Job> find(const std::string& id) const;
    // Most recently started job, optionally of one kind; running jobs first.
    std::shared_ptr<Job> latest(const std::string& kind = {}) const;
    std::vector<std::shared_ptr<Job>> all() const { return m_jobs; }
    bool any_running() const;

    // Service shutdown: forget everything (running operations are not stopped).
    void clear();

    void set_guard(DialogGuard* guard) { m_guard = guard; }

private:
    std::vector<std::shared_ptr<Job>> m_jobs; // oldest first
    DialogGuard*                      m_guard { nullptr };
    size_t                            m_next_id { 1 };
};

// {"job", "kind", "status": running|finished|failed|cancelled, "percent", "text", "elapsed_s",
//  "result"?, "dialogs"?}
json job_json(Job& job);

// A Pending that answers with job_json() as soon as the job is no longer running, or with the
// running state when `wait_s` elapses.
Pending wait_for_job(std::shared_ptr<Job> job, double wait_s);

}}} // namespace Slic3r::GUI::Mcp

#endif
