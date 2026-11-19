#include "Printers.hpp"

#include <cmath>
#include <map>

#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/Utils/Http.hpp"

namespace Slic3r { namespace GUI { namespace Mcp { namespace Printers {

bool HostRequest::done() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_done;
}

json HostRequest::result() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_result;
}

void HostRequest::complete(json result)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_done)
        return;
    m_result = std::move(result);
    m_done   = true;
}

namespace {

std::mutex                  g_cache_mutex;
std::map<std::string, json> g_cache;

void remember(const std::string& id, const json& status)
{
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    g_cache[id] = status;
}

// The connection settings of a host printer, copied so the HTTP threads never touch presets.
struct Host
{
    std::string id;
    std::string kind;
    std::string base;    // never leaves the application
    std::string apikey;  // never leaves the application
    std::string cafile;
    bool        ssl_revoke_best_effort { false };
};

bool host_of(const Printer& printer, Host& out)
{
    const Preset* preset = host_preset(printer);
    if (preset == nullptr)
        return false;
    const DynamicPrintConfig& c = preset->config;
    out.id     = printer.id;
    out.kind   = printer.kind;
    out.base   = c.has("print_host") ? c.opt_string("print_host") : "";
    out.apikey = c.has("printhost_apikey") ? c.opt_string("printhost_apikey") : "";
    out.cafile = c.has("printhost_cafile") ? c.opt_string("printhost_cafile") : "";
    out.ssl_revoke_best_effort = c.has("printhost_ssl_ignore_revoke") && c.opt_bool("printhost_ssl_ignore_revoke");
    return !out.base.empty();
}

std::string url(const Host& h, const std::string& path)
{
    std::string base = h.base;
    if (base.rfind("http://", 0) != 0 && base.rfind("https://", 0) != 0)
        base = "http://" + base;
    if (!base.empty() && base.back() == '/')
        base.pop_back();
    return base + "/" + path;
}

Http prepared(Http http, const Host& h)
{
    // Same headers as the application's own OctoPrint / Moonraker clients.
    if (h.kind == "octoprint" || !h.apikey.empty())
        http.header("X-Api-Key", h.apikey);
    if (!h.cafile.empty())
        http.ca_file(h.cafile);
    http.ssl_revoke_best_effort(h.ssl_revoke_best_effort);
    http.timeout_connect(4);
    http.timeout_max(10);
    return http;
}

// A failure, in words that name neither the host nor its address (curl messages do).
std::string failure(unsigned status)
{
    if (status == 401 || status == 403)
        return "the printer host refused the API key (HTTP " + std::to_string(status) + ")";
    if (status == 0)
        return "the printer host did not answer";
    return "the printer host answered HTTP " + std::to_string(status);
}

json offline(unsigned status)
{
    return json::object({{"online", false}, {"state", "offline"}, {"error", failure(status)}});
}

json parse(const std::string& body)
{
    json j = json::parse(body, nullptr, false);
    return j.is_discarded() ? json::object() : j;
}

double num(const json& j, const char* key, double fallback = -1.)
{
    if (!j.is_object())
        return fallback;
    auto it = j.find(key);
    return it != j.end() && it->is_number() ? it->get<double>() : fallback;
}

json temp(double current, double target)
{
    return json::object({{"current", std::round(current * 10.) / 10.}, {"target", std::round(target * 10.) / 10.}});
}

// ------------------------------------------------------------------------------------------------
// OctoPrint: /api/printer then /api/job.
// ------------------------------------------------------------------------------------------------

void octoprint_job(const Host& h, json status, std::shared_ptr<HostRequest> req)
{
    prepared(Http::get(url(h, "api/job")), h)
        .on_complete([h, status, req](std::string body, unsigned) mutable {
            json       j        = parse(body);
            const json progress = j.value("progress", json::object());
            const json file     = j.contains("job") && j["job"].is_object() ? j["job"].value("file", json::object()) : json::object();
            const std::string state = status.value("state", "");
            if (state == "printing" || state == "paused") {
                json job = json::object();
                if (file.is_object() && file.contains("name") && file["name"].is_string())
                    job["name"] = file["name"];
                if (const double c = num(progress, "completion"); c >= 0.)
                    job["percent"] = static_cast<int>(std::floor(c));
                if (const double t = num(progress, "printTimeLeft"); t >= 0.)
                    job["remaining_s"] = static_cast<int>(t);
                status["job"] = std::move(job);
            }
            remember(h.id, status);
            req->complete(std::move(status));
        })
        .on_error([h, status, req](std::string, std::string, unsigned) mutable {
            remember(h.id, status);
            req->complete(std::move(status));
        })
        .perform();
}

void octoprint_status(const Host& h, std::shared_ptr<HostRequest> req)
{
    prepared(Http::get(url(h, "api/printer")), h)
        .on_complete([h, req](std::string body, unsigned) {
            json       j     = parse(body);
            const json st    = j.value("state", json::object());
            const json flags = st.is_object() ? st.value("flags", json::object()) : json::object();
            auto flag = [&flags](const char* k) { return flags.is_object() && flags.value(k, false); };
            std::string state = "idle";
            if (flag("error") || flag("closedOrError"))
                state = "error";
            else if (flag("paused") || flag("pausing"))
                state = "paused";
            else if (flag("printing") || flag("cancelling"))
                state = "printing";
            json status = json::object({{"online", true}, {"state", state}});
            json temps  = json::object();
            const json t = j.value("temperature", json::object());
            if (t.is_object()) {
                if (t.contains("tool0"))
                    temps["nozzle"] = temp(num(t["tool0"], "actual", 0.), num(t["tool0"], "target", 0.));
                if (t.contains("bed"))
                    temps["bed"] = temp(num(t["bed"], "actual", 0.), num(t["bed"], "target", 0.));
            }
            status["temperatures"] = std::move(temps);
            json errors = json::array();
            if (state == "error" && st.is_object() && st.contains("text") && st["text"].is_string())
                errors.push_back(json::object({{"code", "host_error"}, {"message", st["text"]}}));
            status["errors"] = std::move(errors);
            octoprint_job(h, std::move(status), req);
        })
        .on_error([h, req](std::string, std::string, unsigned http_status) {
            // 409: OctoPrint runs but is not connected to the printer.
            json status = http_status == 409 ? json::object({{"online", true},
                                                             {"state", "offline"},
                                                             {"error", "OctoPrint is not connected to the printer"}})
                                             : offline(http_status);
            remember(h.id, status);
            req->complete(std::move(status));
        })
        .perform();
}

// ------------------------------------------------------------------------------------------------
// Moonraker: one objects query.
// ------------------------------------------------------------------------------------------------

void moonraker_status(const Host& h, std::shared_ptr<HostRequest> req)
{
    prepared(Http::get(url(h, "printer/objects/query?print_stats&virtual_sdcard&display_status&extruder&heater_bed")), h)
        .on_complete([h, req](std::string body, unsigned) {
            json       j  = parse(body);
            const json s  = j.contains("result") && j["result"].is_object() ? j["result"].value("status", json::object()) : json::object();
            const json ps = s.value("print_stats", json::object());
            const std::string raw = ps.is_object() ? ps.value("state", "") : "";
            std::string state = "idle";
            if (raw == "printing")
                state = "printing";
            else if (raw == "paused")
                state = "paused";
            else if (raw == "complete")
                state = "finished";
            else if (raw == "error")
                state = "error";
            json status = json::object({{"online", true}, {"state", state}});
            if (state == "printing" || state == "paused" || state == "finished") {
                json job = json::object();
                if (ps.contains("filename") && ps["filename"].is_string())
                    job["name"] = ps["filename"];
                double p = num(s.value("virtual_sdcard", json::object()), "progress");
                if (p < 0.)
                    p = num(s.value("display_status", json::object()), "progress");
                if (p >= 0.)
                    job["percent"] = static_cast<int>(std::floor(p * 100.));
                const json info = ps.value("info", json::object());
                if (const double total = num(info, "total_layer"); total > 0.)
                    job["layer"] = json::array({static_cast<int>(num(info, "current_layer", 0.)), static_cast<int>(total)});
                status["job"] = std::move(job);
            }
            json temps = json::object();
            if (s.contains("extruder"))
                temps["nozzle"] = temp(num(s["extruder"], "temperature", 0.), num(s["extruder"], "target", 0.));
            if (s.contains("heater_bed"))
                temps["bed"] = temp(num(s["heater_bed"], "temperature", 0.), num(s["heater_bed"], "target", 0.));
            status["temperatures"] = std::move(temps);
            json errors = json::array();
            if (state == "error" && ps.contains("message") && ps["message"].is_string() && !ps["message"].get<std::string>().empty())
                errors.push_back(json::object({{"code", "host_error"}, {"message", ps["message"]}}));
            status["errors"] = std::move(errors);
            remember(h.id, status);
            req->complete(std::move(status));
        })
        .on_error([h, req](std::string, std::string, unsigned http_status) {
            json status = offline(http_status);
            remember(h.id, status);
            req->complete(std::move(status));
        })
        .perform();
}

} // namespace

std::shared_ptr<HostRequest> host_status(const Printer& printer)
{
    auto req = std::make_shared<HostRequest>();
    Host h;
    if (!host_of(printer, h)) {
        req->complete(json::object({{"online", false}, {"state", "unknown"}, {"error", "the printer preset has no print host"}}));
        return req;
    }
    if (h.kind == "octoprint")
        octoprint_status(h, req);
    else if (h.kind == "moonraker")
        moonraker_status(h, req);
    else
        req->complete(json::object({{"online", nullptr}, {"state", "unknown"}, {"status_supported", false}}));
    return req;
}

std::shared_ptr<HostRequest> host_control(const Printer& printer, const std::string& action)
{
    auto req = std::make_shared<HostRequest>();
    Host h;
    if (!host_of(printer, h)) {
        req->complete(json::object({{"ok", false}, {"error", "the printer preset has no print host"}}));
        return req;
    }
    auto done = [req](std::string, unsigned) { req->complete(json::object({{"ok", true}})); };
    auto err  = [req](std::string, std::string, unsigned status) { req->complete(json::object({{"ok", false}, {"error", failure(status)}})); };
    if (h.kind == "octoprint") {
        json body = action == "stop" ? json::object({{"command", "cancel"}})
                                     : json::object({{"command", "pause"}, {"action", action == "pause" ? "pause" : "resume"}});
        prepared(Http::post(url(h, "api/job")), h)
            .header("Content-Type", "application/json")
            .set_post_body(body.dump())
            .on_complete(done)
            .on_error(err)
            .perform();
    } else if (h.kind == "moonraker") {
        const std::string verb = action == "stop" ? "cancel" : action;
        prepared(Http::post(url(h, "printer/print/" + verb)), h).on_complete(done).on_error(err).perform();
    } else {
        req->complete(json::object({{"ok", false}, {"error", "printer_control supports OctoPrint and Moonraker hosts only"}}));
    }
    return req;
}

json host_cached_status(const std::string& printer_id)
{
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    auto it = g_cache.find(printer_id);
    return it == g_cache.end() ? json(nullptr) : it->second;
}

}}}} // namespace Slic3r::GUI::Mcp::Printers
