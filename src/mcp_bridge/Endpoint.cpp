// Server discovery: command line / environment / the endpoint file the app writes.
#include "BridgeCommon.hpp"

#include <boost/asio/ip/address.hpp>
#include <boost/json/parse.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace orca_mcp_bridge {

namespace json = boost::json;

static std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

static std::string trim(const std::string &s)
{
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return std::string();
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

bool is_loopback_host(const std::string &host)
{
    if (lower(host) == "localhost")
        return true;
    boost::system::error_code ec;
    const auto addr = boost::asio::ip::make_address(host, ec);
    return !ec && addr.is_loopback();
}

bool parse_url(const std::string &text_in, Url &out, std::string &err)
{
    const std::string text = trim(text_in);
    const auto sep = text.find("://");
    if (sep == std::string::npos) {
        err = "not an absolute URL: " + text;
        return false;
    }
    out.scheme = lower(text.substr(0, sep));
    if (out.scheme != "http") {
        err = "unsupported URL scheme '" + out.scheme + "' (the bridge speaks plain HTTP to a local server): " + text;
        return false;
    }
    std::string rest = text.substr(sep + 3);
    const auto slash = rest.find_first_of("/?#");
    std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
    std::string target    = slash == std::string::npos ? std::string("/") : rest.substr(slash);
    if (const auto hash = target.find('#'); hash != std::string::npos)
        target.erase(hash);
    if (target.empty() || target[0] != '/')
        target = "/" + target;
    if (authority.find('@') != std::string::npos) {
        err = "credentials in the URL are not supported; use --token or --token-file";
        return false;
    }
    std::string host, port;
    if (!authority.empty() && authority[0] == '[') {
        const auto close = authority.find(']');
        if (close == std::string::npos) {
            err = "bad IPv6 literal in URL: " + text;
            return false;
        }
        host = authority.substr(1, close - 1);
        const std::string after = authority.substr(close + 1);
        if (!after.empty()) {
            if (after[0] != ':') {
                err = "bad URL authority: " + text;
                return false;
            }
            port = after.substr(1);
        }
    } else {
        const auto colon = authority.rfind(':');
        if (colon != std::string::npos) {
            host = authority.substr(0, colon);
            port = authority.substr(colon + 1);
        } else {
            host = authority;
        }
    }
    if (host.empty()) {
        err = "URL has no host: " + text;
        return false;
    }
    if (port.empty())
        port = "80";
    if (port.size() > 5 || !std::all_of(port.begin(), port.end(), [](unsigned char c) { return std::isdigit(c) != 0; }) ||
        std::stoi(port) <= 0 || std::stoi(port) > 65535) {
        err = "bad port in URL: " + text;
        return false;
    }
    out.host        = host;
    out.port        = port;
    out.target      = target;
    const bool v6   = host.find(':') != std::string::npos;
    out.host_header = (v6 ? "[" + host + "]" : host) + ":" + port;
    out.text        = "http://" + out.host_header + target;
    return true;
}

// ---------------------------------------------------------------------------------------------

EndpointResolver::EndpointResolver(const Options &opts) : m_opts(opts) {}

bool EndpointResolver::validate_static(std::string &err) const
{
    if (!m_opts.url.empty()) {
        Url u;
        if (!parse_url(m_opts.url, u, err)) {
            err = m_opts.url_source + ": " + err;
            return false;
        }
        if (!m_opts.allow_remote && !is_loopback_host(u.host)) {
            err = m_opts.url_source + ": refusing non-loopback URL " + u.text +
                  " (the bridge only talks to a server on this computer; pass --allow-remote to override)";
            return false;
        }
    }
    return true;
}

std::vector<std::string> EndpointResolver::endpoint_file_candidates() const
{
    std::vector<std::string> out;
    if (!m_opts.endpoint_file.empty()) {
        out.push_back(m_opts.endpoint_file);
        return out;
    }
    if (!m_opts.datadir.empty()) {
        out.push_back(join_path(join_path(m_opts.datadir, "mcp"), "endpoint.json"));
        return out;
    }
    // OrcaSlicer uses the first of these data directories that exists, so the endpoint file is
    // looked for in that one only; the last candidate is the per-user default.
    const auto dirs = default_data_dirs();
    for (std::size_t i = 0; i < dirs.size(); ++i) {
        if (i + 1 == dirs.size() || file_exists(dirs[i])) {
            out.push_back(join_path(join_path(dirs[i], "mcp"), "endpoint.json"));
            break;
        }
    }
    return out;
}

ResolveResult EndpointResolver::resolve(bool force)
{
    const bool need_file = m_opts.url.empty() || (m_opts.token.empty() && m_opts.token_file.empty());
    std::string path;
    std::optional<std::string> stamp;
    if (need_file) {
        const auto cands = endpoint_file_candidates();
        path  = cands.empty() ? std::string() : cands.front();
        stamp = path.empty() ? std::nullopt : file_stamp(path);
    }
    if (!force && m_cache && path == m_stamp_path && stamp == m_stamp)
        return *m_cache;
    if (m_cache && (path != m_stamp_path || stamp != m_stamp))
        log_debug("endpoint configuration changed; re-reading");
    m_stamp_path = path;
    m_stamp      = stamp;
    m_cache      = resolve_uncached();
    return *m_cache;
}

ResolveResult EndpointResolver::resolve_uncached()
{
    ResolveResult res;
    Endpoint      ep;

    // 1. The endpoint file, when it is needed for the URL or the token.
    std::string file_url, file_token, file_err;
    bool        have_file = false;
    const bool  need_file = m_opts.url.empty() || (m_opts.token.empty() && m_opts.token_file.empty());
    if (need_file && !m_stamp_path.empty()) {
        if (m_stamp) {
            std::string text;
            if (!read_file_utf8_path(m_stamp_path, text, file_err)) {
                // fall through to error below
            } else {
                boost::system::error_code ec;
                json::value v = json::parse(text, ec);
                if (ec || !v.is_object()) {
                    file_err = "the endpoint file " + m_stamp_path + " is not a JSON object" +
                               (ec ? " (" + ec.message() + ")" : std::string()) +
                               ". It may be in the middle of being written; try again, or restart OrcaSlicer.";
                } else {
                    const json::object &o = v.as_object();
                    if (auto *u = o.if_contains("url"); u && u->is_string()) {
                        file_url = std::string(u->as_string());
                    } else if (auto *p = o.if_contains("port"); p && (p->is_int64() || p->is_uint64())) {
                        file_url = "http://127.0.0.1:" + std::to_string(p->to_number<long long>()) + "/mcp";
                    }
                    if (auto *t = o.if_contains("token"); t && t->is_string())
                        file_token = std::string(t->as_string());
                    if (auto *p = o.if_contains("pid"); p && (p->is_int64() || p->is_uint64()))
                        ep.pid = p->to_number<long long>();
                    if (auto *vs = o.if_contains("protocolVersions"); vs && vs->is_array())
                        for (const auto &x : vs->as_array())
                            if (x.is_string())
                                ep.versions.emplace_back(x.as_string());
                    have_file = true;
                    if (file_url.empty() && m_opts.url.empty())
                        file_err = "the endpoint file " + m_stamp_path + " has neither \"url\" nor \"port\"";
                }
            }
        }
    }

    // 2. URL
    if (!m_opts.url.empty()) {
        std::string err;
        if (!parse_url(m_opts.url, ep.url, err)) {
            res.error  = m_opts.url_source + ": " + err;
            res.reason = "bad_url";
            return res;
        }
        ep.url_source = m_opts.url_source;
    } else {
        if (!file_err.empty()) {
            res.error  = file_err;
            res.reason = "bad_endpoint_file";
            return res;
        }
        if (!have_file) {
            res.error = "OrcaSlicer's MCP server is not running: there is no endpoint file at " +
                        (m_stamp_path.empty() ? std::string("<unknown>") : m_stamp_path) +
                        ". Start OrcaSlicer and enable the MCP server in Preferences (the app writes this file "
                        "when the server starts and removes it when the server stops). To connect to a server "
                        "elsewhere, pass --url and --token-file.";
            res.reason = "no_endpoint_file";
            return res;
        }
        std::string err;
        if (!parse_url(file_url, ep.url, err)) {
            res.error  = "the endpoint file " + m_stamp_path + " has a bad url: " + err;
            res.reason = "bad_endpoint_file";
            return res;
        }
        ep.url_source    = "endpoint file " + m_stamp_path;
        ep.endpoint_file = m_stamp_path;
    }
    if (!m_opts.allow_remote && !is_loopback_host(ep.url.host)) {
        res.error  = "refusing non-loopback URL " + ep.url.text + " from " + ep.url_source +
                     " (pass --allow-remote to override)";
        res.reason = "non_loopback";
        return res;
    }

    // 3. Token
    if (!m_opts.token.empty()) {
        ep.token        = m_opts.token;
        ep.token_source = m_opts.token_source;
    } else if (!m_opts.token_file.empty()) {
        std::string text, err;
        if (!read_file_utf8_path(m_opts.token_file, text, err)) {
            res.error  = "cannot read the token file: " + err;
            res.reason = "bad_token_file";
            return res;
        }
        ep.token        = trim(text);
        ep.token_source = "token file " + m_opts.token_file;
    } else if (have_file) {
        ep.token        = file_token;
        ep.token_source = "endpoint file " + m_stamp_path;
        if (ep.endpoint_file.empty())
            ep.endpoint_file = m_stamp_path;
    }
    for (unsigned char c : ep.token)
        if (c < 0x21 || c > 0x7e) {
            res.error  = "the token from " + ep.token_source + " contains characters that cannot be sent in a header";
            res.reason = "bad_token";
            return res;
        }
    res.endpoint = ep;
    return res;
}

std::string EndpointResolver::describe_refused(const Endpoint &ep, std::string &reason)
{
    if (!ep.endpoint_file.empty() && ep.pid && ep.url_source.rfind("endpoint file", 0) == 0) {
        const auto alive = process_alive(*ep.pid);
        if (alive && !*alive) {
            reason = "app_not_running";
            return "OrcaSlicer is not running: the endpoint file " + ep.endpoint_file + " was left by process " +
                   std::to_string(*ep.pid) + ", which has exited. Start OrcaSlicer and enable the MCP server in Preferences.";
        }
        if (alive && *alive) {
            reason = "server_not_listening";
            return "OrcaSlicer (process " + std::to_string(*ep.pid) + ") is running, but nothing accepts connections at " +
                   ep.url.text + ". Enable the MCP server in OrcaSlicer's Preferences, or wait until it has started.";
        }
    }
    reason = "connection_refused";
    return "Cannot connect to OrcaSlicer's MCP server at " + ep.url.text + " (from " + ep.url_source +
           "): connection refused. Start OrcaSlicer and enable the MCP server in Preferences.";
}

} // namespace orca_mcp_bridge
