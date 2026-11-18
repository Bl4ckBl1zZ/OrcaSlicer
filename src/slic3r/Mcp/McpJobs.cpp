#include "McpJobs.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace Slic3r { namespace GUI { namespace Mcp {

namespace {

const size_t MAX_FINISHED_JOBS = 20;

const char* state_name(Job::State state)
{
    switch (state) {
    case Job::State::Running: return "running";
    case Job::State::Finished: return "finished";
    case Job::State::Failed: return "failed";
    case Job::State::Cancelled: return "cancelled";
    }
    return "unknown";
}

} // namespace

Jobs& Jobs::instance()
{
    static Jobs jobs;
    return jobs;
}

std::shared_ptr<Job> Jobs::start(const std::string& kind, std::function<void()> cancel, std::vector<DialogRule> dialog_rules)
{
    auto job = std::make_shared<Job>();
    job->id = "job" + std::to_string(m_next_id++);
    job->kind = kind;
    job->cancel = std::move(cancel);
    job->started = std::chrono::steady_clock::now();
    if (!dialog_rules.empty()) {
        job->dialogs = std::make_shared<DialogScope>();
        job->dialogs->rules = std::move(dialog_rules);
        if (m_guard != nullptr)
            m_guard->add_scope(job->dialogs);
    }
    m_jobs.push_back(job);

    // Keep the list short: drop the oldest finished jobs.
    size_t finished = static_cast<size_t>(std::count_if(m_jobs.begin(), m_jobs.end(), [](const std::shared_ptr<Job>& j) { return j->state != Job::State::Running; }));
    for (auto it = m_jobs.begin(); finished > MAX_FINISHED_JOBS && it != m_jobs.end();) {
        if ((*it)->state != Job::State::Running) {
            it = m_jobs.erase(it);
            --finished;
        } else
            ++it;
    }
    return job;
}

void Jobs::update(const std::shared_ptr<Job>& job, double percent, const std::string& text)
{
    if (!job || job->state != Job::State::Running)
        return;
    job->percent = std::max(0., std::min(100., percent));
    job->text = text;
}

void Jobs::finish(const std::shared_ptr<Job>& job, Job::State state, json result)
{
    if (!job || job->state != Job::State::Running)
        return;
    job->state = state;
    job->ended = std::chrono::steady_clock::now();
    if (state == Job::State::Finished)
        job->percent = 100.;
    if (!result.is_null())
        job->result = std::move(result);
    if (job->dialogs && m_guard != nullptr)
        m_guard->remove_scope(job->dialogs);
}

std::shared_ptr<Job> Jobs::find(const std::string& id) const
{
    for (const std::shared_ptr<Job>& job : m_jobs)
        if (job->id == id)
            return job;
    return nullptr;
}

std::shared_ptr<Job> Jobs::latest(const std::string& kind) const
{
    std::shared_ptr<Job> newest;
    for (auto it = m_jobs.rbegin(); it != m_jobs.rend(); ++it) {
        if (!kind.empty() && (*it)->kind != kind)
            continue;
        if ((*it)->state == Job::State::Running)
            return *it;
        if (!newest)
            newest = *it;
    }
    return newest;
}

bool Jobs::any_running() const
{
    return std::any_of(m_jobs.begin(), m_jobs.end(), [](const std::shared_ptr<Job>& j) { return j->state == Job::State::Running; });
}

void Jobs::clear()
{
    if (m_guard != nullptr)
        for (const std::shared_ptr<Job>& job : m_jobs)
            if (job->dialogs)
                m_guard->remove_scope(job->dialogs);
    m_jobs.clear();
}

json job_json(Job& job)
{
    if (job.state != Job::State::Running && job.result.is_null() && job.collect) {
        job.result = job.collect(job);
        job.collect = nullptr;
    }
    const auto end = job.state == Job::State::Running ? std::chrono::steady_clock::now() : job.ended;
    json out = json::object();
    out["job"] = job.id;
    out["kind"] = job.kind;
    out["status"] = state_name(job.state);
    out["percent"] = std::round(job.percent * 10.) / 10.;
    if (!job.text.empty())
        out["text"] = job.text;
    out["elapsed_s"] = std::round(std::chrono::duration<double>(end - job.started).count() * 10.) / 10.;
    if (!job.result.is_null())
        out["result"] = job.result;
    if (job.dialogs && !job.dialogs->records.empty()) {
        json dialogs = json::array();
        for (const DialogRecord& record : job.dialogs->records)
            dialogs.push_back(to_json(record));
        out["dialogs"] = std::move(dialogs);
    }
    return out;
}

Pending wait_for_job(std::shared_ptr<Job> job, double wait_s)
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
    pending.on_deadline = [job](ToolContext&) { return job_json(*job); };
    return pending;
}

}}} // namespace Slic3r::GUI::Mcp
