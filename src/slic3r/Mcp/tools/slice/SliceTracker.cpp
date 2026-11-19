#include "SliceTracker.hpp"

#include <algorithm>
#include <cmath>

#include <wx/event.h>
#include <wx/timer.h>

#include "libslic3r/PrintBase.hpp"
#include "slic3r/GUI/BackgroundSlicingProcess.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"

namespace Slic3r { namespace GUI { namespace Mcp { namespace Slice {

namespace {

const int TICK_MS = 100;
// Ticks the plater must be idle before a job is declared over: lets Plater's completion handler
// (and the plate switch of "slice all") run first.
const int IDLE_TICKS = 3;

Plater* plater_or_null()
{
    if (wxTheApp == nullptr)
        return nullptr;
    return wxGetApp().plater();
}

} // namespace

int bsp_plate_index()
{
    Plater* plater = plater_or_null();
    if (plater == nullptr)
        return -1;
    PartPlate* plate = plater->background_process().get_current_plate();
    return plate != nullptr ? plate->get_index() : -1;
}

struct Tracker::Filter : public wxEventFilter
{
    const wxEvent* last_completed { nullptr };
    long           last_completed_ts { -1 };

    int FilterEvent(wxEvent& event) override
    {
        if (event.GetEventType() == EVT_SLICING_UPDATE) {
            if (auto* status = dynamic_cast<SlicingStatusEvent*>(&event))
                Tracker::instance().on_status(status->status.percent, status->status.text);
            return Event_Skip;
        }
        // EVT_PROCESS_COMPLETED has no declaration outside Plater.cpp: recognise the class.
        if (auto* done = dynamic_cast<SlicingProcessCompletedEvent*>(&event)) {
            // The same event object can reach the filter more than once while it propagates.
            if (&event == last_completed && event.GetTimestamp() == last_completed_ts)
                return Event_Skip;
            last_completed = &event;
            last_completed_ts = event.GetTimestamp();
            std::string              message;
            std::vector<size_t>      ids;
            bool                     critical = false, invalidate = false;
            if (done->error()) {
                try {
                    auto formatted = done->format_error_message();
                    message = formatted.first;
                    ids = formatted.second;
                } catch (...) {
                    message = "unknown slicing error";
                }
                critical = done->critical_error();
                invalidate = done->invalidate_plater();
            }
            Tracker::instance().on_completed(static_cast<int>(done->status()), message, ids, critical, invalidate);
        }
        return Event_Skip;
    }
};

struct Tracker::Timer : public wxTimer
{
    void Notify() override { Tracker::instance().tick(); }
};

Tracker& Tracker::instance()
{
    static Tracker tracker;
    return tracker;
}

void Tracker::ensure()
{
    if (m_filter)
        return;
    m_filter = std::make_unique<Filter>();
    wxEvtHandler::AddFilter(m_filter.get());
    m_timer = std::make_unique<Timer>();
}

const PlateRecord* Tracker::find_record(int plate_idx) const
{
    auto it = m_plates.find(plate_idx);
    return it == m_plates.end() ? nullptr : &it->second;
}

void Tracker::observe_plate(int plate_idx, bool valid)
{
    if (valid)
        m_plates[plate_idx].had_valid_result = true;
}

std::shared_ptr<Job> Tracker::start_job(const std::string& kind, std::vector<int> plates, bool all)
{
    ensure();
    // Answer the error box a failed slice raises (show_error, deferred): nobody may be watching.
    std::vector<DialogRule> rules;
    {
        DialogRule rule;
        rule.title_contains = "Error";
        rule.answer = DialogAnswer::Ok;
        rule.informational = true;
        rules.push_back(rule);
    }
    {
        DialogRule rule;
        rule.title_contains = "warning";
        rule.answer = DialogAnswer::Ok;
        rule.informational = true;
        rules.push_back(rule);
    }
    m_job = Jobs::instance().start(kind, [this] {
        Plater* plater = plater_or_null();
        if (plater == nullptr)
            return;
        mark_cancel_requested();
        // What the slicing notification's Cancel button does.
        if (!plater->background_process().idle())
            plater->background_process().stop();
    }, std::move(rules));
    m_job_plates = std::move(plates);
    m_job_all = all;
    m_cancel_requested = false;
    m_seen_slicing = false;
    m_idle_ticks = 0;
    m_completions = 0;
    m_job_events.clear();
    m_plate_started = std::chrono::steady_clock::now();
    m_last_percent = 0.;
    m_last_text.clear();
    Jobs::instance().update(m_job, 0., "starting");
    m_timer->Start(TICK_MS);
    return m_job;
}

std::shared_ptr<Job> Tracker::running_job() const
{
    return (m_job && m_job->state == Job::State::Running) ? m_job : nullptr;
}

void Tracker::mark_cancel_requested()
{
    m_cancel_requested = true;
}

void Tracker::on_status(int percent, const std::string& text)
{
    if (percent >= 0)
        m_last_percent = percent;
    if (!text.empty())
        m_last_text = text;
    if (!running_job())
        return;
    m_seen_slicing = true;
    double overall = m_last_percent < 0 ? 0. : m_last_percent;
    if (m_job_all && !m_job_plates.empty()) {
        const double n = static_cast<double>(m_job_plates.size());
        overall = (std::min<double>(m_completions, n - 1) * 100. + overall) / n;
    }
    Jobs::instance().update(m_job, overall, m_last_text);
}

void Tracker::on_completed(int status, const std::string& message, const std::vector<size_t>& ids, bool critical, bool invalidate)
{
    const int    plate_idx = bsp_plate_index();
    PlateRecord& rec = m_plates[plate_idx];
    const auto   now = std::chrono::steady_clock::now();
    rec.finished_at = std::chrono::system_clock::now();
    rec.duration_s = std::chrono::duration<double>(now - m_plate_started).count();
    m_plate_started = now;
    rec.error_message.clear();
    rec.error_object_ids.clear();
    rec.critical = false;
    rec.cancel_reason.clear();
    switch (static_cast<SlicingProcessCompletedEvent::StatusType>(status)) {
    case SlicingProcessCompletedEvent::Finished: rec.last_outcome = "finished"; ++rec.slices; break;
    case SlicingProcessCompletedEvent::Cancelled:
        rec.last_outcome = "cancelled";
        rec.cancel_reason = m_cancel_requested ? "agent" : (running_job() ? "scene_changed_or_user" : "user_or_scene_changed");
        break;
    case SlicingProcessCompletedEvent::Error:
        rec.last_outcome = "error";
        rec.error_message = message;
        rec.error_object_ids = ids;
        rec.critical = critical;
        break;
    }
    (void) invalidate;
    if (!running_job())
        return;
    ++m_completions;
    json ev = json::object();
    ev["plate"] = plate_idx + 1;
    ev["outcome"] = rec.last_outcome;
    if (!rec.error_message.empty())
        ev["error"] = rec.error_message;
    if (!rec.cancel_reason.empty())
        ev["cancel_reason"] = rec.cancel_reason;
    ev["duration_s"] = std::round(rec.duration_s * 10.) / 10.;
    m_job_events.push_back(std::move(ev));
}

void Tracker::tick()
{
    std::shared_ptr<Job> job = running_job();
    if (!job) {
        m_timer->Stop();
        return;
    }
    Plater* plater = plater_or_null();
    if (plater == nullptr) {
        finish("failed");
        return;
    }
    const bool slicing = plater->is_background_process_slicing() || plater->background_process().running();
    if (slicing) {
        m_seen_slicing = true;
        m_idle_ticks = 0;
        return;
    }
    if (++m_idle_ticks < IDLE_TICKS)
        return;
    // Over. Decide the outcome from the completion events and the plates' state.
    bool any_error = false, any_cancel = false;
    for (const json& ev : m_job_events) {
        const std::string outcome = ev.value("outcome", "");
        any_error = any_error || outcome == "error";
        any_cancel = any_cancel || outcome == "cancelled";
    }
    bool all_valid = true;
    PartPlateList& list = plater->get_partplate_list();
    for (int p : m_job_plates) {
        PartPlate* plate = (p >= 0 && p < list.get_plate_count()) ? list.get_plate(p) : nullptr;
        if (plate == nullptr || !plate->is_slice_result_valid())
            all_valid = false;
    }
    if (any_error)
        finish("failed");
    else if (any_cancel || m_cancel_requested)
        finish("cancelled");
    else if (all_valid)
        finish("finished");
    else
        finish(m_job_events.empty() ? "not_started" : "failed");
}

void Tracker::finish(const std::string& outcome)
{
    std::shared_ptr<Job> job = m_job;
    m_timer->Stop();
    json result = json::object();
    result["outcome"] = outcome;
    json plates = json::array();
    for (int p : m_job_plates) {
        json entry = summarize ? summarize(p) : json::object({{"plate", p + 1}});
        // Attach this job's event for the plate, if any.
        for (const json& ev : m_job_events)
            if (ev.value("plate", 0) == p + 1) {
                entry["outcome"] = ev["outcome"];
                if (ev.contains("error"))
                    entry["error"] = ev["error"];
                if (ev.contains("cancel_reason"))
                    entry["cancel_reason"] = ev["cancel_reason"];
                entry["duration_s"] = ev["duration_s"];
            }
        plates.push_back(std::move(entry));
    }
    result["plates"] = std::move(plates);
    Job::State state = Job::State::Finished;
    if (outcome == "failed" || outcome == "not_started")
        state = Job::State::Failed;
    else if (outcome == "cancelled")
        state = Job::State::Cancelled;
    if (state == Job::State::Finished)
        Jobs::instance().update(job, 100., "Slicing complete");
    Jobs::instance().finish(job, state, std::move(result));
}

}}}} // namespace Slic3r::GUI::Mcp::Slice
