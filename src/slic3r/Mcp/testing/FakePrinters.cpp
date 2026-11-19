// =====================================================================================
//  TEST-ONLY CODE. NEVER SHIP.
//
//  Fake Bambu printer agent for the MCP printer tool tests. Built only with
//  -DSLIC3R_MCP_TEST_PRINTERS=ON and active only when ORCA_MCP_FAKE_PRINTERS names a
//  fixture file. Everything here is simulated in-process: no socket is opened, no
//  printer is contacted, the Bambu network plug-in is never loaded or called.
// =====================================================================================
#include "FakePrinters.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/nowide/cstdlib.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include "libslic3r/AppConfig.hpp"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/DeviceCore/DevManager.h"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/Utils/BBLPrinterAgent.hpp"
#include "slic3r/Utils/IPrinterAgent.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"
#include "slic3r/Utils/NetworkAgentFactory.hpp"

namespace Slic3r { namespace GUI { namespace Mcp { namespace Testing {

namespace {

using json   = nlohmann::json;
namespace fs = boost::filesystem;

constexpr const char* FAKE_ENV_VAR      = "ORCA_MCP_FAKE_PRINTERS";
constexpr const char* FAKE_LOG          = "[MCP FAKE PRINTERS - TEST ONLY] ";
constexpr const char* FAKE_SW_VER       = "01.04.00.00";
constexpr int         FAKE_NOZZLE_PRINT = 220;
constexpr int         FAKE_BED_PRINT    = 65;
constexpr double      FAKE_NOZZLE_IDLE  = 25.0;
constexpr double      FAKE_BED_IDLE     = 24.0;

// ---------------------------------------------------------------------------------
// Paths and files (UTF-8 strings in, wide paths on Windows)
// ---------------------------------------------------------------------------------

fs::path to_path(const std::string& utf8)
{
#ifdef _WIN32
    return fs::path(boost::nowide::widen(utf8));
#else
    return fs::path(utf8);
#endif
}

std::string path_utf8(const fs::path& p)
{
#ifdef _WIN32
    return boost::nowide::narrow(p.wstring());
#else
    return p.string();
#endif
}

std::string dump_json_text(const json& j) { return j.dump(2, ' ', false, json::error_handler_t::replace); }

// Writes <target>.tmp and renames it over <target>, so a reader never sees a half-written file.
bool write_json_atomically(const fs::path& target, const json& j, std::string& error)
{
    fs::path tmp = target;
    tmp += ".tmp";
    {
        boost::nowide::ofstream out(path_utf8(tmp), std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "cannot create " + path_utf8(tmp);
            return false;
        }
        out << dump_json_text(j) << '\n';
        out.flush();
        if (!out) {
            error = "cannot write " + path_utf8(tmp);
            return false;
        }
    }
    boost::system::error_code ec;
    fs::rename(tmp, target, ec);
    if (ec) {
        error = "cannot rename " + path_utf8(tmp) + ": " + ec.message();
        boost::system::error_code ignored;
        fs::remove(tmp, ignored);
        return false;
    }
    return true;
}

bool copy_into_dump(const std::string& source, const fs::path& target, std::string& error)
{
    boost::system::error_code ec;
    fs::copy_file(to_path(source), target, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        error = "cannot copy " + source + " to " + path_utf8(target) + ": " + ec.message();
        return false;
    }
    return true;
}

bool is_regular_file_utf8(const std::string& utf8)
{
    if (utf8.empty())
        return false;
    boost::system::error_code ec;
    return fs::is_regular_file(to_path(utf8), ec);
}

// Highest NNN of an existing "NNN_..." file, so a rerun into the same dump dir keeps counting.
int highest_dump_seq(const fs::path& dir)
{
    int                       highest = 0;
    boost::system::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name   = path_utf8(it->path().filename());
        size_t            digits = 0;
        while (digits < name.size() && std::isdigit(static_cast<unsigned char>(name[digits])))
            ++digits;
        if (digits >= 3 && digits < name.size() && name[digits] == '_')
            highest = (std::max)(highest, std::atoi(name.substr(0, digits).c_str()));
    }
    return highest;
}

std::string to_hex(unsigned value)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%x", value);
    return buf;
}

std::string format_one_decimal(double value)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", value);
    return buf;
}

// ---------------------------------------------------------------------------------
// Fixture model
// ---------------------------------------------------------------------------------

struct FakeTray
{
    bool        present  = false;
    std::string type;
    std::string color;
    std::string idx;
    int         remain   = -1;
    int         temp_min = 0;
    int         temp_max = 0;
};

struct FakeAms
{
    int                   id       = 0;
    std::string           humidity = "4";
    std::string           temp     = "25.0";
    std::vector<FakeTray> trays; // always 4 slots; an absent tray has present == false
};

struct FakePrinter
{
    // Identity, as discovery reports it.
    std::string dev_id;
    std::string name;
    std::string dev_type;
    std::string ip;
    std::string access_code;
    std::string connect_type;
    std::string bind_state;
    std::string sw_ver;

    // Hardware.
    double                nozzle_diameter = 0.4;
    std::string           nozzle_type     = "stainless_steel";
    bool                  sdcard          = true;
    std::vector<FakeAms>  ams;
    FakeTray              external;
    int                   job_total_layers = 50;

    // Live state.
    std::string state         = "IDLE";
    int         progress      = 0;
    int         remaining_min = 0;
    int         layer         = 0;
    int         total_layers  = 0;
    std::string subtask_name;
    double      nozzle_temp   = FAKE_NOZZLE_IDLE;
    double      nozzle_target = 0;
    double      bed_temp      = FAKE_BED_IDLE;
    double      bed_target    = 0;
    json        hms           = json::array();
    int         print_error   = 0;
    std::string job_id;
    std::string gcode_file;
    int         hold_left     = -1; // ticks left in FINISH/FAILED before IDLE; -1 = stay
};

struct FakeFixture
{
    std::string              path;
    std::string              dump_dir;
    int                      progress_step = 5;
    int                      finish_hold_s = 10;
    std::vector<FakePrinter> printers;
};

void default_tray_temps(const std::string& type, int& temp_min, int& temp_max)
{
    if (type == "PLA") {
        temp_min = 190; temp_max = 230;
    } else if (type == "PETG") {
        temp_min = 220; temp_max = 260;
    } else if (type == "ABS" || type == "ASA") {
        temp_min = 240; temp_max = 270;
    } else if (type == "TPU") {
        temp_min = 200; temp_max = 250;
    } else {
        temp_min = 190; temp_max = 250;
    }
}

// Generic Bambu filament ids, used when a tray in the fixture names no "idx".
std::string default_filament_idx(const std::string& type)
{
    if (type == "PLA")  return "GFL99";
    if (type == "PETG") return "GFG99";
    if (type == "ABS")  return "GFB99";
    if (type == "ASA")  return "GFB98";
    if (type == "TPU")  return "GFU99";
    return "";
}

std::string normalize_color(std::string color)
{
    std::transform(color.begin(), color.end(), color.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    if (!color.empty() && color.front() == '#')
        color.erase(color.begin());
    if (color.size() == 6)
        color += "FF";
    return color;
}

FakeTray parse_tray(const json& j)
{
    FakeTray tray;
    if (j.is_null())
        return tray;
    if (!j.is_object())
        throw std::runtime_error("a tray must be an object, or null for an empty slot");
    tray.present = true;
    tray.type    = j.value("type", std::string("PLA"));
    tray.color   = normalize_color(j.value("color", std::string("FFFFFFFF")));
    tray.idx     = j.value("idx", default_filament_idx(tray.type));
    tray.remain  = j.value("remain", -1);
    int temp_min = 0;
    int temp_max = 0;
    default_tray_temps(tray.type, temp_min, temp_max);
    tray.temp_min = j.value("temp_min", temp_min);
    tray.temp_max = j.value("temp_max", temp_max);
    return tray;
}

void read_temp_pair(const json& j, const char* key, double& current, double& target)
{
    if (!j.contains(key))
        return;
    const json& v = j.at(key);
    if (v.is_array() && v.size() == 2) {
        current = v[0].get<double>();
        target  = v[1].get<double>();
    } else if (v.is_number()) {
        current = v.get<double>();
    } else {
        throw std::runtime_error(std::string("\"") + key + "\" must be [current, target]");
    }
}

bool is_known_state(const std::string& state)
{
    static const std::set<std::string> states{"IDLE", "PREPARE", "RUNNING", "PAUSE", "FINISH", "FAILED"};
    return states.count(state) != 0;
}

bool is_test_net_ip(const std::string& ip)
{
    return ip.rfind("192.0.2.", 0) == 0 || ip.rfind("198.51.100.", 0) == 0 || ip.rfind("203.0.113.", 0) == 0;
}

FakePrinter parse_printer(const json& j, size_t index)
{
    if (!j.is_object())
        throw std::runtime_error("each printer must be an object");
    FakePrinter p;
    p.dev_id = j.at("dev_id").get<std::string>();
    if (p.dev_id.empty())
        throw std::runtime_error("printer dev_id is empty");
    p.name         = j.value("name", "Fake printer " + p.dev_id);
    p.dev_type     = j.value("dev_type", std::string("N2S"));
    p.ip           = j.value("ip", "192.0.2." + std::to_string(10 + index));
    p.access_code  = j.value("access_code", std::string("12345678"));
    p.connect_type = j.value("connect_type", std::string("lan"));
    if (p.connect_type != "lan" && p.connect_type != "cloud")
        throw std::runtime_error("printer " + p.dev_id + ": connect_type must be \"lan\" or \"cloud\"");
    p.bind_state = j.value("bind_state", std::string(p.connect_type == "cloud" ? "occupied" : "free"));
    p.sw_ver     = j.value("sw_ver", std::string(FAKE_SW_VER));

    p.state = j.value("state", std::string("IDLE"));
    if (!is_known_state(p.state))
        throw std::runtime_error("printer " + p.dev_id + ": unknown state \"" + p.state + "\"");
    p.nozzle_diameter = j.value("nozzle_diameter", 0.4);
    p.nozzle_type     = j.value("nozzle_type", std::string("stainless_steel"));
    p.progress        = (std::min)(100, (std::max)(0, j.value("progress", 0)));
    p.remaining_min   = (std::max)(0, j.value("remaining_min", 0));
    if (j.contains("layer")) {
        const json& layer = j.at("layer");
        if (!layer.is_array() || layer.size() != 2)
            throw std::runtime_error("printer " + p.dev_id + ": \"layer\" must be [current, total]");
        p.layer        = layer[0].get<int>();
        p.total_layers = layer[1].get<int>();
    }
    p.job_total_layers = (std::max)(1, j.value("total_layers", 50));
    p.subtask_name     = j.value("subtask_name", std::string());
    read_temp_pair(j, "nozzle_temp", p.nozzle_temp, p.nozzle_target);
    read_temp_pair(j, "bed_temp", p.bed_temp, p.bed_target);
    if (j.contains("hms")) {
        if (!j.at("hms").is_array())
            throw std::runtime_error("printer " + p.dev_id + ": \"hms\" must be an array of {attr, code}");
        p.hms = j.at("hms");
    }
    p.sdcard      = j.value("sdcard", true);
    p.print_error = j.value("print_error", 0);

    if (j.contains("ams") && !j.at("ams").is_null()) {
        const json& ams_list = j.at("ams");
        if (!ams_list.is_array())
            throw std::runtime_error("printer " + p.dev_id + ": \"ams\" must be an array");
        for (size_t a = 0; a < ams_list.size(); ++a) {
            const json& ja = ams_list[a];
            if (!ja.is_object())
                throw std::runtime_error("printer " + p.dev_id + ": each AMS must be an object");
            FakeAms ams;
            ams.id = ja.value("id", static_cast<int>(a));
            if (ams.id < 0 || ams.id > 3)
                throw std::runtime_error("printer " + p.dev_id + ": AMS id must be 0..3");
            ams.humidity = std::to_string(ja.value("humidity", 4));
            ams.temp     = format_one_decimal(ja.value("temp", 25.0));
            if (ja.contains("trays")) {
                const json& trays = ja.at("trays");
                if (!trays.is_array() || trays.size() > 4)
                    throw std::runtime_error("printer " + p.dev_id + ": \"trays\" must be an array of at most 4 entries");
                for (const json& jt : trays)
                    ams.trays.push_back(parse_tray(jt));
            }
            ams.trays.resize(4);
            p.ams.push_back(std::move(ams));
        }
    }
    if (j.contains("external"))
        p.external = parse_tray(j.at("external"));
    return p;
}

bool load_fixture(const std::string& path, FakeFixture& out, std::string& error)
{
    try {
        boost::nowide::ifstream in(path);
        if (!in) {
            error = "cannot open the fixture file";
            return false;
        }
        const json doc = json::parse(in);
        if (!doc.is_object()) {
            error = "the fixture must be a JSON object";
            return false;
        }

        const fs::path    fixture_path = fs::absolute(to_path(path));
        const std::string dump         = doc.value("dump_dir", std::string());
        fs::path          dump_path    = dump.empty() ? fixture_path.parent_path() / "fake_printer_dump" : to_path(dump);
        if (dump_path.is_relative())
            dump_path = fs::absolute(dump_path, fixture_path.parent_path());

        out.path          = path;
        out.dump_dir      = path_utf8(dump_path);
        out.progress_step = (std::min)(100, (std::max)(0, doc.value("progress_step", 5)));
        out.finish_hold_s = (std::max)(0, doc.value("finish_hold_s", 10));

        if (!doc.contains("printers") || !doc.at("printers").is_array() || doc.at("printers").empty()) {
            error = "\"printers\" must be a non-empty array";
            return false;
        }
        std::set<std::string> ids;
        const json&           printers = doc.at("printers");
        for (size_t i = 0; i < printers.size(); ++i) {
            FakePrinter p = parse_printer(printers[i], i);
            if (!ids.insert(p.dev_id).second) {
                error = "duplicate dev_id " + p.dev_id;
                return false;
            }
            if (!is_test_net_ip(p.ip))
                BOOST_LOG_TRIVIAL(warning) << FAKE_LOG << "printer " << p.dev_id << " uses ip " << p.ip
                                           << ", which is not a TEST-NET address; the fake never contacts it, but prefer 192.0.2.x";
            out.printers.push_back(std::move(p));
        }
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

// ---------------------------------------------------------------------------------
// Process-wide state: the loaded fixture and the dump sequence number
// ---------------------------------------------------------------------------------

struct FakeGlobals
{
    std::mutex                         mutex;
    bool                               attempted = false;
    std::shared_ptr<const FakeFixture> fixture; // set only when the fake is active
    int                                next_seq  = 1;
};

FakeGlobals& globals()
{
    static FakeGlobals g;
    return g;
}

std::shared_ptr<const FakeFixture> active_fixture()
{
    FakeGlobals&                g = globals();
    std::lock_guard<std::mutex> lock(g.mutex);
    return g.fixture;
}

int take_dump_seq()
{
    FakeGlobals&                g = globals();
    std::lock_guard<std::mutex> lock(g.mutex);
    return g.next_seq++;
}

std::string dump_prefix(int seq, const char* method)
{
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%03d_%s", seq, method);
    return buf;
}

std::string product_name_for(const std::string& dev_type)
{
    if (dev_type == "N2S")     return "Bambu Lab A1";
    if (dev_type == "N1")      return "Bambu Lab A1 mini";
    if (dev_type == "C12")     return "Bambu Lab P1S";
    if (dev_type == "C11")     return "Bambu Lab P1P";
    if (dev_type == "BL-P001") return "Bambu Lab X1 Carbon";
    return "Bambu Lab " + dev_type;
}

json print_params_json(const PrintParams& p)
{
    json j;
    j["dev_id"]                      = p.dev_id;
    j["task_name"]                   = p.task_name;
    j["project_name"]                = p.project_name;
    j["preset_name"]                 = p.preset_name;
    j["filename"]                    = p.filename;
    j["config_filename"]             = p.config_filename;
    j["plate_index"]                 = p.plate_index;
    j["ftp_folder"]                  = p.ftp_folder;
    j["ftp_file"]                    = p.ftp_file;
    j["ftp_file_md5"]                = p.ftp_file_md5;
    j["nozzle_mapping"]              = p.nozzle_mapping;
    j["ams_mapping"]                 = p.ams_mapping;
    j["ams_mapping2"]                = p.ams_mapping2;
    j["ams_mapping_info"]            = p.ams_mapping_info;
    j["nozzles_info"]                = p.nozzles_info;
    j["connection_type"]             = p.connection_type;
    j["comments"]                    = p.comments;
    j["origin_profile_id"]           = p.origin_profile_id;
    j["stl_design_id"]               = p.stl_design_id;
    j["origin_model_id"]             = p.origin_model_id;
    j["print_type"]                  = p.print_type;
    j["dst_file"]                    = p.dst_file;
    j["dev_name"]                    = p.dev_name;
    j["dev_ip"]                      = p.dev_ip;
    j["use_ssl_for_ftp"]             = p.use_ssl_for_ftp;
    j["use_ssl_for_mqtt"]            = p.use_ssl_for_mqtt;
    j["username"]                    = p.username;
    j["password"]                    = p.password;
    j["task_bed_leveling"]           = p.task_bed_leveling;
    j["task_flow_cali"]              = p.task_flow_cali;
    j["task_vibration_cali"]         = p.task_vibration_cali;
    j["task_layer_inspect"]          = p.task_layer_inspect;
    j["task_record_timelapse"]       = p.task_record_timelapse;
    j["task_timelapse_use_internal"] = p.task_timelapse_use_internal;
    j["task_use_ams"]                = p.task_use_ams;
    j["task_bed_type"]               = p.task_bed_type;
    j["extra_options"]               = p.extra_options;
    j["auto_bed_leveling"]           = p.auto_bed_leveling;
    j["auto_flow_cali"]              = p.auto_flow_cali;
    j["auto_offset_cali"]            = p.auto_offset_cali;
    j["extruder_cali_manual_mode"]   = p.extruder_cali_manual_mode;
    j["task_ext_change_assist"]      = p.task_ext_change_assist;
    j["try_emmc_print"]              = p.try_emmc_print;
    j["svc_context"]                 = p.svc_context;
    j["slicer_uid"]                  = p.slicer_uid;
    j["queue_plate_id"]              = p.queue_plate_id;
    return j;
}

json tray_json(const FakeTray& tray, int slot)
{
    json j;
    j["id"] = std::to_string(slot);
    if (!tray.present)
        return j; // an empty slot carries its id only, as on a real printer
    j["tray_type"]       = tray.type;
    j["tray_info_idx"]   = tray.idx;
    j["tray_color"]      = tray.color;
    j["remain"]          = tray.remain;
    j["nozzle_temp_min"] = std::to_string(tray.temp_min);
    j["nozzle_temp_max"] = std::to_string(tray.temp_max);
    j["tag_uid"]         = "0000000000000000";
    j["tray_sub_brands"] = "";
    j["tray_diameter"]   = "1.75";
    return j;
}

std::string ssdp_payload(const FakePrinter& p)
{
    json j;
    j["dev_name"]     = p.name;
    j["dev_id"]       = p.dev_id;
    j["dev_ip"]       = p.ip;
    j["dev_type"]     = p.dev_type;
    j["dev_signal"]   = "-45dBm";
    j["connect_type"] = p.connect_type;
    j["bind_state"]   = p.bind_state;
    j["sec_link"]     = "secure";
    return j.dump();
}

std::string version_payload(const FakePrinter& p, const std::string& sequence_id)
{
    json ota;
    ota["name"]         = "ota";
    ota["product_name"] = product_name_for(p.dev_type);
    ota["sw_ver"]       = p.sw_ver;
    ota["hw_ver"]       = "";
    ota["sn"]           = "";
    json mc;
    mc["name"]         = "mc";
    mc["product_name"] = product_name_for(p.dev_type);
    mc["sw_ver"]       = p.sw_ver;
    mc["hw_ver"]       = "MC07";
    mc["sn"]           = p.dev_id;

    json j;
    j["info"]["command"]     = "get_version";
    j["info"]["sequence_id"] = sequence_id;
    j["info"]["result"]      = "success";
    j["info"]["module"]      = json::array({ota, mc});
    return j.dump();
}

std::string status_payload(const FakePrinter& p, const std::string& sequence_id)
{
    json pr;
    pr["command"]             = "push_status";
    pr["msg"]                 = 0;
    pr["sequence_id"]         = sequence_id;
    pr["gcode_state"]         = p.state;
    pr["mc_percent"]          = p.progress;
    pr["mc_remaining_time"]   = p.remaining_min;
    pr["layer_num"]           = p.layer;
    pr["total_layer_num"]     = p.total_layers;
    pr["subtask_name"]        = p.subtask_name;
    pr["gcode_file"]          = p.gcode_file;
    pr["print_type"]          = p.job_id.empty() ? "idle" : "local";
    if (!p.job_id.empty())
        pr["job_id"] = p.job_id;
    pr["nozzle_temper"]        = p.nozzle_temp;
    pr["nozzle_target_temper"] = p.nozzle_target;
    pr["bed_temper"]           = p.bed_temp;
    pr["bed_target_temper"]    = p.bed_target;
    pr["nozzle_diameter"]      = format_one_decimal(p.nozzle_diameter);
    pr["nozzle_type"]          = p.nozzle_type;
    pr["sdcard"]               = p.sdcard;
    pr["hms"]                  = p.hms;
    pr["print_error"]          = p.print_error;
    pr["wifi_signal"]          = "-45dBm";
    pr["ams_status"]           = 0;

    unsigned ams_bits  = 0;
    unsigned tray_bits = 0;
    json     ams_list  = json::array();
    for (const FakeAms& ams : p.ams) {
        ams_bits |= 1u << ams.id;
        json trays = json::array();
        for (size_t slot = 0; slot < ams.trays.size(); ++slot) {
            if (ams.trays[slot].present)
                tray_bits |= 1u << (ams.id * 4 + static_cast<int>(slot));
            trays.push_back(tray_json(ams.trays[slot], static_cast<int>(slot)));
        }
        json ja;
        ja["id"]       = std::to_string(ams.id);
        ja["humidity"] = ams.humidity;
        ja["temp"]     = ams.temp;
        ja["tray"]     = std::move(trays);
        ams_list.push_back(std::move(ja));
    }
    json ams;
    ams["ams"]                 = std::move(ams_list);
    ams["ams_exist_bits"]      = to_hex(ams_bits);
    ams["tray_exist_bits"]     = to_hex(tray_bits);
    ams["tray_read_done_bits"] = to_hex(tray_bits);
    ams["tray_reading_bits"]   = "0";
    ams["tray_is_bbl_bits"]    = to_hex(tray_bits);
    ams["tray_now"]            = "255";
    ams["tray_tar"]            = "255";
    ams["insert_flag"]         = true;
    ams["power_on_flag"]       = false;
    pr["ams"]                  = std::move(ams);

    pr["vt_tray"] = tray_json(p.external, 255);

    json j;
    j["print"] = std::move(pr);
    return j.dump();
}

void ramp(double& value, double goal, double step)
{
    if (value < goal)
        value = (std::min)(goal, value + step);
    else if (value > goal)
        value = (std::max)(goal, value - step);
}

void reset_to_idle(FakePrinter& p)
{
    p.state         = "IDLE";
    p.progress      = 0;
    p.remaining_min = 0;
    p.layer         = 0;
    p.total_layers  = 0;
    p.subtask_name.clear();
    p.job_id.clear();
    p.gcode_file.clear();
    p.nozzle_target = 0;
    p.bed_target    = 0;
    p.hold_left     = -1;
}

std::string command_of(const json& section)
{
    if (section.is_object() && section.contains("command") && section.at("command").is_string())
        return section.at("command").get<std::string>();
    return "";
}

std::string sequence_of(const json& section)
{
    if (section.is_object() && section.contains("sequence_id")) {
        const json& s = section.at("sequence_id");
        if (s.is_string())
            return s.get<std::string>();
        if (s.is_number_integer())
            return std::to_string(s.get<long long>());
    }
    return "0";
}

// ---------------------------------------------------------------------------------
// The fake agent
// ---------------------------------------------------------------------------------

class FakePrinterAgent final : public IPrinterAgent
{
public:
    explicit FakePrinterAgent(std::shared_ptr<const FakeFixture> fixture);
    ~FakePrinterAgent() override;

    FakePrinterAgent(const FakePrinterAgent&)            = delete;
    FakePrinterAgent& operator=(const FakePrinterAgent&) = delete;

    void set_cloud_agent(std::shared_ptr<ICloudServiceAgent> /*cloud*/) override {}

    int send_message(std::string dev_id, std::string json_str, int /*qos*/, int /*flag*/) override;

    // The real BBL agent sends these as gcode; the fake accepts and ignores them, so
    // MachineObject never shows its "not supported" dialog during a test.
    int command_ams_refresh_rfid(std::string /*dev_id*/, int /*tray_id*/, int /*seq*/, int /*ams_id*/, bool /*lan_mode*/) override
    { return BAMBU_NETWORK_SUCCESS; }
    int command_ams_calibrate(std::string /*dev_id*/, int /*ams_id*/, int /*seq*/, bool /*lan_mode*/) override
    { return BAMBU_NETWORK_SUCCESS; }
    int command_ams_select_tray(std::string /*dev_id*/, std::string /*tray_id*/, int /*seq*/, bool /*lan_mode*/) override
    { return BAMBU_NETWORK_SUCCESS; }
    int command_start_camera(std::string /*dev_id*/) override { return BAMBU_NETWORK_SUCCESS; }

    std::string default_lan_username() const override { return "bblp"; }

    int connect_printer(const PrinterConnectionParams& params) override;
    int disconnect_printer() override;
    int send_message_to_printer(std::string dev_id, std::string json_str, int /*qos*/, int /*flag*/) override;

    bool start_discovery(bool start, bool /*sending*/) override;
    int  bind_detect(std::string dev_ip, std::string /*sec_link*/, detectResult& detect) override;
    int  set_server_callback(OnServerErrFn /*fn*/) override { return BAMBU_NETWORK_SUCCESS; }

    std::string get_user_selected_machine() override;
    int         set_user_selected_machine(std::string dev_id) override;

    int start_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) override;
    int start_local_print_with_record(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) override;
    int start_send_gcode_to_sdcard(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) override;
    int start_local_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn) override;
    int start_sdcard_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn) override;

    int set_on_ssdp_msg_fn(OnMsgArrivedFn fn) override;
    int set_on_printer_connected_fn(OnPrinterConnectedFn /*fn*/) override { return BAMBU_NETWORK_SUCCESS; }
    int set_on_subscribe_failure_fn(GetSubscribeFailureFn /*fn*/) override { return BAMBU_NETWORK_SUCCESS; }
    int set_on_message_fn(OnMessageFn /*fn*/) override { return BAMBU_NETWORK_SUCCESS; }
    int set_on_user_message_fn(OnMessageFn /*fn*/) override { return BAMBU_NETWORK_SUCCESS; }
    int set_on_local_connect_fn(OnLocalConnectedFn fn) override;
    int set_on_local_message_fn(OnMessageFn fn) override;
    int set_queue_on_main_fn(QueueOnMainFn /*fn*/) override { return BAMBU_NETWORK_SUCCESS; }

    AgentInfo get_agent_info() override
    {
        return {BBL_PRINTER_AGENT_ID, "Bambu Lab", "fake-test-only",
                "FAKE Bambu printer agent for MCP tests (test-only build; no network, no plug-in)"};
    }

    FilamentSyncMode get_filament_sync_mode() const override { return FilamentSyncMode::subscription; }
    std::string      to_orca_filament_id(const std::string& printer_filament_id) const override;
    std::string      from_orca_filament_id(const std::string& orca_filament_id) const override
    {
        return BBLPrinterAgent::bambu_filament_id(orca_filament_id);
    }

private:
    enum class OutKind { Ssdp, LocalConnect, LocalMessage };

    struct Outgoing
    {
        OutKind     kind;
        std::string dev_id;
        std::string payload;
        int         status;
    };

    struct JobKind
    {
        const char* method;       // dump file name and log label
        bool        lan_upload;   // checks the access code like an FTP upload
        bool        needs_file;   // params.filename must exist (except for verify_job)
        bool        record_stage; // reports PrintingStageRecord
        bool        starts_print; // moves the printer to RUNNING
        int         auth_error;   // result for a wrong access code
    };

    int  run_job(const JobKind& kind, PrintParams params, const OnUpdateStatusFn& update_fn, const WasCancelledFn& cancel_fn,
                 const OnWaitFn& wait_fn);
    int  handle_command(const char* method, const std::string& dev_id, const std::string& json_str);
    void worker_main();
    void tick_locked();
    void advance_locked(FakePrinter& p);
    void queue_locked(OutKind kind, const std::string& dev_id, std::string payload, int status = 0);
    void queue_ssdp_locked();
    void queue_status_locked(const FakePrinter& p);
    void queue_version_locked(const FakePrinter& p);
    void drop_connection_locked();
    FakePrinter* find_locked(const std::string& dev_id);
    std::string  next_sequence_locked();

    const std::shared_ptr<const FakeFixture> m_fixture;
    std::mutex                               m_mutex;
    std::condition_variable                  m_cv;
    std::vector<FakePrinter>                 m_printers;
    std::deque<Outgoing>                     m_out;
    OnMsgArrivedFn                           m_on_ssdp;
    OnLocalConnectedFn                       m_on_local_connect;
    OnMessageFn                              m_on_local_message;
    std::string                              m_connected_dev;
    std::string                              m_selected_machine;
    long long                                m_next_job_id   = 1000001;
    unsigned                                 m_push_sequence = 0;
    bool                                     m_discovery     = false;
    bool                                     m_stop          = false;
    std::thread                              m_thread; // last: started once every other member exists
};

FakePrinterAgent::FakePrinterAgent(std::shared_ptr<const FakeFixture> fixture)
    : m_fixture(std::move(fixture))
    , m_printers(m_fixture->printers)
{
    for (FakePrinter& p : m_printers) {
        // A printer the fixture puts mid-job needs a job id like a real one would report.
        if (p.state != "IDLE" && p.job_id.empty())
            p.job_id = std::to_string(m_next_job_id++);
        if (p.state == "RUNNING" && p.total_layers == 0)
            p.total_layers = p.job_total_layers;
    }
    m_thread = std::thread([this] { worker_main(); });
}

FakePrinterAgent::~FakePrinterAgent()
{
    // No logging here: the cached agent may be destroyed during static teardown.
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
    }
    m_cv.notify_all();
    if (m_thread.joinable())
        m_thread.join();
}

FakePrinter* FakePrinterAgent::find_locked(const std::string& dev_id)
{
    for (FakePrinter& p : m_printers)
        if (p.dev_id == dev_id)
            return &p;
    return nullptr;
}

std::string FakePrinterAgent::next_sequence_locked() { return std::to_string(++m_push_sequence); }

void FakePrinterAgent::queue_locked(OutKind kind, const std::string& dev_id, std::string payload, int status)
{
    m_out.push_back(Outgoing{kind, dev_id, std::move(payload), status});
}

void FakePrinterAgent::queue_ssdp_locked()
{
    for (const FakePrinter& p : m_printers)
        queue_locked(OutKind::Ssdp, p.dev_id, ssdp_payload(p));
}

void FakePrinterAgent::queue_status_locked(const FakePrinter& p)
{
    queue_locked(OutKind::LocalMessage, p.dev_id, status_payload(p, next_sequence_locked()));
}

void FakePrinterAgent::queue_version_locked(const FakePrinter& p)
{
    queue_locked(OutKind::LocalMessage, p.dev_id, version_payload(p, next_sequence_locked()));
}

void FakePrinterAgent::drop_connection_locked()
{
    if (m_connected_dev.empty())
        return;
    const std::string old = m_connected_dev;
    m_out.erase(std::remove_if(m_out.begin(), m_out.end(),
                               [&old](const Outgoing& o) { return o.kind == OutKind::LocalMessage && o.dev_id == old; }),
                m_out.end());
    m_connected_dev.clear();
}

void FakePrinterAgent::advance_locked(FakePrinter& p)
{
    const int step = m_fixture->progress_step;
    if (p.state == "RUNNING" && step > 0) {
        p.progress = (std::min)(100, p.progress + step);
        if (p.progress >= 100) {
            p.state         = "FINISH";
            p.remaining_min = 0;
            p.layer         = p.total_layers;
            p.nozzle_target = 0;
            p.bed_target    = 0;
            p.hold_left     = m_fixture->finish_hold_s;
        } else {
            // One tick stands in for one minute of printing.
            p.remaining_min = (100 - p.progress + step - 1) / step;
            p.layer         = (std::max)(1, p.progress * p.total_layers / 100);
        }
    } else if ((p.state == "FINISH" || p.state == "FAILED") && p.hold_left >= 0) {
        if (p.hold_left == 0)
            reset_to_idle(p);
        else
            --p.hold_left;
    }
    ramp(p.nozzle_temp, p.nozzle_target > 0 ? p.nozzle_target : FAKE_NOZZLE_IDLE, 15.0);
    ramp(p.bed_temp, p.bed_target > 0 ? p.bed_target : FAKE_BED_IDLE, 4.0);
}

void FakePrinterAgent::tick_locked()
{
    for (FakePrinter& p : m_printers)
        advance_locked(p);
    if (m_discovery && m_on_ssdp)
        queue_ssdp_locked();
    if (!m_connected_dev.empty())
        if (const FakePrinter* p = find_locked(m_connected_dev))
            queue_status_locked(*p);
}

void FakePrinterAgent::worker_main()
{
    using steady   = std::chrono::steady_clock;
    auto next_tick = steady::now() + std::chrono::seconds(1);
    std::unique_lock<std::mutex> lock(m_mutex);
    while (!m_stop) {
        m_cv.wait_until(lock, next_tick, [this] { return m_stop || !m_out.empty(); });
        if (m_stop)
            break;
        const auto now = steady::now();
        if (now >= next_tick) {
            tick_locked();
            next_tick += std::chrono::seconds(1);
            if (next_tick <= now)
                next_tick = now + std::chrono::seconds(1);
        }
        if (m_out.empty())
            continue;

        std::deque<Outgoing> batch;
        batch.swap(m_out);
        const OnMsgArrivedFn     on_ssdp       = m_on_ssdp;
        const OnLocalConnectedFn on_connect    = m_on_local_connect;
        const OnMessageFn        on_message    = m_on_local_message;
        const std::string        connected_dev = m_connected_dev;
        lock.unlock();

        // Callbacks run without the lock: they may call straight back into this agent.
        for (Outgoing& o : batch) {
            try {
                switch (o.kind) {
                case OutKind::Ssdp:
                    if (on_ssdp)
                        on_ssdp(o.payload);
                    break;
                case OutKind::LocalConnect:
                    if (on_connect)
                        on_connect(o.status, o.dev_id, o.payload);
                    break;
                case OutKind::LocalMessage:
                    // Status goes to the one connected printer only.
                    if (on_message && o.dev_id == connected_dev)
                        on_message(o.dev_id, BBLPrinterAgent::to_orca_payload(std::move(o.payload)));
                    break;
                }
            } catch (const std::exception& e) {
                BOOST_LOG_TRIVIAL(error) << FAKE_LOG << "callback threw: " << e.what();
            } catch (...) {
                BOOST_LOG_TRIVIAL(error) << FAKE_LOG << "callback threw an unknown exception";
            }
        }
        lock.lock();
    }
}

int FakePrinterAgent::set_on_ssdp_msg_fn(OnMsgArrivedFn fn)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_on_ssdp = std::move(fn);
    if (m_discovery && m_on_ssdp)
        queue_ssdp_locked();
    m_cv.notify_all();
    return BAMBU_NETWORK_SUCCESS;
}

int FakePrinterAgent::set_on_local_connect_fn(OnLocalConnectedFn fn)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_on_local_connect = std::move(fn);
    return BAMBU_NETWORK_SUCCESS;
}

int FakePrinterAgent::set_on_local_message_fn(OnMessageFn fn)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_on_local_message = std::move(fn);
    return BAMBU_NETWORK_SUCCESS;
}

bool FakePrinterAgent::start_discovery(bool start, bool /*sending*/)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_discovery = start;
    if (m_discovery && m_on_ssdp)
        queue_ssdp_locked(); // answer at once instead of waiting for the next tick
    m_cv.notify_all();
    return true;
}

int FakePrinterAgent::bind_detect(std::string dev_ip, std::string /*sec_link*/, detectResult& detect)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const FakePrinter& p : m_printers) {
        if (p.ip != dev_ip)
            continue;
        detect.result_msg   = "success";
        detect.command      = "bind_detect";
        detect.dev_id       = p.dev_id;
        detect.model_id     = p.dev_type;
        detect.dev_name     = p.name;
        detect.version      = p.sw_ver;
        detect.bind_state   = p.bind_state;
        detect.connect_type = p.connect_type;
        return BAMBU_NETWORK_SUCCESS;
    }
    return BAMBU_NETWORK_ERR_INVALID_HANDLE;
}

std::string FakePrinterAgent::get_user_selected_machine()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_selected_machine;
}

int FakePrinterAgent::set_user_selected_machine(std::string dev_id)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_selected_machine = std::move(dev_id);
    return BAMBU_NETWORK_SUCCESS;
}

std::string FakePrinterAgent::to_orca_filament_id(const std::string& printer_filament_id) const
{
    try {
        json probe;
        probe["tray_info_idx"] = printer_filament_id;
        const json translated  = json::parse(BBLPrinterAgent::to_orca_payload(probe.dump()));
        return translated.at("tray_info_idx").get<std::string>();
    } catch (...) {
        return printer_filament_id;
    }
}

int FakePrinterAgent::connect_printer(const PrinterConnectionParams& params)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    drop_connection_locked(); // one LAN connection at a time
    const FakePrinter* p = find_locked(params.dev_id);
    if (p == nullptr || (!params.host.empty() && params.host != p->ip)) {
        BOOST_LOG_TRIVIAL(warning) << FAKE_LOG << "connect_printer: no fake printer " << params.dev_id << " at " << params.host;
        queue_locked(OutKind::LocalConnect, params.dev_id, "-1", ConnectStatusFailed);
    } else if (params.password != p->access_code) {
        BOOST_LOG_TRIVIAL(warning) << FAKE_LOG << "connect_printer: wrong access code for " << params.dev_id;
        queue_locked(OutKind::LocalConnect, params.dev_id, "5", ConnectStatusFailed);
    } else {
        BOOST_LOG_TRIVIAL(info) << FAKE_LOG << "connect_printer: connected to " << p->dev_id;
        m_connected_dev = p->dev_id;
        queue_locked(OutKind::LocalConnect, p->dev_id, "", ConnectStatusOk);
        queue_version_locked(*p);
        queue_status_locked(*p);
    }
    m_cv.notify_all();
    return BAMBU_NETWORK_SUCCESS;
}

int FakePrinterAgent::disconnect_printer()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_connected_dev.empty())
        BOOST_LOG_TRIVIAL(info) << FAKE_LOG << "disconnect_printer: " << m_connected_dev;
    drop_connection_locked();
    return BAMBU_NETWORK_SUCCESS;
}

int FakePrinterAgent::send_message_to_printer(std::string dev_id, std::string json_str, int /*qos*/, int /*flag*/)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (dev_id.empty() || dev_id != m_connected_dev)
            return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    }
    return handle_command("send_message_to_printer", dev_id, json_str);
}

int FakePrinterAgent::send_message(std::string dev_id, std::string json_str, int /*qos*/, int /*flag*/)
{
    return handle_command("send_message", dev_id, json_str);
}

int FakePrinterAgent::handle_command(const char* method, const std::string& dev_id, const std::string& json_str)
{
    json msg;
    try {
        msg = json::parse(BBLPrinterAgent::from_orca_payload(json_str));
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << FAKE_LOG << method << ": ignoring a message that is not JSON: " << e.what();
        return BAMBU_NETWORK_SUCCESS;
    }
    if (!msg.is_object())
        return BAMBU_NETWORK_SUCCESS;

    json record;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        FakePrinter*                p = find_locked(dev_id);
        if (p == nullptr)
            return BAMBU_NETWORK_ERR_INVALID_HANDLE;
        const bool connected = dev_id == m_connected_dev;

        if (msg.contains("pushing") && command_of(msg.at("pushing")) == "pushall") {
            if (connected)
                queue_status_locked(*p);
        } else if (msg.contains("info") && command_of(msg.at("info")) == "get_version") {
            if (connected)
                queue_version_locked(*p);
        } else if (msg.contains("system") && command_of(msg.at("system")) == "get_access_code") {
            if (connected) {
                json reply;
                reply["system"]["command"]     = "get_access_code";
                reply["system"]["access_code"] = p->access_code;
                reply["system"]["result"]      = "success";
                reply["system"]["sequence_id"] = sequence_of(msg.at("system"));
                queue_locked(OutKind::LocalMessage, dev_id, reply.dump());
            }
        } else if (msg.contains("print")) {
            const std::string command = command_of(msg.at("print"));
            if (command == "pause" || command == "resume" || command == "stop") {
                const std::string before   = p->state;
                bool              accepted = false;
                if (command == "pause" && (before == "RUNNING" || before == "PREPARE")) {
                    p->state = "PAUSE";
                    accepted = true;
                } else if (command == "resume" && before == "PAUSE") {
                    p->state = "RUNNING";
                    accepted = true;
                } else if (command == "stop" && (before == "RUNNING" || before == "PAUSE" || before == "PREPARE")) {
                    p->state         = "FAILED";
                    p->remaining_min = 0;
                    p->nozzle_target = 0;
                    p->bed_target    = 0;
                    p->hold_left     = m_fixture->finish_hold_s;
                    accepted         = true;
                }
                record["method"]       = method;
                record["dev_id"]       = dev_id;
                record["command"]      = command;
                record["payload"]      = msg;
                record["state_before"] = before;
                record["state_after"]  = p->state;
                record["accepted"]     = accepted;
                record["job_id"]       = p->job_id;
                record["time"]         = static_cast<long long>(std::time(nullptr));
                if (connected)
                    queue_status_locked(*p);
                BOOST_LOG_TRIVIAL(info) << FAKE_LOG << method << ": " << command << " on " << dev_id << " (" << before << " -> "
                                        << p->state << ")";
            }
        }
        m_cv.notify_all();
    }

    if (!record.is_null()) {
        const int seq  = take_dump_seq();
        record["seq"]  = seq;
        std::string error;
        if (!write_json_atomically(to_path(m_fixture->dump_dir) / (dump_prefix(seq, "command") + ".json"), record, error))
            BOOST_LOG_TRIVIAL(error) << FAKE_LOG << "cannot record command: " << error;
    }
    return BAMBU_NETWORK_SUCCESS;
}

int FakePrinterAgent::run_job(const JobKind&          kind,
                              PrintParams             params,
                              const OnUpdateStatusFn& update_fn,
                              const WasCancelledFn&   cancel_fn,
                              const OnWaitFn&         wait_fn)
{
    const bool verify       = params.project_name == "verify_job";
    params.ams_mapping_info = BBLPrinterAgent::from_orca_payload(params.ams_mapping_info);

    auto report    = [&update_fn](int stage, int code, const std::string& info) {
        if (update_fn)
            update_fn(stage, code, info);
    };
    auto cancelled = [&cancel_fn]() { return cancel_fn && cancel_fn(); };
    auto settle    = []() { std::this_thread::sleep_for(std::chrono::milliseconds(150)); };

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const FakePrinter*          p = find_locked(params.dev_id);
        if (p == nullptr) {
            BOOST_LOG_TRIVIAL(error) << FAKE_LOG << kind.method << ": no fake printer " << params.dev_id;
            return BAMBU_NETWORK_ERR_INVALID_HANDLE;
        }
        if (kind.lan_upload && params.password != p->access_code) {
            BOOST_LOG_TRIVIAL(warning) << FAKE_LOG << kind.method << ": wrong access code for " << params.dev_id;
            return kind.auth_error;
        }
    }
    if (kind.needs_file && !verify && !is_regular_file_utf8(params.filename)) {
        BOOST_LOG_TRIVIAL(error) << FAKE_LOG << kind.method << ": file not found: " << params.filename;
        return BAMBU_NETWORK_ERR_FILE_NOT_EXIST;
    }

    report(PrintingStageCreate, 0, "");
    if (cancelled())
        return BAMBU_NETWORK_ERR_CANCELED;
    for (int percent : {0, 50, 100}) {
        report(PrintingStageUpload, percent, std::to_string(percent) + "%");
        settle();
        if (cancelled()) {
            BOOST_LOG_TRIVIAL(info) << FAKE_LOG << kind.method << ": cancelled during upload, nothing recorded";
            return BAMBU_NETWORK_ERR_CANCELED;
        }
    }
    report(PrintingStageSending, 0, "");
    if (kind.record_stage)
        report(PrintingStageRecord, 0, "");
    settle();
    if (cancelled()) {
        BOOST_LOG_TRIVIAL(info) << FAKE_LOG << kind.method << ": cancelled before start, nothing recorded";
        return BAMBU_NETWORK_ERR_CANCELED;
    }

    std::string job_id;
    if (kind.starts_print && !verify) {
        std::lock_guard<std::mutex> lock(m_mutex);
        job_id = std::to_string(m_next_job_id++);
    }

    // Record: copies first, the json last and atomically, so its presence means the job is complete.
    const int         seq    = take_dump_seq();
    const std::string prefix = dump_prefix(seq, kind.method);
    const fs::path    dir    = to_path(m_fixture->dump_dir);
    std::string       error;
    std::string       dumped_3mf;
    std::string       dumped_config;
    if (is_regular_file_utf8(params.filename)) {
        if (!copy_into_dump(params.filename, dir / (prefix + ".3mf"), error)) {
            BOOST_LOG_TRIVIAL(error) << FAKE_LOG << kind.method << ": " << error;
            return BAMBU_NETWORK_ERR_INVALID_HANDLE;
        }
        dumped_3mf = prefix + ".3mf";
    }
    if (!params.config_filename.empty()) {
        if (is_regular_file_utf8(params.config_filename)) {
            if (!copy_into_dump(params.config_filename, dir / (prefix + ".config.3mf"), error)) {
                BOOST_LOG_TRIVIAL(error) << FAKE_LOG << kind.method << ": " << error;
                return BAMBU_NETWORK_ERR_INVALID_HANDLE;
            }
            dumped_config = prefix + ".config.3mf";
        } else {
            BOOST_LOG_TRIVIAL(warning) << FAKE_LOG << kind.method << ": config file not found: " << params.config_filename;
        }
    }
    json record          = print_params_json(params);
    record["seq"]        = seq;
    record["method"]     = kind.method;
    record["verify_job"] = verify;
    record["job_id"]     = job_id;
    record["dump_3mf"]   = dumped_3mf;
    record["dump_config_3mf"] = dumped_config;
    record["time"]       = static_cast<long long>(std::time(nullptr));
    if (!write_json_atomically(dir / (prefix + ".json"), record, error)) {
        BOOST_LOG_TRIVIAL(error) << FAKE_LOG << kind.method << ": cannot record the job: " << error;
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    }
    BOOST_LOG_TRIVIAL(info) << FAKE_LOG << kind.method << ": recorded " << prefix << ".json for " << params.dev_id
                            << (verify ? " (verify_job)" : "");

    if (!job_id.empty()) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (FakePrinter* p = find_locked(params.dev_id)) {
            const int step   = m_fixture->progress_step;
            p->state         = "RUNNING";
            p->progress      = 0;
            p->remaining_min = step > 0 ? (100 + step - 1) / step : 0;
            p->layer         = 0;
            p->total_layers  = p->job_total_layers;
            p->subtask_name  = params.project_name;
            p->job_id        = job_id;
            p->gcode_file    = !params.dst_file.empty() ? params.dst_file
                             : !params.ftp_file.empty() ? params.ftp_file
                                                        : path_utf8(to_path(params.filename).filename());
            p->nozzle_target = FAKE_NOZZLE_PRINT;
            p->bed_target    = FAKE_BED_PRINT;
            p->print_error   = 0;
            p->hold_left     = -1;
            if (p->dev_id == m_connected_dev)
                queue_status_locked(*p);
            m_cv.notify_all();
        }
    }

    report(PrintingStageWaitPrinter, 0, "");
    if (wait_fn && !job_id.empty()) {
        json info;
        info["job_id"] = job_id;
        wait_fn(PrintingStageWaitPrinter, info.dump());
    }
    return BAMBU_NETWORK_SUCCESS;
}

int FakePrinterAgent::start_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn)
{
    static const JobKind kind{"start_print", false, true, true, true, BAMBU_NETWORK_ERR_INVALID_HANDLE};
    return run_job(kind, std::move(params), update_fn, cancel_fn, wait_fn);
}

int FakePrinterAgent::start_local_print_with_record(PrintParams      params,
                                                    OnUpdateStatusFn update_fn,
                                                    WasCancelledFn   cancel_fn,
                                                    OnWaitFn         wait_fn)
{
    static const JobKind kind{"start_local_print_with_record", true, true, true, true, BAMBU_NETWORK_ERR_PRINT_WR_UPLOAD_FTP_FAILED};
    return run_job(kind, std::move(params), update_fn, cancel_fn, wait_fn);
}

int FakePrinterAgent::start_send_gcode_to_sdcard(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn)
{
    static const JobKind kind{"start_send_gcode_to_sdcard", true, true, false, false, BAMBU_NETWORK_ERR_FTP_UPLOAD_FAILED};
    return run_job(kind, std::move(params), update_fn, cancel_fn, wait_fn);
}

int FakePrinterAgent::start_local_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn)
{
    static const JobKind kind{"start_local_print", true, true, false, true, BAMBU_NETWORK_ERR_PRINT_LP_UPLOAD_FTP_FAILED};
    return run_job(kind, std::move(params), update_fn, cancel_fn, OnWaitFn());
}

int FakePrinterAgent::start_sdcard_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn)
{
    static const JobKind kind{"start_sdcard_print", false, false, false, true, BAMBU_NETWORK_ERR_INVALID_HANDLE};
    return run_job(kind, std::move(params), update_fn, cancel_fn, OnWaitFn());
}

} // namespace

// ---------------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------------

void register_fake_printer_agent()
{
    FakeGlobals&                g = globals();
    std::lock_guard<std::mutex> lock(g.mutex);
    if (g.attempted)
        return;
    g.attempted = true;

    const char* env = boost::nowide::getenv(FAKE_ENV_VAR);
    if (env == nullptr || *env == '\0')
        return;
    const std::string fixture_path = env;

    auto        fixture = std::make_shared<FakeFixture>();
    std::string error;
    if (!load_fixture(fixture_path, *fixture, error)) {
        BOOST_LOG_TRIVIAL(error) << FAKE_LOG << "fixture " << fixture_path << " is invalid, the fake stays inactive: " << error;
        return;
    }

    boost::system::error_code ec;
    const fs::path            dump_dir = to_path(fixture->dump_dir);
    fs::create_directories(dump_dir, ec);
    if (ec || !fs::is_directory(dump_dir, ec)) {
        BOOST_LOG_TRIVIAL(error) << FAKE_LOG << "cannot create dump dir " << fixture->dump_dir << ", the fake stays inactive";
        return;
    }

    std::shared_ptr<const FakeFixture> shared = fixture;
    const bool registered = NetworkAgentFactory::register_printer_agent(
        BBL_PRINTER_AGENT_ID, "Bambu Lab [FAKE test agent]",
        [shared](std::shared_ptr<ICloudServiceAgent> cloud_agent, const std::string& /*log_dir*/) -> std::shared_ptr<IPrinterAgent> {
            auto agent = std::make_shared<FakePrinterAgent>(shared);
            agent->set_cloud_agent(std::move(cloud_agent));
            return agent;
        });
    if (!registered) {
        BOOST_LOG_TRIVIAL(error) << FAKE_LOG << "the \"bbl\" printer agent id is already registered, the fake stays inactive";
        return;
    }

    g.next_seq = highest_dump_seq(dump_dir) + 1;
    g.fixture  = shared;
    BOOST_LOG_TRIVIAL(warning) << FAKE_LOG << "FAKE Bambu printer agent is ACTIVE (test-only build). Fixture: " << fixture_path
                               << ", printers: " << shared->printers.size() << ", dump dir: " << shared->dump_dir
                               << ". No real printer or network is used.";
}

void attach_fake_printers(NetworkAgent* agent, DeviceManager* devices)
{
    const std::shared_ptr<const FakeFixture> fixture = active_fixture();
    if (!fixture || agent == nullptr)
        return;

    // Same route as GUI_App::post_init: NetworkAgent marshals SSDP to the main thread.
    agent->set_on_ssdp_msg_fn([](std::string json_str) {
        GUI_App& app = wxGetApp();
        if (app.is_closing())
            return;
        if (DeviceManager* manager = app.getDeviceManager())
            manager->on_machine_alive(json_str);
    });

    // The access codes go where the connect dialog puts them (in memory; AppConfig saves them
    // with the rest of the config, into whatever data dir this test instance runs with).
    AppConfig* config = wxGetApp().app_config;
    for (const FakePrinter& p : fixture->printers) {
        if (p.access_code.empty())
            continue;
        if (config != nullptr) {
            config->set_str("access_code", p.dev_id, p.access_code);
            const auto& machines = config->get_local_machines();
            const auto  it       = machines.find(p.dev_id);
            if (it != machines.end() && it->second.printer_agent_id == BBL_PRINTER_AGENT_ID && it->second.access_code != p.access_code) {
                BBLocalMachine machine = it->second;
                machine.access_code    = p.access_code;
                config->update_local_machine(machine);
            }
        }
        if (devices != nullptr)
            if (MachineObject* obj = devices->get_local_machine(p.dev_id))
                obj->set_access_code(p.access_code, false);
    }

    // If the fake is already live, start discovery now; otherwise switch_printer_agent() does it
    // when it installs the fake, and the callback set above is applied to it then.
    if (std::dynamic_pointer_cast<FakePrinterAgent>(agent->get_printer_agent()))
        agent->start_discovery(true, false);

    BOOST_LOG_TRIVIAL(warning) << FAKE_LOG << "attached: " << fixture->printers.size() << " fake printer(s) wired into discovery";
}

bool fake_printers_active() { return active_fixture() != nullptr; }

std::string fake_printers_dump_dir()
{
    const std::shared_ptr<const FakeFixture> fixture = active_fixture();
    return fixture ? fixture->dump_dir : std::string();
}

} } } } // namespace Slic3r::GUI::Mcp::Testing
