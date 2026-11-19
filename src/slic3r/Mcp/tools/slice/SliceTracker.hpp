#ifndef slic3r_Mcp_SliceTracker_hpp_
#define slic3r_Mcp_SliceTracker_hpp_

// Follows the application's own slicing, whoever started it (the agent through `slice`, or the user
// with the Slice button), without hooks in upstream files:
//
//  - a wxEventFilter sees SlicingStatusEvent (progress text and percent) and
//    SlicingProcessCompletedEvent (finished / cancelled / error with its message and object ids)
//    before Plater handles them. EVT_PROCESS_COMPLETED is not declared in any header, so the event
//    is recognised by its C++ class;
//  - a timer, running only while an agent job is active, decides when the job is over: when the
//    plater no longer reports slicing and the background process is idle, after Plater's own
//    handler has updated the plate state.
//
// Per plate it keeps the outcome of the most recent slice (state, error, timing) and whether a result
// that existed was invalidated since. Main thread only.

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../../McpJobs.hpp"
#include "../../McpJson.hpp"

namespace Slic3r { namespace GUI { namespace Mcp { namespace Slice {

struct PlateRecord
{
    // Outcome of the last slice of this plate seen by the tracker: "", "finished", "cancelled", "error".
    std::string last_outcome;
    std::string error_message;       // error: the message the GUI shows
    std::vector<size_t> error_object_ids; // ObjectBase ids named by the error (PrintObject or ModelObject)
    bool        critical { false };
    std::string cancel_reason;       // "agent", "user", "scene_changed" (best effort)
    std::chrono::system_clock::time_point finished_at;
    double      duration_s { 0. };
    bool        had_valid_result { false }; // a valid result was seen; used to report "invalidated"
    unsigned    slices { 0 };        // completed slices seen
};

class Tracker
{
public:
    static Tracker& instance();

    // Installs the event filter (idempotent). Called by every slice tool.
    void ensure();

    // A slice started by a tool. `plates`: 0-based indices to slice, in order (all plates for "all").
    std::shared_ptr<Job> start_job(const std::string& kind, std::vector<int> plates, bool all);
    std::shared_ptr<Job> running_job() const;
    // The agent asked to stop the running slice.
    void mark_cancel_requested();

    PlateRecord& record(int plate_idx) { return m_plates[plate_idx]; }
    const PlateRecord* find_record(int plate_idx) const;
    // Called by tools that read a plate: notes valid results so a later invalidation can be reported.
    void observe_plate(int plate_idx, bool valid);

    // Last progress seen (any slice): percent (-1 unknown) and text.
    double      last_percent() const { return m_last_percent; }
    const std::string& last_text() const { return m_last_text; }

    // Internal, from the filter.
    void on_status(int percent, const std::string& text);
    void on_completed(int status, const std::string& message, const std::vector<size_t>& ids, bool critical, bool invalidate);
    // Internal, from the timer.
    void tick();

    // Builds the job result once the job ended (used by the timer and by tests through slice_status).
    std::function<json(int plate_idx)> summarize; // set by ToolsSlice: compact per-plate summary

private:
    Tracker() = default;
    void finish(const std::string& outcome);

    struct Filter;
    std::unique_ptr<Filter> m_filter;
    struct Timer;
    std::unique_ptr<Timer> m_timer;

    std::map<int, PlateRecord> m_plates;

    // Current agent job.
    std::shared_ptr<Job> m_job;
    std::vector<int>     m_job_plates;
    bool                 m_job_all { false };
    bool                 m_cancel_requested { false };
    bool                 m_seen_slicing { false };   // the plater reported slicing at least once for this job
    int                  m_idle_ticks { 0 };
    int                  m_completions { 0 };        // completion events seen during the job
    std::vector<json>    m_job_events;               // per-plate outcomes during the job
    std::chrono::steady_clock::time_point m_plate_started;

    double      m_last_percent { -1. };
    std::string m_last_text;
};

// The plate the background slicing process currently works on (0-based), -1 when unknown.
int bsp_plate_index();

}}}} // namespace Slic3r::GUI::Mcp::Slice

#endif
