// print_send and printer_control: the only MCP code that makes a printer do something.
//
// Every send and every stop goes through the user's confirmation window (SendConfirm.hpp). The
// order of events for a send:
//   1. checks, in the call: switch on, plate sliced and current, printer known, compatible, online,
//      idle, filament mapping valid; any failure is an error result and nothing happens;
//   2. the file to send is written to a private temp file (what the user confirms is what is sent,
//      whatever happens to the scene meanwhile), and the call returns a job;
//   3. the job waits for its turn (one confirmation window at a time, at most MAX_QUEUED waiting),
//      then for the user's answer: Send, Cancel / close, or the timeout;
//   4. only on Send, after checking the printer is still online and idle, the file is transferred.

#include "Printers.hpp"
#include "SendConfirm.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <set>
#include <sstream>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include <wx/event.h>
#include <wx/timer.h>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/ProjectTask.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/DeviceCore/DevDefs.h"
#include "slic3r/GUI/DeviceCore/DevExtruderSystem.h"
#include "slic3r/GUI/DeviceCore/DevMapping.h"
#include "slic3r/GUI/DeviceCore/DevNozzleSystem.h"
#include "slic3r/GUI/DeviceCore/DevStorage.h"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Jobs/BoostThreadWorker.hpp"
#include "slic3r/GUI/Jobs/PlaterWorker.hpp"
#include "slic3r/GUI/Jobs/PrintJob.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/Utils/ASCIIFolding.hpp"
#include "slic3r/Utils/PrintHost.hpp"

#include "../../McpError.hpp"
#include "../../McpIds.hpp"
#include "../../McpJobs.hpp"
#include "../../McpTool.hpp"
#include "../project/LoadGuard.hpp"
#include "../slice/SliceData.hpp"

namespace Slic3r { namespace GUI { namespace Mcp { namespace Printers {

namespace fs = boost::filesystem;

namespace {

constexpr int MAX_QUEUED = 3; // sends waiting behind the one being confirmed

Plater&       plater() { return *wxGetApp().plater(); }
PresetBundle& bundle() { return *wxGetApp().preset_bundle; }

std::string upper(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// ------------------------------------------------------------------------------------------------
// Plate
// ------------------------------------------------------------------------------------------------

void require_current_slice(int idx)
{
    using namespace Slice;
    if (plate_busy(idx) || slicing_now())
        fail("busy_slicing", "A slice is running.", "Wait with slice_status, then send.");
    if (!plate_has_result(idx)) {
        json state = plate_state(idx);
        const std::string s = state.value("state", "not_sliced");
        if (s == "invalidated")
            fail("slice_stale", "Plate " + std::to_string(idx + 1) + " changed after it was sliced; its slice result is out of date.",
                 "Slice the plate again, then send.", state);
        fail("not_sliced", "Plate " + std::to_string(idx + 1) + " has no valid slice result (state: " + s + ").",
             s == "empty" ? "Put an object on the plate, slice it, then send." : "Slice the plate first.", state);
    }
    PartPlate* plate = handles(idx).plate;
    const std::string gcode = plate->get_tmp_gcode_path();
    if (!plate->is_valid_gcode_file() || gcode.empty() || !fs::exists(gcode))
        fail("slice_stale", "The sliced G-code of plate " + std::to_string(idx + 1) + " is not available.", "Slice the plate again, then send.");
}

void refuse_scripts()
{
    const json found = LoadGuard::untrusted_scripts();
    if (!found.empty())
        fail("untrusted_scripts",
             "The process preset runs post-processing scripts or slicing plug-ins the user has not saved as their own; sending is "
             "refused while they are set.",
             "Ask the user to check them and either save the process preset in the window or remove them.",
             json::object({{"settings", found}}));
}

struct UsedFilament
{
    int         index { 0 };  // 0-based slot of the project
    std::string type;
    std::string color_rgba;   // "#RRGGBBAA"
    std::string color;        // "#RRGGBB"
    std::string filament_id;
    std::string brand;
};

std::vector<UsedFilament> plate_filaments(int idx)
{
    std::vector<UsedFilament> out;
    PresetBundle& pb    = bundle();
    PartPlate*    plate = Slice::handles(idx).plate;
    for (int one_based : plate->get_used_filaments()) {
        const int i = one_based - 1;
        if (i < 0 || i >= int(pb.filament_presets.size()))
            continue;
        UsedFilament f;
        f.index = i;
        if (const Preset* preset = pb.filaments.find_preset(pb.filament_presets[size_t(i)], false); preset != nullptr) {
            std::string display;
            f.type        = const_cast<Preset*>(preset)->config.get_filament_type(display);
            f.filament_id = preset->filament_id;
            if (auto* vendor = dynamic_cast<const ConfigOptionStrings*>(preset->config.option("filament_vendor")); vendor && !vendor->values.empty())
                f.brand = vendor->values.front();
        }
        std::string colour = pb.project_config.opt_string("filament_colour", static_cast<unsigned int>(i));
        colour             = upper(colour);
        if (colour.empty() || colour[0] != '#')
            colour = "#" + colour;
        if (colour.size() == 7)
            colour += "FF";
        f.color_rgba = colour.size() == 9 ? colour : "#808080FF";
        f.color      = f.color_rgba.substr(0, 7);
        out.push_back(std::move(f));
    }
    return out;
}

double colour_distance(const std::string& a, const std::string& b)
{
    auto rgb = [](const std::string& c, int k) {
        try {
            return std::stoi(c.substr(1 + 2 * k, 2), nullptr, 16);
        } catch (...) {
            return 0;
        }
    };
    if (a.size() < 7 || b.size() < 7)
        return 0.;
    double d = 0.;
    for (int k = 0; k < 3; ++k)
        d += std::pow(double(rgb(a, k) - rgb(b, k)), 2.);
    return std::sqrt(d);
}

// ------------------------------------------------------------------------------------------------
// Filament -> tray mapping (Bambu)
// ------------------------------------------------------------------------------------------------

struct Mapped
{
    UsedFilament f;
    Tray         tray;
    bool         color_warn { false };
};

std::string norm_type(std::string t)
{
    t = upper(t);
    // "PLA Basic" and "PLA" are the same material for the printer.
    if (auto sp = t.find(' '); sp != std::string::npos)
        t = t.substr(0, sp);
    return t;
}

std::vector<Mapped> map_filaments(MachineObject* obj, const std::vector<UsedFilament>& used, const json& mapping_arg, bool use_ams,
                                  json& warnings)
{
    const std::vector<Tray> trays = bambu_trays(obj);
    auto by_label = [&trays](const std::string& label) -> const Tray* {
        for (const Tray& t : trays)
            if (upper(t.label) == upper(label))
                return &t;
        return nullptr;
    };
    auto by_ids = [&trays](const std::string& ams_id, const std::string& slot_id) -> const Tray* {
        for (const Tray& t : trays)
            if (t.ams_id == ams_id && t.slot_id == slot_id)
                return &t;
        return nullptr;
    };
    json tray_list = json::array();
    for (const Tray& t : trays)
        tray_list.push_back(tray_json(t));

    std::vector<Mapped> out;
    if (mapping_arg.is_array()) {
        std::map<int, std::string> wanted; // 1-based filament -> tray label
        for (const json& m : mapping_arg)
            wanted[m.value("filament", 0)] = m.value("tray", "");
        for (const auto& [filament, label] : wanted) {
            bool on_plate = false;
            for (const UsedFilament& f : used)
                on_plate = on_plate || f.index + 1 == filament;
            if (!on_plate)
                fail("invalid_argument", "`ams_mapping`: filament " + std::to_string(filament) + " is not used on this plate.",
                     "Map only the filaments the plate uses.", json::object({{"used_filaments", [&used] {
                                                                                 json a = json::array();
                                                                                 for (const UsedFilament& f : used)
                                                                                     a.push_back(f.index + 1);
                                                                                 return a;
                                                                             }()}}));
        }
        for (const UsedFilament& f : used) {
            auto it = wanted.find(f.index + 1);
            if (it == wanted.end())
                fail("incomplete_mapping", "`ams_mapping` gives no tray for filament " + std::to_string(f.index + 1) + " (" + f.type + ").",
                     "Map every filament the plate uses, or pass ams_mapping \"auto\".", json::object({{"trays", tray_list}}));
            const Tray* t = by_label(it->second);
            if (t == nullptr)
                fail("tray_not_found", "The printer has no tray \"" + it->second + "\".", "Use a tray label from printer_status.",
                     json::object({{"trays", tray_list}}));
            out.push_back(Mapped{f, *t});
        }
    } else {
        // "auto": the application's own matching (type first, then the nearest colour).
        if (!use_ams || !obj->HasAms()) {
            const Tray* ext = by_label("ext");
            if (ext == nullptr)
                fail("tray_not_found", "The printer reports no external spool holder.", "Call printer_status and map the trays explicitly.");
            for (const UsedFilament& f : used)
                out.push_back(Mapped{f, *ext});
        } else {
            std::vector<FilamentInfo> filaments, result;
            for (const UsedFilament& f : used) {
                FilamentInfo info;
                info.id          = f.index;
                info.type        = f.type;
                info.color       = f.color_rgba;
                info.filament_id = f.filament_id;
                info.brand       = f.brand;
                filaments.push_back(info);
            }
            const int rc = DevMappingUtil::ams_filament_mapping(obj, filaments, result, {false, true, false, false});
            if (rc != 0 || result.size() != used.size())
                fail("no_matching_tray", "The printer's trays could not be matched to the plate's filaments.",
                     "Map the trays explicitly with ams_mapping (see printer_status).", json::object({{"trays", tray_list}}));
            for (size_t k = 0; k < used.size(); ++k) {
                const FilamentInfo& r = result[k];
                const Tray*         t = r.tray_id < 0 ? nullptr : by_ids(r.ams_id, r.slot_id);
                if (t == nullptr)
                    fail("no_matching_tray", "No tray of the printer holds " + used[k].type + " for filament " + std::to_string(used[k].index + 1) + ".",
                         "Load the filament, or map the trays explicitly with ams_mapping.", json::object({{"trays", tray_list}}));
                out.push_back(Mapped{used[k], *t});
            }
        }
    }

    // Checks: empty tray and wrong material are refused; a different colour is a warning.
    for (Mapped& m : out) {
        if (m.tray.empty)
            fail("tray_empty", "Tray " + m.tray.label + " (for filament " + std::to_string(m.f.index + 1) + ") is empty.",
                 "Map a loaded tray, or ask the user to load " + m.f.type + ".", json::object({{"trays", tray_list}}));
        if (!m.f.type.empty() && norm_type(m.f.type) != norm_type(m.tray.type))
            fail("filament_type_mismatch",
                 "Filament " + std::to_string(m.f.index + 1) + " is " + m.f.type + " but tray " + m.tray.label + " holds " + m.tray.type + ".",
                 "Map a tray with " + m.f.type + ", or change the plate's filament and slice again.", json::object({{"trays", tray_list}}));
        if (!m.tray.color.empty() && colour_distance(m.f.color, m.tray.color) > 60.) {
            m.color_warn = true;
            warnings.push_back("filament " + std::to_string(m.f.index + 1) + " is " + m.f.color + " on the plate but tray " + m.tray.label +
                               " holds " + m.tray.color);
        }
    }
    return out;
}

// The three mapping strings of a print job, built the way SelectMachineDialog::get_ams_mapping_result
// does: one entry per project filament slot.
void mapping_strings(const std::vector<Mapped>& mapped, int plate_idx, std::string& v0, std::string& v1, std::string& info)
{
    json a0 = json::array(), a1 = json::array(), ai = json::array();
    const std::vector<int> filament_maps = Slice::handles(plate_idx).plate->get_filament_maps();
    for (int i = 0; i < int(bundle().filament_presets.size()); ++i) {
        int  tray_id = -1;
        json item_v1 = json::object({{"ams_id", 0xff}, {"slot_id", 0xff}});
        json item    = json::object({{"ams", tray_id}, {"targetColor", ""}, {"filamentId", ""}, {"filamentType", ""}});
        for (const Mapped& m : mapped) {
            if (m.f.index != i)
                continue;
            tray_id              = m.tray.tray_index;
            item["ams"]          = tray_id;
            item["filamentType"] = m.f.type;
            item["filamentId"]   = m.f.filament_id;
            if (i < int(filament_maps.size()))
                item["nozzleId"] = filament_maps[size_t(i)] == 2 ? 0 : 1; // CloudTaskNozzleId
            item["sourceColor"] = m.f.color_rgba;
            item["targetColor"] = m.tray.color_raw;
            if (tray_id == VIRTUAL_TRAY_MAIN_ID || tray_id == VIRTUAL_TRAY_DEPUTY_ID)
                tray_id = -1;
            try {
                item_v1["ams_id"]  = std::stoi(m.tray.ams_id);
                item_v1["slot_id"] = std::stoi(m.tray.slot_id);
            } catch (...) {
                item_v1["ams_id"]  = VIRTUAL_TRAY_MAIN_ID;
                item_v1["slot_id"] = VIRTUAL_TRAY_MAIN_ID;
            }
        }
        a0.push_back(tray_id);
        a1.push_back(item_v1);
        ai.push_back(item);
    }
    v0   = a0.dump();
    v1   = a1.dump();
    info = ai.dump();
}

// ------------------------------------------------------------------------------------------------
// The confirmation queue
// ------------------------------------------------------------------------------------------------

struct Task
{
    std::shared_ptr<Job> job;
    std::string          tool;      // "print_send" / "printer_control"
    std::string          action;    // "send" / "stop"
    Printer              printer;
    ConfirmRequest       request;
    json                 summary;   // the job's result base: what was asked
    // Checked again right after the user's Send: returns an error ({code, message, hint}) or null.
    std::function<json()> recheck;
    // Starts the transfer (main thread). Must finish the job eventually.
    std::function<void(std::shared_ptr<Task>)> transfer;
    std::vector<fs::path> temp_files;
    bool                  withdrawn { false }; // job_cancel while waiting
};

std::deque<std::shared_ptr<Task>> g_waiting;
std::shared_ptr<Task>             g_asking;
std::atomic<int>                  g_transferring { 0 };

void cleanup(const std::shared_ptr<Task>& t)
{
    for (const fs::path& p : t->temp_files) {
        boost::system::error_code ec;
        fs::remove(p, ec);
    }
    t->temp_files.clear();
}

void end_task(const std::shared_ptr<Task>& t, Job::State state, json result)
{
    cleanup(t);
    if (t->job->state == Job::State::Running)
        Jobs::instance().finish(t->job, state, std::move(result));
}

json failure_result(const std::shared_ptr<Task>& t, const std::string& code, const std::string& message, const std::string& outcome,
                    const std::string& hint = {})
{
    json error = json::object({{"code", code}, {"message", message}});
    if (!hint.empty())
        error["hint"] = hint;
    json r       = t->summary;
    r["error"]   = std::move(error);
    r["outcome"] = outcome;
    return r;
}

void pump();

void on_answer(const std::shared_ptr<Task>& t, ConfirmAnswer answer)
{
    g_asking = nullptr;
    audit_later(t->tool, t->action + "_answer", t->printer.id, json::object({{"answer", answer_name(answer)}, {"job", t->job->id}}));
    if (t->withdrawn) {
        end_task(t, Job::State::Cancelled, failure_result(t, "cancelled", "The agent withdrew the request before the user answered.", "cancelled"));
    } else if (answer == ConfirmAnswer::Declined) {
        end_task(t, Job::State::Failed,
                 failure_result(t, "declined", "The user declined: nothing was sent.", "declined",
                                "Do not send it again unless the user asks you to."));
    } else if (answer == ConfirmAnswer::TimedOut) {
        end_task(t, Job::State::Failed,
                 failure_result(t, "timed_out", "Nobody answered the confirmation window in time: nothing was sent.", "timed_out",
                                "Ask the user whether they want to send it, then call again while they are at the computer."));
    } else {
        json problem = t->recheck ? t->recheck() : json(nullptr);
        if (!problem.is_null()) {
            end_task(t, Job::State::Failed,
                     failure_result(t, problem.value("code", "printer_busy"), problem.value("message", ""), "not_sent", problem.value("hint", "")));
        } else {
            Jobs::instance().update(t->job, 5., t->action == "stop" ? "stopping the print" : "sending");
            ++g_transferring;
            t->transfer(t);
        }
    }
    // Let the next request show its window after this event handler returns.
    wxGetApp().CallAfter([] { pump(); });
}

// Calls fn every 100 ms on the main thread until it returns true (waiting for a worker thread without
// spinning the event loop).
class PollTimer : public wxTimer
{
public:
    static void start(std::function<bool()> fn) { (new PollTimer(std::move(fn)))->Start(100); }

    void Notify() override
    {
        if (m_fn()) {
            Stop();
            delete this;
        }
    }

private:
    explicit PollTimer(std::function<bool()> fn) : m_fn(std::move(fn)) {}
    std::function<bool()> m_fn;
};

void pump()
{
    if (g_asking != nullptr || confirmation_open())
        return;
    while (!g_waiting.empty()) {
        std::shared_ptr<Task> t = g_waiting.front();
        g_waiting.pop_front();
        if (t->withdrawn || t->job->state != Job::State::Running) {
            end_task(t, Job::State::Cancelled, failure_result(t, "cancelled", "The agent withdrew the request.", "cancelled"));
            continue;
        }
        // The printer may have changed while this request waited for an earlier one.
        if (json problem = t->recheck ? t->recheck() : json(nullptr); !problem.is_null()) {
            end_task(t, Job::State::Failed,
                     failure_result(t, problem.value("code", "printer_busy"), problem.value("message", ""), "not_sent", problem.value("hint", "")));
            continue;
        }
        g_asking = t;
        Jobs::instance().update(t->job, 0., "waiting for the user to confirm in the OrcaSlicer window");
        audit_later(t->tool, t->action + "_asked", t->printer.id, json::object({{"job", t->job->id}}));
        ask_confirmation(t->request, [t](ConfirmAnswer a) { on_answer(t, a); });
        return;
    }
}

std::shared_ptr<Task> enqueue(std::shared_ptr<Task> t)
{
    std::weak_ptr<Task> weak = t;
    t->job = Jobs::instance().start(t->tool, [weak] {
        // job_cancel: a request still waiting is withdrawn (nothing is sent). One being transferred
        // cannot be called back.
        if (auto task = weak.lock(); task) {
            task->withdrawn = true;
            auto it = std::find(g_waiting.begin(), g_waiting.end(), task);
            if (it != g_waiting.end()) {
                g_waiting.erase(it);
                end_task(task, Job::State::Cancelled, failure_result(task, "cancelled", "The agent withdrew the request.", "cancelled"));
            }
        }
    });
    g_waiting.push_back(t);
    Jobs::instance().update(t->job, 0., g_asking != nullptr ? "waiting for an earlier confirmation to be answered" : "waiting");
    pump();
    return t;
}

void check_capacity()
{
    const int waiting = int(g_waiting.size()) + (g_asking != nullptr ? 1 : 0);
    if (waiting > MAX_QUEUED)
        fail("rate_limited", "Too many print requests are waiting for the user (" + std::to_string(waiting) + ").",
             "Wait for the user to answer the open request (job_status), then try again.");
}

// ------------------------------------------------------------------------------------------------
// Bambu transfer: the application's own PrintJob, observed.
// ------------------------------------------------------------------------------------------------

struct Observed
{
    std::mutex  mutex;
    std::string status;
    std::string error;
    int         percent { 0 };
    bool        lan_check_failed { false };
};

class ObservedPrintJob : public GUI::Job
{
public:
    ObservedPrintJob(std::shared_ptr<PrintJob> job, std::shared_ptr<Task> task, std::shared_ptr<Observed> seen)
        : m_job(std::move(job)), m_task(std::move(task)), m_seen(std::move(seen))
    {}

    void process(Ctl& ctl) override
    {
        struct Spy : Ctl
        {
            Ctl&                      inner;
            std::shared_ptr<Observed> seen;
            std::shared_ptr<GUI::Mcp::Job> job;
            Spy(Ctl& c, std::shared_ptr<Observed> s, std::shared_ptr<GUI::Mcp::Job> j) : inner(c), seen(std::move(s)), job(std::move(j)) {}
            void update_status(int st, const std::string& msg) override
            {
                {
                    std::lock_guard<std::mutex> lock(seen->mutex);
                    if (st >= 0)
                        seen->percent = st;
                    if (!msg.empty())
                        seen->status = msg;
                }
                auto j = job;
                const double pct = std::max(5., std::min(99., double(st)));
                inner.call_on_main_thread([j, pct, msg] {
                    if (j->state == GUI::Mcp::Job::State::Running)
                        Jobs::instance().update(j, pct, msg.empty() ? j->text : msg);
                });
            }
            bool was_canceled() const override { return inner.was_canceled(); }
            void clear_percent() override {}
            void show_error_info(const std::string& msg, int, const std::string&, const std::string&) override
            {
                std::lock_guard<std::mutex> lock(seen->mutex);
                seen->error = msg;
            }
            std::future<void> call_on_main_thread(std::function<void()> fn) override { return inner.call_on_main_thread(std::move(fn)); }
        } spy { ctl, m_seen, m_task->job };
        m_job->process(spy);
    }

    void finalize(bool canceled, std::exception_ptr& eptr) override
    {
        m_job->finalize(canceled, eptr);
        std::string error;
        if (eptr) {
            try {
                std::rethrow_exception(eptr);
            } catch (const std::exception& e) {
                error = e.what();
            } catch (...) {
                error = "unknown error";
            }
            eptr = nullptr;
        }
        --g_transferring;
        std::string status, shown;
        bool        lan_failed = false;
        {
            std::lock_guard<std::mutex> lock(m_seen->mutex);
            status     = m_seen->status;
            shown      = m_seen->error;
            lan_failed = m_seen->lan_check_failed;
        }
        const bool ok = !canceled && error.empty() && shown.empty() && !lan_failed && m_job->is_finished();
        audit_later(m_task->tool, "send_result", m_task->printer.id,
                    json::object({{"job", m_task->job->id}, {"ok", ok}, {"status", ok ? "sent" : (shown.empty() ? status : shown)}}));
        if (ok) {
            json r       = m_task->summary;
            r["outcome"] = "sent";
            r["message"] = "The printer accepted the job.";
            end_task(m_task, Mcp::Job::State::Finished, std::move(r));
        } else {
            std::string why = !error.empty() ? error : !shown.empty() ? shown : lan_failed ? "the printer did not accept the LAN upload (check its access code and IP)" : status;
            end_task(m_task, Mcp::Job::State::Failed,
                     failure_result(m_task, canceled ? "cancelled" : "send_failed", "Sending failed: " + why, "failed",
                                    "Check the printer in the Device tab; the user may need to re-enter its access code."));
        }
    }

private:
    std::shared_ptr<PrintJob> m_job;
    std::shared_ptr<Task>     m_task;
    std::shared_ptr<Observed> m_seen;
};

Worker& send_worker()
{
    // Created once and kept for the life of the application: its thread waits idle between sends.
    static Worker* worker = new PlaterWorker<BoostThreadWorker>(wxGetApp().plater(), nullptr, "mcp_print_send");
    return *worker;
}

// An event type nobody listens to, so a finished send does not switch the window to the Device tab.
const wxEventType EVT_MCP_SEND_DONE = wxNewEventType();

struct BambuOptions
{
    std::string bed_leveling { "auto" };
    std::string flow_calibration { "auto" };
    bool        timelapse { false };
    bool        use_ams { true };
    std::string storage { "auto" };
};

void bambu_transfer(const std::shared_ptr<Task>& t, int plate_idx, std::string dev_id, fs::path file_3mf, fs::path config_3mf,
                    std::string project_name, std::vector<Mapped> mapped, BambuOptions opt)
{
    MachineObject* obj = nullptr;
    if (DeviceManager* dev = wxGetApp().getDeviceManager(); dev != nullptr)
        obj = machine(t->printer);
    if (obj == nullptr) {
        --g_transferring;
        end_task(t, Job::State::Failed, failure_result(t, "printer_offline", "The printer disappeared before the job was sent.", "not_sent"));
        return;
    }
    auto job = std::make_shared<PrintJob>(dev_id);
    job->m_dev_ip       = obj->get_dev_ip();
    job->m_ftp_folder   = obj->get_ftp_folder();
    job->m_access_code  = obj->get_access_code();
    job->m_local_use_ssl_for_ftp = obj->local_use_ssl_for_ftp;
    job->m_local_use_ssl         = obj->local_use_ssl;
    job->connection_type         = obj->connection_type();
    job->cloud_print_only        = obj->is_support_cloud_print_only;
    job->m_print_type            = "from_normal";
    job->set_project_name(project_name);
    mapping_strings(mapped, plate_idx, job->task_ams_mapping, job->task_ams_mapping2, job->task_ams_mapping_info);

    job->sdcard_state = obj->GetStorage()->get_sdcard_state();
    const bool abnormal_ok = wxGetApp().app_config->get("allow_abnormal_storage") == "true";
    job->has_sdcard = job->sdcard_state == DevStorage::SdcardState::HAS_SDCARD_NORMAL ||
                      (abnormal_ok && job->sdcard_state == DevStorage::SdcardState::HAS_SDCARD_ABNORMAL);
    job->could_emmc_print = opt.storage == "sdcard" ? false : obj->can_use_emmc_print();
    if (opt.storage == "internal")
        job->has_sdcard = false;

    const bool timelapse = opt.timelapse && obj->is_support_timelapse;
    const int  pa_mode   = obj->is_support_pa_mode ? 0 : -1;
    auto       as_int    = [](const std::string& v) { return v == "off" ? 0 : v == "on" ? 1 : 2; };
    // "pc": what SelectMachineDialog passes (MachineBedTypeString[0]); the printer takes the plate type
    // from the 3mf.
    job->set_print_config("pc", opt.bed_leveling == "on", opt.flow_calibration == "on", false, timelapse, true, false,
                          as_int(opt.bed_leveling), obj->is_support_flow_calibration ? as_int(opt.flow_calibration) : 0, 0, pa_mode);
    bool any_ams = false, any_ext = false;
    for (const Mapped& m : mapped)
        (m.tray.tray_index == VIRTUAL_TRAY_MAIN_ID || m.tray.tray_index == VIRTUAL_TRAY_DEPUTY_ID ? any_ext : any_ams) = true;
    job->task_use_ams = obj->HasAms() && opt.use_ams && (any_ams || !any_ext);

    // The files: our own copies, not the plater's (the scene may have changed since the user saw it).
    job->job_data.is_from_plater   = false;
    job->job_data.plate_idx        = plate_idx;
    job->job_data._3mf_path        = file_3mf;
    job->job_data._3mf_config_path = config_3mf;
    PartPlate* plate               = Slice::handles(plate_idx).plate;
    job->plate_data.is_valid        = true;
    job->plate_data.plate_count     = plater().get_partplate_list().get_plate_count();
    job->plate_data.cur_plate_index = plate_idx + 1;
    job->plate_data.bed_type        = plate->get_bed_type(true);
    job->set_print_job_finished_event(EVT_MCP_SEND_DONE);

    auto seen = std::make_shared<Observed>();
    job->on_check_ip_address_fail([seen] {
        std::lock_guard<std::mutex> lock(seen->mutex);
        seen->lan_check_failed = true;
    });
    // Queued, not replaced: replace_job would cancel a transfer the user confirmed earlier.
    queue_job(send_worker(), std::make_shared<ObservedPrintJob>(job, t, seen));
}

// ------------------------------------------------------------------------------------------------
// Host transfer: PrintHost::upload on a thread of its own.
// ------------------------------------------------------------------------------------------------

void host_transfer(const std::shared_ptr<Task>& t, DynamicPrintConfig host_config, fs::path source, std::string upload_name, bool start, bool use_3mf,
                   int plate_idx)
{
    auto job = t->job;
    std::thread([t, job, host_config, source, upload_name, start, use_3mf, plate_idx]() mutable {
        std::unique_ptr<PrintHost> host(PrintHost::get_print_host(&host_config));
        bool        ok = false;
        std::string error;
        if (host == nullptr) {
            error = "this printer's host type cannot receive uploads";
        } else {
            PrintHostUpload data;
            data.use_3mf     = use_3mf;
            data.source_path = source;
            data.upload_path = upload_name;
            data.post_action = start ? PrintHostPostUploadAction::StartPrint : PrintHostPostUploadAction::None;
            if (use_3mf)
                data.extended_info["plateindex"] = std::to_string(plate_idx + 1);
            try {
                ok = host->upload(
                    std::move(data),
                    [job](Http::Progress p, bool& cancel) {
                        cancel = false;
                        if (p.ultotal > 0) {
                            const double pct = 5. + 90. * double(p.ulnow) / double(p.ultotal);
                            wxGetApp().CallAfter([job, pct] {
                                if (job->state == Job::State::Running)
                                    Jobs::instance().update(job, pct, "uploading");
                            });
                        }
                    },
                    [&error](wxString msg) { error = msg.ToUTF8().data(); }, [](wxString, wxString) {});
            } catch (const std::exception& e) {
                error = e.what();
                ok    = false;
            }
        }
        // Never pass on host names or addresses that curl messages contain.
        const std::string h = host_config.opt_string("print_host");
        if (!h.empty())
            for (size_t pos; (pos = error.find(h)) != std::string::npos;)
                error.replace(pos, h.size(), "<printer host>");
        if (const std::string bare = Http::get_host_from_url(h); !bare.empty())
            for (size_t pos; (pos = error.find(bare)) != std::string::npos;)
                error.replace(pos, bare.size(), "<printer host>");
        wxGetApp().CallAfter([t, ok, error, start] {
            --g_transferring;
            audit_later(t->tool, "send_result", t->printer.id, json::object({{"job", t->job->id}, {"ok", ok}}));
            if (ok) {
                json r       = t->summary;
                r["outcome"] = "sent";
                r["message"] = start ? "Uploaded; the host was asked to start printing." : "Uploaded; not started.";
                end_task(t, Job::State::Finished, std::move(r));
            } else {
                end_task(t, Job::State::Failed,
                         failure_result(t, "send_failed", "Upload failed: " + (error.empty() ? std::string("unknown error") : error), "failed",
                                        "Check the printer host and its API key in the printer's Connection settings."));
            }
        });
    }).detach();
}

// ------------------------------------------------------------------------------------------------
// Pieces of the confirmation window
// ------------------------------------------------------------------------------------------------

std::shared_ptr<ThumbnailData> plate_thumbnail(int idx)
{
    PartPlate* plate = Slice::handles(idx).plate;
    if (plate == nullptr || !plate->thumbnail_data.is_valid())
        return nullptr;
    auto copy = std::make_shared<ThumbnailData>();
    copy->set(plate->thumbnail_data.width, plate->thumbnail_data.height);
    copy->pixels = plate->thumbnail_data.pixels;
    return copy;
}

std::string project_name()
{
    std::string name = plater().get_project_name().ToUTF8().data();
    return name.empty() ? "Untitled" : name;
}

void add_plate_facts(ConfirmRequest& req, json& summary, int idx)
{
    const json s = Slice::compact_summary(idx);
    if (s.contains("time"))
        req.facts.emplace_back(_u8L("Estimated time"), s["time"].get<std::string>());
    if (s.contains("filament_g") && s["filament_g"].is_number())
        req.facts.emplace_back(_u8L("Filament"), std::to_string(int(std::lround(s["filament_g"].get<double>()))) + " g");
    for (const char* key : {"time", "time_s", "filament_g", "layers"})
        if (s.contains(key))
            summary[key] = s[key];
}

json printer_problem(const Printer& printer, MachineObject* obj)
{
    (void) printer;
    if (obj == nullptr)
        return json::object({{"code", "printer_offline"}, {"message", "The printer is no longer known to the application."}});
    if (!obj->is_online())
        return json::object({{"code", "printer_offline"}, {"message", "The printer is offline."}, {"hint", "Ask the user to switch it on or check its network."}});
    const std::string state = bambu_state(obj);
    if (state != "idle" && state != "finished" && state != "failed")
        return json::object({{"code", "printer_busy"}, {"message", "The printer is " + state + "."},
                             {"hint", "Wait until it is idle (printer_status), then send."}});
    return nullptr;
}

std::string options_text(const json& o)
{
    std::string s;
    for (auto it = o.begin(); it != o.end(); ++it) {
        if (!s.empty())
            s += ", ";
        s += it.key() + ": " + (it->is_string() ? it->get<std::string>() : it->dump());
    }
    return s;
}

} // namespace

int sends_in_flight() { return int(g_waiting.size()) + (g_asking != nullptr ? 1 : 0) + g_transferring.load(); }

// ------------------------------------------------------------------------------------------------
// print_send
// ------------------------------------------------------------------------------------------------

json print_send(ToolContext& ctx)
{
    require_send(ctx);
    const json& args      = ctx.args();
    const int   plate_idx = Ids::plate(args.contains("plate") ? args["plate"] : json(nullptr), "plate");
    Printer     printer   = resolve(args.value("printer", json(nullptr)));
    const double wait_s   = ctx.wait_s(10., 280.);

    auto refuse = [&](const std::string& code, const std::string& message, const std::string& hint, json details = nullptr) {
        audit(ctx, "send_refused", printer.id, json::object({{"reason", code}, {"plate", plate_idx + 1}}));
        fail(code, message, hint, std::move(details));
    };

    try {
        refuse_scripts();
        require_current_slice(plate_idx);
    } catch (const ToolError& e) {
        audit(ctx, "send_refused", printer.id, json::object({{"reason", e.code()}, {"plate", plate_idx + 1}}));
        throw;
    }

    const json compat = compatibility(printer);
    if (!compat.value("compatible", false))
        refuse("printer_incompatible", "Plate " + std::to_string(plate_idx + 1) + " is sliced for a different printer: " +
                   compat["reasons"].front().get<std::string>() + ".",
               "Select a printer preset that matches this printer, slice again, then send.", compat);

    check_capacity();
    json warnings = json::array();
    for (const json& w : compat.value("warnings", json::array()))
        warnings.push_back(w);

    auto task     = std::make_shared<Task>();
    task->tool    = "print_send";
    task->action  = "send";
    task->printer = printer;
    task->request.printer_name  = printer.name;
    task->request.printer_model = printer.model;
    task->request.headline      = "Plate " + std::to_string(plate_idx + 1) + " of \"" + project_name() + "\"";
    task->request.thumbnail     = plate_thumbnail(plate_idx);
    task->request.timeout_s     = confirm_timeout_s();
    task->summary = json::object({{"printer", base_json(printer)}, {"plate", plate_idx + 1}});
    add_plate_facts(task->request, task->summary, plate_idx);

    const fs::path dir = fs::path(data_dir()) / "mcp" / "send";
    boost::system::error_code ec;
    fs::create_directories(dir, ec);
    const std::string stem = "send_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());

    if (printer.kind == "bambu") {
        MachineObject* obj = machine(printer);
        if (obj == nullptr || !obj->is_online())
            refuse("printer_offline", "Printer \"" + printer.name + "\" is offline.", "Ask the user to switch it on or check its network.");
        if (!bambu_live(obj))
            refuse("printer_status_unknown", "There is no recent status from printer \"" + printer.name + "\".",
                   "Call printer_status for it first (it connects and waits for the status), then send.");
        if (json problem = printer_problem(printer, obj); !problem.is_null())
            refuse(problem["code"], problem["message"], problem.value("hint", ""), bambu_status_json(printer, obj, false));
        if (auto ext = obj->GetExtderSystem(); ext != nullptr && ext->GetTotalExtderCount() > 1)
            refuse("unsupported_printer", "Sending to printers with two nozzles is not supported by the MCP server yet.",
                   "Ask the user to send it from the window (Print plate).");

        // Options.
        const json   o = args.value("options", json::object());
        BambuOptions opt;
        opt.bed_leveling     = o.value("bed_leveling", "auto");
        opt.flow_calibration = o.value("flow_calibration", "auto");
        opt.timelapse        = o.value("timelapse", false);
        opt.use_ams          = o.value("use_ams", true);
        opt.storage          = o.value("storage", "auto");
        if (opt.timelapse && !obj->is_support_timelapse) {
            warnings.push_back("this printer does not record timelapses; timelapse is off");
            opt.timelapse = false;
        }
        if (opt.flow_calibration != "off" && !obj->is_support_flow_calibration && o.contains("flow_calibration"))
            warnings.push_back("this printer has no flow calibration; the option is ignored");
        const auto sd = obj->GetStorage()->get_sdcard_state();
        if (opt.storage == "sdcard" && sd != DevStorage::SdcardState::HAS_SDCARD_NORMAL)
            refuse("no_storage", "The printer has no usable SD card.", "Use storage \"auto\" or ask the user to insert a card.");
        if (opt.storage == "internal" && !obj->can_use_emmc_print())
            refuse("no_storage", "The printer cannot print from internal storage.", "Use storage \"auto\" or \"sdcard\".");
        if (obj->is_lan_mode_printer() && opt.storage == "auto" && !obj->can_use_emmc_print() && sd != DevStorage::SdcardState::HAS_SDCARD_NORMAL &&
            !(sd == DevStorage::SdcardState::HAS_SDCARD_ABNORMAL && wxGetApp().app_config->get("allow_abnormal_storage") == "true"))
            refuse("no_storage", "A LAN print needs a storage card in the printer, and this printer reports none usable.",
                   "Ask the user to insert an SD card.");

        // Filament mapping.
        const std::vector<UsedFilament> used = plate_filaments(plate_idx);
        if (used.empty())
            refuse("not_sliced", "Plate " + std::to_string(plate_idx + 1) + " uses no filament.", "Slice the plate first.");
        std::vector<Mapped> mapped;
        try {
            mapped = map_filaments(obj, used, args.value("ams_mapping", json("auto")), opt.use_ams, warnings);
        } catch (const ToolError& e) {
            audit(ctx, "send_refused", printer.id, json::object({{"reason", e.code()}, {"plate", plate_idx + 1}}));
            throw;
        }
        json mapping_json = json::array();
        for (const Mapped& m : mapped) {
            task->request.rows.push_back(ConfirmRow{m.f.index + 1, m.f.type, m.f.color, m.tray.label, m.tray.type, m.tray.color, m.color_warn});
            mapping_json.push_back(json::object({{"filament", m.f.index + 1},
                                                 {"filament_type", m.f.type},
                                                 {"filament_color", m.f.color},
                                                 {"tray", m.tray.label},
                                                 {"tray_type", m.tray.type},
                                                 {"tray_color", m.tray.color},
                                                 {"color_matches", !m.color_warn}}));
        }
        json options = json::object({{"bed_leveling", opt.bed_leveling},
                                     {"flow_calibration", opt.flow_calibration},
                                     {"timelapse", opt.timelapse},
                                     {"use_ams", opt.use_ams},
                                     {"storage", opt.storage}});
        task->summary["ams_mapping"] = mapping_json;
        task->summary["options"]     = options;
        task->request.facts.emplace_back(_u8L("Connection"), printer.connection == "lan" ? "LAN" : "Bambu cloud");
        task->request.facts.emplace_back(_u8L("Options"), options_text(options));

        // The files, now.
        const fs::path file_3mf    = dir / (stem + ".gcode.3mf");
        const fs::path config_3mf  = dir / (stem + ".config.3mf");
        task->temp_files           = {file_3mf, config_3mf};
        SaveStrategy   strategy    = SaveStrategy::Silence | SaveStrategy::SkipModel | SaveStrategy::WithGcode | SaveStrategy::SkipAuxiliary;
        if (plater().export_3mf(file_3mf, strategy, plate_idx) < 0 || !fs::exists(file_3mf)) {
            cleanup(task);
            refuse("send_failed", "Writing the print file failed.", "Slice the plate again, then send.");
        }
        if (!obj->is_lan_mode_printer()) {
            const SaveStrategy cfg = SaveStrategy::Silence | SaveStrategy::SkipModel | SaveStrategy::WithSliceInfo | SaveStrategy::SkipAuxiliary;
            if (plater().export_3mf(config_3mf, cfg, plate_idx) < 0) {
                cleanup(task);
                refuse("send_failed", "Writing the print configuration failed.", "Slice the plate again, then send.");
            }
        }

        const std::string dev_id = printer.dev_id;
        task->recheck  = [printer] { return printer_problem(printer, machine(printer)); };
        task->transfer = [plate_idx, dev_id, file_3mf, config_3mf, mapped, opt, name = project_name()](std::shared_ptr<Task> t) {
            bambu_transfer(t, plate_idx, dev_id, file_3mf, config_3mf, name, mapped, opt);
        };
    } else {
        // Host printer.
        const Preset* preset = host_preset(printer);
        if (preset == nullptr)
            refuse("printer_not_found", "The printer preset of \"" + printer.name + "\" is gone.", "Call printer_list.");
        if (args.contains("ams_mapping") && args["ams_mapping"].is_array())
            refuse("invalid_argument", "`ams_mapping` is for Bambu Lab printers only.", "Leave it out for this printer.");
        const DynamicPrintConfig& pc = bundle().prints.get_edited_preset().config;
        if (pc.has("post_process") && !pc.option<ConfigOptionStrings>("post_process")->values.empty())
            refuse("post_processing_not_supported",
                   "The process preset has post-processing scripts; the MCP server does not run them, so the file would differ from what "
                   "the window's Send button uploads.",
                   "Ask the user to send it from the window, or remove the scripts.");
        const std::shared_ptr<HostRequest> probe = host_status(printer);
        // Wait for the host's answer (a few seconds at most), then check and queue.
        const json o        = args.value("options", json::object());
        const bool start    = o.value("start_print", true);
        const bool use_3mf  = preset->config.has("use_3mf") && preset->config.opt_bool("use_3mf");
        std::string name    = o.value("file_name", "");
        if (name.find_first_of("/\\:*?\"<>|") != std::string::npos)
            refuse("invalid_argument", "`options.file_name` must be a plain file name.", "Leave out folders and special characters.");
        if (name.empty())
            name = fold_utf8_to_ascii(project_name()) + "_plate_" + std::to_string(plate_idx + 1);
        const std::string ext = use_3mf ? ".gcode.3mf" : ".gcode";
        if (name.size() < ext.size() || name.compare(name.size() - ext.size(), ext.size(), ext) != 0)
            name += ext;

        const fs::path file = dir / (stem + ext);
        task->temp_files    = {file};
        if (use_3mf) {
            SaveStrategy strategy = SaveStrategy::Silence | SaveStrategy::SkipModel | SaveStrategy::WithGcode | SaveStrategy::SkipAuxiliary;
            if (plater().export_3mf(file, strategy, plate_idx) < 0 || !fs::exists(file)) {
                cleanup(task);
                refuse("send_failed", "Writing the print file failed.", "Slice the plate again, then send.");
            }
        } else {
            std::string error;
            if (copy_file(Slice::handles(plate_idx).plate->get_tmp_gcode_path(), file.string(), error, false) != CopyFileResult::SUCCESS) {
                cleanup(task);
                refuse("send_failed", "Copying the G-code failed: " + error, "Slice the plate again, then send.");
            }
        }
        json options = json::object({{"start_print", start}, {"file_name", name}});
        task->summary["options"] = options;
        task->request.facts.emplace_back(_u8L("Connection"), printer.kind);
        task->request.facts.emplace_back(_u8L("File"), name);
        task->request.facts.emplace_back(_u8L("Then"), start ? _u8L("start printing") : _u8L("only upload"));

        DynamicPrintConfig host_config = preset->config;
        task->recheck = [printer]() -> json {
            // Not re-probed (that would make the user wait); the probe before the window counts.
            const json cached = host_cached_status(printer.id);
            if (cached.is_object() && !cached.value("online", true))
                return json::object({{"code", "printer_offline"}, {"message", "The printer host stopped answering."}});
            return nullptr;
        };
        task->transfer = [host_config, file, name, start, use_3mf, plate_idx](std::shared_ptr<Task> t) {
            host_transfer(t, host_config, file, name, start, use_3mf, plate_idx);
        };

        for (const json& w : warnings)
            task->request.warnings.push_back(w.get<std::string>());
        task->summary["warnings"] = warnings;

        // Defer: answer once the probe is back.
        audit(ctx, "send_requested", printer.id, json::object({{"plate", plate_idx + 1}, {"options", options}}));
        Pending pending;
        pending.wait_s = std::max(wait_s, 15.);
        auto started   = std::make_shared<std::shared_ptr<Task>>();
        auto deadline  = std::chrono::steady_clock::now() + std::chrono::duration<double>(wait_s);
        pending.poll   = [task, probe, printer, started, deadline](ToolContext& c, json& result) -> bool {
            if (!*started) {
                if (!probe->done())
                    return false;
                const json  st      = probe->result();
                const bool  unknown = !st.contains("online") || st["online"].is_null();
                if ((!unknown && !st["online"].get<bool>()) || st.value("state", "") == "offline") {
                    cleanup(task);
                    audit(c, "send_refused", printer.id, json::object({{"reason", "printer_offline"}}));
                    fail("printer_offline", "Printer \"" + printer.name + "\" is not reachable: " + st.value("error", "no answer") + ".",
                         "Ask the user to check the printer host is on.", st);
                }
                const std::string state = st.value("state", "unknown");
                if (state == "printing" || state == "paused" || state == "error") {
                    cleanup(task);
                    audit(c, "send_refused", printer.id, json::object({{"reason", "printer_busy"}, {"state", state}}));
                    fail("printer_busy", "Printer \"" + printer.name + "\" is " + state + ".", "Wait until it is idle (printer_status), then send.", st);
                }
                if (unknown) {
                    const std::string w = "OrcaSlicer cannot read the state of this printer host; make sure it is idle and ready before sending";
                    task->request.warnings.push_back(w);
                    task->summary["warnings"].push_back(w);
                }
                check_capacity();
                *started = enqueue(task);
            }
            const auto& job = (*started)->job;
            if (job->state == Job::State::Running && std::chrono::steady_clock::now() < deadline) {
                c.progress(job->percent, job->text);
                return false;
            }
            result = job_json(*job);
            return true;
        };
        pending.on_deadline = [started, task](ToolContext&) -> json {
            if (*started)
                return job_json(*(*started)->job);
            cleanup(task);
            fail("printer_offline", "The printer host did not answer in time.", "Check it is on, then try again.");
        };
        ctx.defer(std::move(pending));
        return nullptr;
    }

    for (const json& w : warnings)
        task->request.warnings.push_back(w.get<std::string>());
    task->summary["warnings"] = warnings;
    audit(ctx, "send_requested", printer.id, json::object({{"plate", plate_idx + 1}, {"summary", task->summary}}));
    enqueue(task);
    ctx.defer(wait_for_job(task->job, wait_s));
    return nullptr;
}

// ------------------------------------------------------------------------------------------------
// printer_control
// ------------------------------------------------------------------------------------------------

json printer_control(ToolContext& ctx)
{
    require_send(ctx);
    Printer           printer = resolve(ctx.args().value("printer", json(nullptr)));
    const std::string action  = ctx.arg("action", "");
    const double      wait_s  = ctx.wait_s(10., 280.);

    if (printer.kind == "bambu") {
        MachineObject* obj = machine(printer);
        if (obj == nullptr || !obj->is_online())
            fail("printer_offline", "Printer \"" + printer.name + "\" is offline.", "Ask the user to check it.");
        if (!bambu_live(obj))
            fail("printer_status_unknown", "There is no recent status from printer \"" + printer.name + "\".", "Call printer_status first.");
        const bool possible = action == "pause" ? obj->can_pause() : action == "resume" ? obj->can_resume() : obj->can_abort();
        if (!possible) {
            audit(ctx, "control_refused", printer.id, json::object({{"action", action}, {"state", bambu_state(obj)}}));
            fail("invalid_state", "Printer \"" + printer.name + "\" cannot " + action + " now (it is " + bambu_state(obj) + ").",
                 "Check printer_status.", bambu_status_json(printer, obj, false));
        }
        if (action != "stop") {
            const int rc = action == "pause" ? obj->command_task_pause() : obj->command_task_resume();
            audit(ctx, "control", printer.id, json::object({{"action", action}, {"rc", rc}}));
            if (rc != 0)
                fail("control_failed", "The printer did not take the " + action + " command (code " + std::to_string(rc) + ").", "Try again.");
            return json::object({{"ok", true}, {"printer", base_json(printer)}, {"action", action}, {"note", "Command sent; printer_status shows the result in a few seconds."}});
        }
    } else if (action != "stop") {
        auto req = host_control(printer, action);
        audit(ctx, "control", printer.id, json::object({{"action", action}}));
        Pending pending;
        pending.wait_s = 15.;
        pending.poll   = [req, printer, action](ToolContext&, json& result) -> bool {
            if (!req->done())
                return false;
            const json r = req->result();
            if (!r.value("ok", false))
                fail("control_failed", "The printer host did not take the " + action + " command: " + r.value("error", "") + ".", "Check printer_status.");
            result = json::object({{"ok", true}, {"printer", base_json(printer)}, {"action", action}});
            return true;
        };
        ctx.defer(std::move(pending));
        return nullptr;
    }

    // stop: the user confirms, like a send.
    check_capacity();
    auto task     = std::make_shared<Task>();
    task->tool    = "printer_control";
    task->action  = "stop";
    task->printer = printer;
    task->request.stop          = true;
    task->request.printer_name  = printer.name;
    task->request.printer_model = printer.model;
    task->request.timeout_s     = confirm_timeout_s();
    task->summary = json::object({{"printer", base_json(printer)}, {"action", "stop"}});
    if (printer.kind == "bambu") {
        MachineObject* obj = machine(printer);
        task->request.headline = obj->subtask_name;
        task->request.facts.emplace_back(_u8L("Progress"), std::to_string(obj->mc_print_percent) + " %");
        task->recheck  = [printer]() -> json {
            MachineObject* o = machine(printer);
            if (o == nullptr || !o->is_online() || !o->can_abort())
                return json::object({{"code", "invalid_state"}, {"message", "The print is no longer running."}});
            return nullptr;
        };
        task->transfer = [](std::shared_ptr<Task> t) {
            MachineObject* o  = machine(t->printer);
            const int      rc = o != nullptr ? o->command_task_abort() : -1;
            --g_transferring;
            audit_later(t->tool, "control", t->printer.id, json::object({{"action", "stop"}, {"rc", rc}}));
            if (rc == 0) {
                json r       = t->summary;
                r["outcome"] = "stopped";
                end_task(t, Job::State::Finished, std::move(r));
            } else {
                end_task(t, Job::State::Failed, failure_result(t, "control_failed", "The printer did not take the stop command.", "failed"));
            }
        };
    } else {
        task->recheck  = nullptr;
        task->transfer = [](std::shared_ptr<Task> t) {
            auto req = host_control(t->printer, "stop");
            PollTimer::start([t, req]() -> bool {
                if (!req->done())
                    return false;
                --g_transferring;
                const json r = req->result();
                audit_later(t->tool, "control", t->printer.id, json::object({{"action", "stop"}, {"ok", r.value("ok", false)}}));
                if (r.value("ok", false)) {
                    json out       = t->summary;
                    out["outcome"] = "stopped";
                    end_task(t, Job::State::Finished, std::move(out));
                } else {
                    end_task(t, Job::State::Failed, failure_result(t, "control_failed", "The host did not take the stop command: " + r.value("error", ""), "failed"));
                }
                return true;
            });
        };
    }
    audit(ctx, "stop_requested", printer.id, json::object());
    enqueue(task);
    ctx.defer(wait_for_job(task->job, wait_s));
    return nullptr;
}

}}}} // namespace Slic3r::GUI::Mcp::Printers
