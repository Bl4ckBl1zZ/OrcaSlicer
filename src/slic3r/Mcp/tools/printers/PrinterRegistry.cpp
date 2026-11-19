#include "Printers.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <set>

#include <boost/algorithm/string.hpp>

#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/DeviceCore/DevBed.h"
#include "slic3r/GUI/DeviceCore/DevConfigUtil.h"
#include "slic3r/GUI/DeviceCore/DevDefs.h"
#include "slic3r/GUI/DeviceCore/DevExtruderSystem.h"
#include "slic3r/GUI/DeviceCore/DevFilaSystem.h"
#include "slic3r/GUI/DeviceCore/DevHMS.h"
#include "slic3r/GUI/DeviceCore/DevManager.h"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/HMS.hpp"

#include "../../McpError.hpp"

namespace Slic3r { namespace GUI { namespace Mcp { namespace Printers {

namespace {

PresetBundle& bundle() { return *wxGetApp().preset_bundle; }

std::string lower(std::string s)
{
    boost::algorithm::to_lower(s);
    return s;
}

double round1(double v) { return std::round(v * 10.) / 10.; }

std::string nozzle_type_name(NozzleType type)
{
    switch (type) {
    case ntHardenedSteel: return "hardened_steel";
    case ntStainlessSteel: return "stainless_steel";
    case ntTungstenCarbide: return "tungsten_carbide";
    case ntBrass: return "brass";
    case ntE3D: return "e3d";
    default: return "";
    }
}

// "RRGGBBAA" or "#RRGGBB(AA)" -> "#RRGGBB" (upper case); "" when not a colour.
std::string to_rgb(std::string color)
{
    if (!color.empty() && color[0] == '#')
        color.erase(0, 1);
    if (color.size() < 6)
        return {};
    for (size_t i = 0; i < 6; ++i)
        if (!std::isxdigit(static_cast<unsigned char>(color[i])))
            return {};
    return "#" + boost::algorithm::to_upper_copy(color.substr(0, 6));
}

std::string host_type_of(const DynamicPrintConfig& config)
{
    if (!config.has("host_type"))
        return "octoprint"; // PrintHost::get_print_host's default
    return config.opt_serialize("host_type");
}

void add_bambu(std::vector<Printer>& out, std::set<std::string>& seen, MachineObject* obj, const std::string& connection)
{
    if (obj == nullptr || obj->get_dev_id().empty() || !seen.insert(obj->get_dev_id()).second)
        return;
    Printer p;
    p.dev_id     = obj->get_dev_id();
    p.id         = make_id("prn", p.dev_id);
    p.name       = obj->get_dev_name();
    p.kind       = "bambu";
    p.connection = connection.empty() ? obj->connection_type() : connection;
    if (p.connection != "lan" && p.connection != "cloud")
        p.connection = obj->is_lan_mode_printer() ? "lan" : "cloud";
    p.model_id = obj->printer_type;
    p.model    = DevPrinterConfigUtil::get_printer_display_name(obj->printer_type);
    if (p.model.empty())
        p.model = obj->printer_type;
    out.push_back(std::move(p));
}

} // namespace

std::vector<Printer> all_printers()
{
    std::vector<Printer> bambu;
    std::set<std::string> seen;
    if (DeviceManager* dev = wxGetApp().getDeviceManager(); dev != nullptr) {
        // A printer bound to the account and also found on the LAN is listed once, as a LAN
        // printer when the user connected to it in LAN mode, else as a cloud printer.
        for (auto& [id, obj] : dev->get_local_machinelist())
            if (obj != nullptr && obj->is_lan_mode_printer())
                add_bambu(bambu, seen, obj, "lan");
        for (auto& [id, obj] : dev->get_user_machinelist())
            add_bambu(bambu, seen, obj, "cloud");
        for (auto& [id, obj] : dev->get_local_machinelist())
            add_bambu(bambu, seen, obj, "");
    }
    std::sort(bambu.begin(), bambu.end(), [](const Printer& a, const Printer& b) {
        return lower(a.name) != lower(b.name) ? lower(a.name) < lower(b.name) : a.id < b.id;
    });

    std::vector<Printer> hosts;
    PresetCollection&    printers = bundle().printers;
    const std::string    selected = printers.get_selected_preset_name();
    std::set<std::string> names;
    for (const Preset& preset : printers) {
        // The selected preset's host settings are those of the edited copy (what the window's
        // Send button uses).
        const Preset&             effective = preset.name == selected ? printers.get_edited_preset() : preset;
        const DynamicPrintConfig& config    = effective.config;
        if (!config.has("print_host") || config.opt_string("print_host").empty() || !names.insert(preset.name).second)
            continue;
        if (config.has("printer_technology") && config.opt_serialize("printer_technology") != "FFF")
            continue;
        Printer p;
        p.preset_name = preset.name;
        p.id          = make_id("host", preset.name);
        p.name        = preset.name;
        p.kind        = host_type_of(config);
        p.connection  = "host";
        p.model_id    = config.has("printer_model") ? config.opt_string("printer_model") : "";
        p.model       = p.model_id.empty() ? preset.name : p.model_id;
        p.status_supported = p.kind == "octoprint" || p.kind == "moonraker";
        hosts.push_back(std::move(p));
    }
    std::sort(hosts.begin(), hosts.end(), [](const Printer& a, const Printer& b) { return lower(a.name) < lower(b.name); });

    bambu.insert(bambu.end(), hosts.begin(), hosts.end());
    return bambu;
}

Printer resolve(const json& ref)
{
    if (!ref.is_string() || ref.get<std::string>().empty())
        fail("invalid_argument", "`printer` must be a printer id from printer_list or a printer's exact name.", "Call printer_list.");
    const std::string    key      = ref.get<std::string>();
    std::vector<Printer> printers = all_printers();
    for (Printer& p : printers)
        if (p.id == key)
            return p;
    std::vector<Printer> by_name;
    for (Printer& p : printers)
        if (lower(p.name) == lower(key))
            by_name.push_back(p);
    if (by_name.size() == 1)
        return by_name.front();
    if (by_name.size() > 1) {
        json ids = json::array();
        for (const Printer& p : by_name)
            ids.push_back(p.id);
        fail("ambiguous_printer", "Several printers are called \"" + key + "\".", "Use the printer's id.", json::object({{"ids", ids}}));
    }
    json known = json::array();
    for (const Printer& p : printers)
        known.push_back(json::object({{"id", p.id}, {"name", p.name}}));
    fail("printer_not_found", "No printer with id or name \"" + key + "\".",
         known.empty() ? "The application knows no printer: Bambu Lab printers appear once discovered on the LAN or bound to the "
                         "user's account; other printers need a printer preset with a print host (the window's Connection dialog)."
                       : "Use an id from printer_list.",
         json::object({{"printers", known}}));
}

MachineObject* machine(const Printer& printer)
{
    DeviceManager* dev = wxGetApp().getDeviceManager();
    if (dev == nullptr || printer.dev_id.empty())
        return nullptr;
    if (MachineObject* obj = dev->get_local_machine(printer.dev_id); obj != nullptr)
        return obj;
    auto users = dev->get_user_machinelist();
    auto it    = users.find(printer.dev_id);
    return it == users.end() ? nullptr : it->second;
}

const Preset* host_preset(const Printer& printer)
{
    if (printer.preset_name.empty())
        return nullptr;
    PresetCollection& printers = bundle().printers;
    if (printers.get_selected_preset_name() == printer.preset_name)
        return &printers.get_edited_preset();
    return printers.find_preset(printer.preset_name, false);
}

double bambu_status_age_s(MachineObject* obj)
{
    if (obj == nullptr || obj->last_push_time == std::chrono::system_clock::time_point {})
        return -1.;
    const double s = std::chrono::duration<double>(std::chrono::system_clock::now() - obj->last_push_time).count();
    return s < 0. ? 0. : s;
}

bool bambu_live(MachineObject* obj)
{
    if (obj == nullptr || !obj->is_connected())
        return false;
    const double age = bambu_status_age_s(obj);
    return age >= 0. && age < 15.;
}

std::string bambu_state(MachineObject* obj)
{
    if (obj == nullptr)
        return "unknown";
    if (!obj->is_online())
        return "offline";
    if (bambu_status_age_s(obj) < 0.)
        return "unknown";
    const std::string s = obj->print_status;
    if (s == "RUNNING")
        return "printing";
    if (s == "PAUSE")
        return "paused";
    if (s == "PREPARE" || s == "SLICING" || s == "INIT")
        return "preparing";
    if (s == "FINISH")
        return "finished";
    if (s == "FAILED")
        return "failed";
    return "idle";
}

std::vector<Tray> bambu_trays(MachineObject* obj)
{
    std::vector<Tray> out;
    if (obj == nullptr)
        return out;
    if (auto fila = obj->GetFilaSystem(); fila) {
        for (auto& [ams_key, ams] : fila->GetAmsList()) {
            if (ams == nullptr)
                continue;
            int ams_num = -1;
            try {
                ams_num = std::stoi(ams->GetAmsId());
            } catch (...) {
                continue;
            }
            for (auto& [tray_key, tray] : ams->GetTrays()) {
                if (tray == nullptr)
                    continue;
                int slot = -1;
                try {
                    slot = std::stoi(tray->id);
                } catch (...) {
                    continue;
                }
                Tray t;
                t.ams_id  = ams->GetAmsId();
                t.slot_id = tray->id;
                // Tray index as DevMappingUtil computes it (the index the printer's mapping uses).
                if (ams->GetAmsType() == DevAms::N3S) {
                    // Single-slot units (AMS HT) number from 128.
                    t.label      = "HT" + std::to_string(ams_num - 127);
                    t.tray_index = ams_num + slot;
                } else {
                    t.label      = std::string(1, static_cast<char>('A' + std::min(ams_num, 25))) + std::to_string(slot + 1);
                    t.tray_index = ams->IsAmsLiteMixed() ? AMS_LITE_MIXED_TRAY_INDEX_OFFSET + slot : ams_num * 4 + slot;
                }
                t.empty = !tray->is_exists || tray->m_fila_type.empty();
                if (!t.empty) {
                    t.type        = tray->m_fila_type;
                    t.color_raw   = tray->color;
                    t.color       = to_rgb(tray->color);
                    t.filament_id = tray->setting_id;
                    t.remain      = tray->remain;
                }
                out.push_back(std::move(t));
            }
        }
    }
    for (const DevAmsTray& tray : obj->vt_slot) {
        int id = -1;
        try {
            id = std::stoi(tray.id);
        } catch (...) {
            continue;
        }
        Tray t;
        t.label      = id == VIRTUAL_TRAY_DEPUTY_ID ? "ext2" : "ext";
        t.tray_index = id;
        t.ams_id     = tray.id;
        t.slot_id    = "0";
        t.empty      = tray.m_fila_type.empty();
        if (!t.empty) {
            t.type        = tray.m_fila_type;
            t.color_raw   = tray.color;
            t.color       = to_rgb(tray.color);
            t.filament_id = tray.setting_id;
            t.remain      = tray.remain;
        }
        out.push_back(std::move(t));
    }
    return out;
}

json tray_json(const Tray& t)
{
    json j = json::object({{"tray", t.label}, {"empty", t.empty}});
    if (!t.empty) {
        j["type"] = t.type;
        if (!t.color.empty())
            j["color"] = t.color;
        if (t.remain >= 0)
            j["remain_percent"] = t.remain;
        if (!t.filament_id.empty())
            j["filament_id"] = t.filament_id;
    }
    return j;
}

json compatibility(const Printer& printer)
{
    PresetBundle&     pb       = bundle();
    const Preset&     selected = pb.printers.get_edited_preset();
    json              reasons  = json::array();
    json              warnings = json::array();
    const auto*       nd       = selected.config.option<ConfigOptionFloats>("nozzle_diameter");
    const double      nozzle   = nd != nullptr && !nd->values.empty() ? nd->values.front() : 0.;

    if (printer.kind == "bambu") {
        MachineObject* obj = machine(printer);
        if (!pb.is_bbl_vendor())
            reasons.push_back("the selected printer preset \"" + selected.name + "\" is not a Bambu Lab printer");
        else if (obj != nullptr) {
            const std::string source = const_cast<Preset&>(selected).get_printer_type(&pb);
            if (!DevPrinterConfigUtil::is_printer_model_compatible(source, *obj))
                reasons.push_back("the plate is sliced for " + selected.config.opt_string("printer_model") + ", the printer is a " + printer.model);
            else if (!source.empty() && !obj->printer_type.empty() && source != obj->printer_type)
                warnings.push_back("the plate is sliced for " + selected.config.opt_string("printer_model") + ", a compatible model");
            if (auto ext = obj->GetExtderSystem(); ext != nullptr && ext->GetTotalExtderCount() > 0) {
                const double machine_nozzle = ext->GetNozzleDiameter(0);
                if (machine_nozzle > 0. && nozzle > 0. && std::abs(machine_nozzle - nozzle) > 0.01)
                    reasons.push_back("nozzle " + std::to_string(machine_nozzle).substr(0, 4) + " mm on the printer, the preset is for " +
                                      std::to_string(nozzle).substr(0, 4) + " mm");
            }
        }
    } else if (const Preset* host = host_preset(printer); host != nullptr) {
        if (host->name != selected.name) {
            const std::string a = selected.config.has("printer_model") ? selected.config.opt_string("printer_model") : "";
            const std::string b = host->config.has("printer_model") ? host->config.opt_string("printer_model") : "";
            if (a.empty() || b.empty() || a != b)
                reasons.push_back("the plate is sliced with printer preset \"" + selected.name + "\", this printer is preset \"" + host->name + "\"");
            const auto*  hd           = host->config.option<ConfigOptionFloats>("nozzle_diameter");
            const double host_nozzle  = hd != nullptr && !hd->values.empty() ? hd->values.front() : 0.;
            if (host_nozzle > 0. && nozzle > 0. && std::abs(host_nozzle - nozzle) > 0.01)
                reasons.push_back("nozzle " + std::to_string(host_nozzle).substr(0, 4) + " mm on the printer, the preset is for " +
                                  std::to_string(nozzle).substr(0, 4) + " mm");
        }
    }
    json out = json::object({{"compatible", reasons.empty()}, {"selected_preset", selected.name}});
    if (!reasons.empty())
        out["reasons"] = reasons;
    if (!warnings.empty())
        out["warnings"] = warnings;
    return out;
}

json base_json(const Printer& p)
{
    return json::object({{"id", p.id}, {"name", p.name}, {"kind", p.kind}, {"connection", p.connection}, {"model", p.model}});
}

json bambu_status_json(const Printer& printer, MachineObject* obj, bool full)
{
    json out = base_json(printer);
    if (obj == nullptr) {
        out["online"] = false;
        out["state"]  = "unknown";
        return out;
    }
    const double age   = bambu_status_age_s(obj);
    const std::string state = bambu_state(obj);
    out["online"]       = obj->is_online();
    out["state"]        = state;
    out["live"]         = bambu_live(obj);
    out["status_age_s"] = age < 0. ? json(nullptr) : json(std::round(age));
    if (age < 0.) {
        out["note"] = "No status received from this printer yet. printer_status connects to it.";
        return out;
    }

    // Nozzle.
    if (auto ext = obj->GetExtderSystem(); ext != nullptr && ext->GetTotalExtderCount() > 0) {
        json nozzles = json::array();
        for (int i = 0; i < ext->GetTotalExtderCount(); ++i) {
            json n = json::object();
            if (const double d = ext->GetNozzleDiameter(i); d > 0.)
                n["diameter_mm"] = round1(d * 10.) / 10.;
            if (const std::string t = nozzle_type_name(ext->GetNozzleType(i)); !t.empty())
                n["type"] = t;
            nozzles.push_back(std::move(n));
        }
        out["nozzle"] = nozzles.size() == 1 ? nozzles.front() : nozzles;
    }

    // Filament.
    json trays = json::array();
    for (const Tray& t : bambu_trays(obj))
        trays.push_back(tray_json(t));
    out["trays"] = std::move(trays);

    // Job.
    if (state == "printing" || state == "paused" || state == "preparing" || (full && (state == "finished" || state == "failed"))) {
        json job = json::object();
        job["name"]    = obj->subtask_name;
        job["percent"] = obj->mc_print_percent;
        if (obj->total_layers > 0)
            job["layer"] = json::array({obj->curr_layer, obj->total_layers});
        if (obj->mc_left_time > 0)
            job["remaining_s"] = obj->mc_left_time;
        out["job"] = std::move(job);
    }

    // Temperatures and errors.
    if (full) {
        json temps = json::object();
        if (auto ext = obj->GetExtderSystem(); ext != nullptr && ext->GetTotalExtderCount() > 0)
            temps["nozzle"] = json::object({{"current", ext->GetNozzleTempCurrent(0)}, {"target", ext->GetNozzleTempTarget(0)}});
        if (DevBed* bed = obj->GetBed(); bed != nullptr)
            temps["bed"] = json::object({{"current", round1(bed->GetBedTemp())}, {"target", round1(bed->GetBedTempTarget())}});
        out["temperatures"] = std::move(temps);
    }
    json errors = json::array();
    HMSQuery* query = wxGetApp().get_hms_query();
    if (DevHMS* hms = obj->GetHMS(); hms != nullptr) {
        for (const DevHMSItem& item : hms->GetHMSItems()) {
            const std::string code = item.get_long_error_code();
            json e = json::object({{"code", "HMS_" + code}, {"level", static_cast<int>(item.get_level())}});
            if (full && query != nullptr)
                if (const std::string msg = query->query_hms_msg(obj, code).ToUTF8().data(); !msg.empty())
                    e["message"] = msg;
            errors.push_back(std::move(e));
        }
    }
    if (obj->print_error != 0) {
        json e = json::object({{"code", obj->get_print_error_str()}, {"level", "print_error"}});
        if (full && query != nullptr)
            if (const std::string msg = query->query_print_error_msg(obj, obj->print_error).ToUTF8().data(); !msg.empty())
                e["message"] = msg;
        errors.push_back(std::move(e));
    }
    out["errors"] = std::move(errors);
    return out;
}

}}}} // namespace Slic3r::GUI::Mcp::Printers
