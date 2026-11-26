#ifndef slic3r_Mcp_Printers_hpp_
#define slic3r_Mcp_Printers_hpp_

// Printers for the MCP tools printer_list, printer_status, print_send and printer_control.
//
// Two kinds of printer are known to the application:
//   - Bambu Lab printers from the device manager: LAN printers found by discovery (SSDP) and
//     printers bound to the user's cloud account. Their status arrives over the connection of the
//     selected printer only (one LAN connection at a time), so the status of the others is the
//     last one received, with its age.
//   - Host printers: printer presets with a print host (OctoPrint, Moonraker, PrusaLink ...), the
//     "physical printers" of this fork. Their status is fetched over HTTP when asked for.
//
// What leaves the application: names, models, states, temperatures, filament trays. Never access
// codes, API keys, passwords, serial numbers, IP addresses or host names, or account data. A
// printer is named by an opaque id: "prn_<10 hex>" for a Bambu printer, "host_<10 hex>" for a host
// printer, a salted hash (the salt lives in <data_dir>/mcp/printer_salt and never leaves).
//
// Access: Preferences switches, all off by default, that only the Preferences dialog writes:
//   mcp_printer_read              printer_list, printer_status
//   mcp_printer_send              print_send, printer_control
//   mcp_printer_send_unattended   sends and stops go ahead without the confirmation window
//                                 (only while mcp_printer_send is on)
// Every call of these tools, allowed or refused, is written to the MCP journal (`history`).
//
// Main thread only, except where noted.

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../../McpJson.hpp"

namespace Slic3r {
class MachineObject;
class Preset;
namespace GUI { namespace Mcp {

class ToolContext;

namespace Printers {

// ---------------------------------------------------------------------------------------------
// Access, ids, audit (PrinterAccess.cpp)
// ---------------------------------------------------------------------------------------------

inline constexpr const char* PREF_READ = "mcp_printer_read";
inline constexpr const char* PREF_SEND = "mcp_printer_send";
inline constexpr const char* PREF_SEND_UNATTENDED = "mcp_printer_send_unattended";

bool read_allowed();
bool send_allowed();
// The user lets sends and stops through without asking (send_allowed() and its own switch).
bool send_unattended();
// Fail with printer_access_disabled (and journal the refusal) unless the switch is on.
void require_read(ToolContext& ctx);
void require_send(ToolContext& ctx);

// "prn_" / "host_" + 10 hex of a salted hash of `key`. Stable across sessions of one data dir.
std::string make_id(const std::string& prefix, const std::string& key);

// One journal entry: scope {"printer": id}, key = action ("list", "status", "refused", "send_requested",
// "send_asked", "send_answer", "send_result", "send_refused", "control" ...; docs/mcp/tools-printers.md),
// after = details. Not revertible.
void audit(ToolContext& ctx, const std::string& action, const std::string& printer_id, json details);
// The same, from code that runs outside a call (a send job ending later).
void audit_later(const std::string& tool, const std::string& action, const std::string& printer_id, json details);

// Seconds a send waits for the user's answer. 300; a test build may shorten it with
// ORCA_MCP_CONFIRM_TIMEOUT_S.
double confirm_timeout_s();

// ---------------------------------------------------------------------------------------------
// Printers (PrinterRegistry.cpp)
// ---------------------------------------------------------------------------------------------

struct Tray
{
    std::string label;          // "A1".."D4" (AMS unit letter, slot number), "HT1".. for single-slot units, "ext"
    int         tray_index { -1 }; // the printer's global tray index (ams_id*4+slot; 254/255 external)
    std::string ams_id;
    std::string slot_id;
    bool        empty { true };
    std::string type;           // "PLA", "PETG" ...
    std::string color;          // "#RRGGBB" ("" unknown)
    std::string color_raw;      // as the printer reports it ("RRGGBBAA"), for the print job's mapping
    std::string filament_id;    // the slicer's filament id ("GFA00"), may be empty
    int         remain { -1 };  // percent, -1 unknown
};

struct Printer
{
    std::string id;
    std::string name;
    std::string kind;        // "bambu" or the host type: "octoprint", "moonraker", "prusalink" ...
    std::string connection;  // bambu: "lan" | "cloud"; host printers: "host"
    std::string model;       // display name ("Bambu Lab A1") or the preset's printer model
    std::string model_id;    // bambu: printer_type ("N2S"); host: printer_model
    // Bambu
    std::string dev_id;      // internal only, never returned
    // Host
    std::string preset_name; // the printer preset holding the host settings
    bool        status_supported { true };
};

// Every printer the application knows: Bambu printers first (by name), then host printers.
std::vector<Printer> all_printers();
// By id, or by exact (case-insensitive) name. Fails with printer_not_found / ambiguous_printer.
Printer resolve(const json& ref);

// The live machine object of a Bambu printer, or nullptr if the device manager forgot it.
MachineObject* machine(const Printer& printer);
// The host printer's preset config (printer collection), or nullptr.
const Preset* host_preset(const Printer& printer);

// Normalised state from a Bambu machine: "idle", "preparing", "printing", "paused", "finished",
// "failed", "offline", "unknown" (no status received yet).
std::string bambu_state(MachineObject* obj);
// Seconds since the last status push, or -1 if none.
double      bambu_status_age_s(MachineObject* obj);
// The machine is connected and pushed status in the last 15 s.
bool        bambu_live(MachineObject* obj);
std::vector<Tray> bambu_trays(MachineObject* obj);
json        tray_json(const Tray& tray);

// Compatibility of a printer with the selected printer preset (the one the plate is sliced for):
// {"compatible": bool, "reasons": [...]}. Model and nozzle diameter; unknown values do not fail.
json compatibility(const Printer& printer);

// Base JSON of a printer (id, name, kind, connection, model). No secrets.
json base_json(const Printer& printer);
// Status JSON of a Bambu printer: online, state, live, status_age_s, job, temperatures, nozzle,
// trays, errors.
json bambu_status_json(const Printer& printer, MachineObject* obj, bool full);

// ---------------------------------------------------------------------------------------------
// Host printers over HTTP (HostClient.cpp). Requests run on the HTTP client's own threads; the
// result object is filled from there and read on the main thread under its mutex.
// ---------------------------------------------------------------------------------------------

class HostRequest
{
public:
    // Read on the main thread; complete() is called once from the HTTP thread (or the main thread).
    bool done() const;
    json result() const;     // {"online", "state", "job", "temperatures", "errors"} or {"online": false, "error"}
    void complete(json result);

private:
    mutable std::mutex m_mutex;
    bool               m_done { false };
    json               m_result;
};

// Starts a status probe of a host printer (OctoPrint, Moonraker). Returns immediately.
std::shared_ptr<HostRequest> host_status(const Printer& printer);
// Pause / resume / stop on a host printer. Returns immediately.
std::shared_ptr<HostRequest> host_control(const Printer& printer, const std::string& action);
// Cached result of the last finished probe of this printer, or null.
json host_cached_status(const std::string& printer_id);

// ---------------------------------------------------------------------------------------------
// Sending (PrintSend.cpp)
// ---------------------------------------------------------------------------------------------

// print_send / printer_control handlers.
json print_send(ToolContext& ctx);
json printer_control(ToolContext& ctx);
// Sends waiting for the user's answer or transferring (for app_status / tests).
int  sends_in_flight();

}}}} // namespace Slic3r::GUI::Mcp::Printers

#endif
