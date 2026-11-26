// Printer tools: printer_list, printer_status, print_send, printer_control.
//
// All four are off until the user switches them on in Preferences (two switches: read, send); see
// tools/printers/Printers.hpp for what leaves the application and tools/printers/SendConfirm.hpp for
// the confirmation window every send and every stop goes through (unless the user made sends unattended). DESIGN.md D13 has the reasoning.

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/DeviceCore/DevManager.h"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/GUI_App.hpp"

#include "../McpError.hpp"
#include "../McpSchema.hpp"
#include "../McpTool.hpp"
#include "printers/Printers.hpp"

namespace Slic3r { namespace GUI { namespace Mcp {

namespace {

using namespace Printers;

json host_entry(const Printer& p, const json& status)
{
    json out = base_json(p);
    if (!p.status_supported) {
        out["online"]           = nullptr;
        out["state"]            = "unknown";
        out["status_supported"] = false;
        return out;
    }
    if (status.is_object())
        for (auto it = status.begin(); it != status.end(); ++it)
            out[it.key()] = it.value();
    else {
        out["online"] = nullptr;
        out["state"]  = "unknown";
    }
    return out;
}

json bambu_entry(const Printer& p, bool full)
{
    return bambu_status_json(p, machine(p), full);
}

json printer_list(ToolContext& ctx)
{
    require_read(ctx);
    const bool probe = ctx.arg<bool>("probe_hosts", true);
    const std::vector<Printer> printers = all_printers();
    audit(ctx, "list", "", json::object({{"count", printers.size()}}));

    std::string selected;
    if (PresetBundle* pb = wxGetApp().preset_bundle; pb != nullptr)
        selected = pb->printers.get_selected_preset_name();

    // Host printers are asked over HTTP (a few seconds at most); Bambu printers report what the
    // device manager knows.
    auto probes = std::make_shared<std::vector<std::shared_ptr<HostRequest>>>();
    for (const Printer& p : printers)
        probes->push_back(p.kind != "bambu" && p.status_supported && probe ? host_status(p) : nullptr);

    auto build = [printers, probes, selected](bool timed_out) {
        json list = json::array();
        for (size_t i = 0; i < printers.size(); ++i) {
            const Printer& p = printers[i];
            json entry;
            if (p.kind == "bambu")
                entry = bambu_entry(p, false);
            else {
                const auto& req = (*probes)[i];
                json status = req && req->done() ? req->result() : host_cached_status(p.id);
                if (req && !req->done() && timed_out && status.is_null())
                    status = json::object({{"online", nullptr}, {"state", "unknown"}, {"error", "no answer yet"}});
                entry = host_entry(p, status);
                entry.erase("temperatures");
                entry.erase("job");
            }
            entry["compatibility"] = compatibility(p);
            list.push_back(std::move(entry));
        }
        json out = json::object({{"printers", list}, {"selected_printer_preset", selected}});
        if (printers.empty())
            out["note"] = "No printers are known: no Bambu Lab printer was found (LAN discovery or cloud account) and no printer preset "
                          "has a print host.";
        return out;
    };

    bool any = false;
    for (const auto& r : *probes)
        any = any || r != nullptr;
    if (!any)
        return build(false);

    Pending pending;
    pending.wait_s      = 8.;
    pending.poll        = [probes, build](ToolContext&, json& result) -> bool {
        for (const auto& r : *probes)
            if (r && !r->done())
                return false;
        result = build(false);
        return true;
    };
    pending.on_deadline = [build](ToolContext&) { return build(true); };
    ctx.defer(std::move(pending));
    return nullptr;
}

json printer_status(ToolContext& ctx)
{
    require_read(ctx);
    const Printer p      = resolve(ctx.args().value("printer", json(nullptr)));
    const double  wait_s = ctx.wait_s(10., 30.);
    audit(ctx, "status", p.id, json::object());

    if (p.kind != "bambu") {
        if (!p.status_supported) {
            json out = host_entry(p, nullptr);
            out["compatibility"] = compatibility(p);
            out["note"] = "The MCP server reads status from OctoPrint and Moonraker hosts only.";
            return out;
        }
        auto    req = host_status(p);
        Pending pending;
        pending.wait_s = std::max(wait_s, 8.);
        pending.poll   = [req, p](ToolContext&, json& result) -> bool {
            if (!req->done())
                return false;
            result                  = host_entry(p, req->result());
            result["compatibility"] = compatibility(p);
            return true;
        };
        pending.on_deadline = [p](ToolContext&) {
            json out = host_entry(p, json::object({{"online", nullptr}, {"state", "unknown"}, {"error", "the host did not answer in time"}}));
            out["compatibility"] = compatibility(p);
            return out;
        };
        ctx.defer(std::move(pending));
        return nullptr;
    }

    MachineObject* obj = machine(p);
    if (obj == nullptr)
        fail("printer_not_found", "The application no longer knows printer \"" + p.name + "\".", "Call printer_list.");
    auto finish = [p](const std::string& note) {
        json out             = bambu_entry(p, true);
        out["compatibility"] = compatibility(p);
        if (!note.empty())
            out["note"] = note;
        return out;
    };
    if (!obj->is_online() || bambu_live(obj))
        return finish("");

    // Status reaches the application only from the printer selected in the Device tab. Select this
    // one, as a click in the Device tab would, and wait for its first report.
    DeviceManager* dev = wxGetApp().getDeviceManager();
    std::string    note;
    if (dev != nullptr) {
        MachineObject* selected = dev->get_selected_machine();
        if (selected == nullptr || selected->get_dev_id() != p.dev_id) {
            if (!dev->set_selected_machine(p.dev_id))
                return finish("The application could not connect to this printer; the status shown is the last one received.");
            note = "Selected this printer in the Device tab to receive its status (the previously selected printer was disconnected).";
            audit(ctx, "status_connect", p.id, json::object());
        }
    }
    Pending pending;
    pending.wait_s      = wait_s;
    pending.poll        = [p, finish, note](ToolContext& c, json& result) -> bool {
        MachineObject* o = machine(p);
        if (o == nullptr || !bambu_live(o)) {
            c.progress(0., "waiting for the printer's status");
            return false;
        }
        result = finish(note);
        return true;
    };
    pending.on_deadline = [finish, note](ToolContext&) {
        return finish((note.empty() ? std::string() : note + " ") +
                      "No status arrived in time; the values shown are the last ones received (see status_age_s). Call again to wait longer.");
    };
    ctx.defer(std::move(pending));
    return nullptr;
}

} // namespace

MCP_TOOL_GROUP(Printers)
{
    {
        ToolDef tool;
        tool.name  = "printer_list";
        tool.title = "List printers";
        tool.description =
            "Lists the printers the application knows: Bambu Lab printers (LAN discovery and the user's cloud account) and printer presets "
            "with a print host (OctoPrint, Moonraker, PrusaLink ...). Needs the user's Preferences switch \"Allow AI agents to read printer "
            "status\" (off by default; error printer_access_disabled otherwise; no tool can change it). Each entry: `id` (opaque, stable; "
            "pass it to the other printer tools), name, kind (bambu or the host type), connection (lan, cloud, host), model, online, state "
            "(idle, preparing, printing, paused, finished, failed, error, offline, unknown), nozzle, filament `trays` (label A1..D4 / HT1.. / "
            "ext / ext2, type, color, remain_percent, empty) and `compatibility` with the selected printer preset ({compatible, reasons, "
            "warnings}). Bambu printers that are not selected in the Device tab show their last known status (status_age_s; null = none "
            "yet): call printer_status for a live one. Host printers are asked over HTTP unless probe_hosts is false. Never returns access "
            "codes, serial numbers, addresses or account data.";
        tool.input_schema = Schema::parse(R"JSON({
            "type": "object",
            "properties": {
                "probe_hosts": {"type": "boolean", "default": true, "description": "Ask OctoPrint / Moonraker hosts for their state (a few seconds). false: last known state."}
            },
            "additionalProperties": false})JSON", "printer_list");
        tool.read_only  = true;
        tool.idempotent = true;
        tool.handler    = printer_list;
        registry.add(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name  = "printer_status";
        tool.title = "Printer status";
        tool.description =
            "Live status of one printer: online, state, job {name, percent, layer [current, total], remaining_s}, temperatures (nozzle, "
            "bed; current and target), nozzle, filament trays, errors (HMS codes with messages, print errors) and compatibility "
            "with the selected printer preset. Needs the Preferences switch \"Allow AI agents to read printer status\". A Bambu Lab printer "
            "reports status only while it is the printer selected in the Device tab: this tool selects it (as a click there would, which "
            "disconnects the previously selected LAN printer) and waits up to wait_s for its first report; `live` says whether the values "
            "are current. Host printers (OctoPrint, Moonraker) are asked over HTTP; other host types report state unknown.";
        tool.input_schema = Schema::parse(R"JSON({
            "type": "object",
            "properties": {
                "printer": {"type": "string", "description": "Printer id from printer_list, or its exact name."},
                "wait_s": {"type": "number", "minimum": 0, "maximum": 30, "default": 10, "description": "Seconds to wait for a live status."}
            },
            "required": ["printer"],
            "additionalProperties": false})JSON", "printer_status");
        tool.read_only  = true;
        tool.idempotent = true;
        tool.handler    = printer_status;
        registry.add(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name  = "print_send";
        tool.title = "Send a sliced plate to a printer";
        tool.description =
            "Sends a sliced plate to a printer, after the user confirms it in a window of the application (the user sees the plate picture, "
            "the printer, the filament-to-tray mapping, time, weight and options, and clicks Send or Cancel; nothing is sent otherwise). "
            "When the user has also switched on \"Send print jobs without asking me\" (app_status printers.send_unattended), no window "
            "opens and the plate is sent as soon as the checks pass: only send what the user asked for. "
            "Needs the Preferences switch \"Allow AI agents to send print jobs\" (off by default; printer_access_disabled otherwise; no "
            "tool can change it, so ask the user). Checks first and fails without asking when: the plate is not sliced (not_sliced) or "
            "changed since (slice_stale), a slice runs (busy_slicing), the printer is offline (printer_offline), has no recent status "
            "(printer_status_unknown: call printer_status), is busy (printer_busy), does not match the printer preset the plate was sliced "
            "for (printer_incompatible), a mapped tray is empty (tray_empty) or holds another material (filament_type_mismatch), or more "
            "requests wait for the user (rate_limited). A different colour is only a warning, shown to the user. Bambu Lab printers: "
            "`ams_mapping` \"auto\" (the application's own matching) or [{filament: 1-based project filament, tray: label from "
            "printer_status}] for every filament the plate uses. Host printers upload the G-code and start it unless "
            "options.start_print is false. Runs as a job: returns within wait_s with {status, job, text} (text says while it waits for "
            "the user), then poll job_status. The finished job's result has outcome sent; a failed one has outcome declined (the user "
            "said no: do not retry unless asked), timed_out (nobody answered in 5 minutes), not_sent or failed, with an error. job_cancel "
            "withdraws a request the user has not answered.";
        tool.input_schema = Schema::parse(R"JSON({
            "type": "object",
            "properties": {
                "plate": {"$ref": "#/$defs/PlateRef", "description": "Default: the current plate."},
                "printer": {"type": "string", "description": "Printer id from printer_list, or its exact name."},
                "ams_mapping": {
                    "oneOf": [
                        {"const": "auto"},
                        {"type": "array", "minItems": 1, "maxItems": 32, "items": {
                            "type": "object",
                            "properties": {
                                "filament": {"type": "integer", "minimum": 1, "maximum": 32, "description": "Project filament number (1-based)."},
                                "tray": {"type": "string", "minLength": 1, "maxLength": 8, "description": "Tray label: A1..D4, HT1.., ext, ext2."}
                            },
                            "required": ["filament", "tray"],
                            "additionalProperties": false}}
                    ],
                    "default": "auto",
                    "description": "Bambu Lab printers: which tray feeds each filament the plate uses."
                },
                "options": {
                    "type": "object",
                    "properties": {
                        "bed_leveling": {"enum": ["auto", "on", "off"], "default": "auto"},
                        "flow_calibration": {"enum": ["auto", "on", "off"], "default": "auto"},
                        "timelapse": {"type": "boolean", "default": false},
                        "use_ams": {"type": "boolean", "default": true, "description": "false: print from the external spool."},
                        "storage": {"enum": ["auto", "sdcard", "internal"], "default": "auto"},
                        "start_print": {"type": "boolean", "default": true, "description": "Host printers: start printing after the upload."},
                        "file_name": {"type": "string", "maxLength": 120, "description": "Host printers: file name on the host. Default <project>_plate_<n>.gcode."}
                    },
                    "additionalProperties": false
                },
                "wait_s": {"type": "number", "minimum": 0, "maximum": 280, "default": 10, "description": "Seconds to wait for the outcome before returning the running job."}
            },
            "required": ["printer"],
            "additionalProperties": false})JSON", "print_send");
        tool.read_only      = false;
        tool.destructive    = false;
        tool.idempotent     = false;
        tool.accepts_if_rev = false;
        tool.handler        = Printers::print_send;
        registry.add(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name  = "printer_control";
        tool.title = "Pause, resume or stop a print";
        tool.description =
            "Pauses, resumes or stops the print running on a printer. Needs the Preferences switch \"Allow AI agents to send print jobs\". "
            "pause and resume are sent at once and return {ok}; check the effect with printer_status. stop asks the user in a window of "
            "the application first, like print_send (not when sends are unattended), and runs as a job (outcome stopped, or declined / timed_out). Fails with "
            "invalid_state when a Bambu Lab printer cannot do it now (nothing printing, already paused ...); a host that refuses "
            "gives control_failed.";
        tool.input_schema = Schema::parse(R"JSON({
            "type": "object",
            "properties": {
                "printer": {"type": "string", "description": "Printer id from printer_list, or its exact name."},
                "action": {"enum": ["pause", "resume", "stop"]},
                "wait_s": {"type": "number", "minimum": 0, "maximum": 280, "default": 10, "description": "stop: seconds to wait for the user's answer."}
            },
            "required": ["printer", "action"],
            "additionalProperties": false})JSON", "printer_control");
        tool.destructive    = true;
        tool.accepts_if_rev = false;
        tool.handler        = Printers::printer_control;
        registry.add(std::move(tool));
    }
}

}}} // namespace Slic3r::GUI::Mcp
