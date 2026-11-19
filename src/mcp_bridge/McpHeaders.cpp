// HTTP-binding helpers: mirrored header values, x-mcp-header annotations, SSE parsing.
#include "BridgeCommon.hpp"

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/string.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

namespace orca_mcp_bridge {

namespace json = boost::json;

static std::string base64(const std::string &in)
{
    static const char *tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    std::size_t i = 0;
    while (i + 2 < in.size()) {
        const unsigned v = (unsigned char) in[i] << 16 | (unsigned char) in[i + 1] << 8 | (unsigned char) in[i + 2];
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        out += tbl[(v >> 6) & 63];
        out += tbl[v & 63];
        i += 3;
    }
    if (i + 1 == in.size()) {
        const unsigned v = (unsigned char) in[i] << 16;
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        out += "==";
    } else if (i + 2 == in.size()) {
        const unsigned v = (unsigned char) in[i] << 16 | (unsigned char) in[i + 1] << 8;
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        out += tbl[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

bool is_header_token(const std::string &s)
{
    if (s.empty())
        return false;
    static const std::string extra = "!#$%&'*+-.^_`|~";
    return std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return std::isalnum(c) != 0 || extra.find(char(c)) != std::string::npos;
    });
}

bool is_safe_header_value(const std::string &s)
{
    for (unsigned char c : s)
        if (!((c >= 0x20 && c <= 0x7e) || c == '\t'))
            return false;
    if (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.back() == ' ' || s.back() == '\t'))
        return false;
    return true;
}

std::string encode_header_value(const std::string &utf8)
{
    const bool sentinel = utf8.size() >= 11 && utf8.compare(0, 9, "=?base64?") == 0 &&
                          utf8.compare(utf8.size() - 2, 2, "?=") == 0;
    if (is_safe_header_value(utf8) && !sentinel)
        return utf8;
    return "=?base64?" + base64(utf8) + "?=";
}

std::string to_single_line(std::string text)
{
    for (char &c : text)
        if (c == '\n' || c == '\r')
            c = ' ';
    return text;
}

// ---------------------------------------------------------------------------------------------
// x-mcp-header (2026-07-28 "Custom Headers from Tool Parameters")
// ---------------------------------------------------------------------------------------------
namespace {

struct Scan
{
    std::vector<ParamHeader> found;
    std::string              problem;
};

bool primitive_type_ok(const json::object &schema)
{
    const json::value *t = schema.if_contains("type");
    if (!t || !t->is_string())
        return false;
    const json::string &s = t->as_string();
    return s == "string" || s == "integer" || s == "boolean";
}

// Walks a schema. `props_only` is true while every step from the root was a `properties` key.
void scan(const json::value &node, std::vector<std::string> &path, bool props_only, bool is_root, Scan &out)
{
    if (node.is_array()) {
        for (const auto &e : node.as_array())
            scan(e, path, false, false, out);
        return;
    }
    if (!node.is_object())
        return;
    const json::object &o = node.as_object();
    if (const json::value *h = o.if_contains("x-mcp-header")) {
        if (!props_only || is_root)
            out.problem = "x-mcp-header outside a chain of properties";
        else if (!h->is_string() || !is_header_token(std::string(h->as_string())))
            out.problem = "x-mcp-header value is not an HTTP token";
        else if (!primitive_type_ok(o))
            out.problem = "x-mcp-header on a parameter that is not string, integer or boolean";
        else
            out.found.push_back({path, std::string(h->as_string())});
    }
    for (const auto &kv : o) {
        if (kv.key() == "x-mcp-header")
            continue;
        if (kv.key() == "properties" && kv.value().is_object()) {
            for (const auto &p : kv.value().as_object()) {
                path.emplace_back(p.key());
                scan(p.value(), path, props_only, false, out);
                path.pop_back();
            }
        } else {
            scan(kv.value(), path, false, false, out);
        }
    }
}

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

} // namespace

std::vector<std::string> ToolHeaderRegistry::learn(const json::object &result)
{
    std::vector<std::string> invalid;
    const json::value *tools = result.if_contains("tools");
    if (!tools || !tools->is_array())
        return invalid;
    for (const auto &t : tools->as_array()) {
        if (!t.is_object())
            continue;
        const json::value *name = t.as_object().if_contains("name");
        const json::value *schema = t.as_object().if_contains("inputSchema");
        if (!name || !name->is_string())
            continue;
        const std::string tool_name(name->as_string());
        Scan s;
        if (schema) {
            std::vector<std::string> path;
            scan(*schema, path, true, true, s);
        }
        if (s.problem.empty()) {
            std::set<std::string> seen;
            for (const auto &h : s.found)
                if (!seen.insert(lower(h.name)).second)
                    s.problem = "duplicate x-mcp-header value " + h.name;
        }
        if (!s.problem.empty()) {
            log_error("tool '" + tool_name + "' has an invalid x-mcp-header annotation (" + s.problem +
                      "); hiding it from tools/list as the MCP specification requires");
            invalid.push_back(tool_name);
            m_tools.erase(tool_name);
            continue;
        }
        if (s.found.empty())
            m_tools.erase(tool_name);
        else
            m_tools[tool_name] = std::move(s.found);
    }
    return invalid;
}

std::vector<std::pair<std::string, std::string>> ToolHeaderRegistry::headers_for(const json::object &params) const
{
    std::vector<std::pair<std::string, std::string>> out;
    const json::value *name = params.if_contains("name");
    if (!name || !name->is_string())
        return out;
    auto it = m_tools.find(std::string(name->as_string()));
    if (it == m_tools.end())
        return out;
    const json::value *args = params.if_contains("arguments");
    for (const auto &h : it->second) {
        const json::value *v = args;
        for (const auto &key : h.path) {
            if (!v || !v->is_object()) {
                v = nullptr;
                break;
            }
            v = v->as_object().if_contains(key);
        }
        if (!v || v->is_null())
            continue;
        std::string text;
        if (v->is_string())
            text = std::string(v->as_string());
        else if (v->is_bool())
            text = v->as_bool() ? "true" : "false";
        else if (v->is_int64())
            text = std::to_string(v->as_int64());
        else if (v->is_uint64())
            text = std::to_string(v->as_uint64());
        else if (v->is_double() && v->as_double() == double((long long) v->as_double()))
            text = std::to_string((long long) v->as_double());
        else
            continue; // wrong type; the server reports it
        out.emplace_back("Mcp-Param-" + h.name, encode_header_value(text));
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// SSE
// ---------------------------------------------------------------------------------------------
void SseParser::feed(const char *data, std::size_t len, std::vector<Event> &out)
{
    std::size_t i = 0;
    if (!m_bom_checked) {
        // A UTF-8 BOM may start the stream; it can arrive split over feeds, so wait for 3 bytes.
        m_line.append(data, len);
        if (m_line.size() < 3 && std::string("\xEF\xBB\xBF").compare(0, m_line.size(), m_line) == 0)
            return;
        m_bom_checked = true;
        std::string pending;
        pending.swap(m_line);
        if (pending.compare(0, 3, "\xEF\xBB\xBF") == 0)
            pending.erase(0, 3);
        feed(pending.data(), pending.size(), out);
        return;
    }
    for (; i < len; ++i) {
        const char c = data[i];
        if (c == '\n') {
            if (m_last_was_cr) {
                m_last_was_cr = false; // CRLF: the CR already ended the line
                continue;
            }
            line(m_line, out);
            m_line.clear();
        } else if (c == '\r') {
            m_last_was_cr = true;
            line(m_line, out);
            m_line.clear();
        } else {
            m_last_was_cr = false;
            m_line += c;
        }
    }
}

void SseParser::line(const std::string &l, std::vector<Event> &out)
{
    if (l.empty()) {
        // Dispatch.
        if (m_have_data) {
            std::string d = m_data;
            if (!d.empty() && d.back() == '\n')
                d.pop_back();
            out.push_back(Event{m_type, std::move(d), m_last_id});
        }
        m_data.clear();
        m_type.clear();
        m_have_data = false;
        return;
    }
    if (l[0] == ':')
        return; // comment / keep-alive
    std::string field, value;
    const auto colon = l.find(':');
    if (colon == std::string::npos) {
        field = l;
    } else {
        field = l.substr(0, colon);
        value = l.substr(colon + 1);
        if (!value.empty() && value[0] == ' ')
            value.erase(0, 1);
    }
    if (field == "data") {
        m_data += value;
        m_data += '\n';
        m_have_data = true;
    } else if (field == "event") {
        m_type = value;
    } else if (field == "id") {
        if (value.find('\0') == std::string::npos)
            m_last_id = value;
    } else if (field == "retry") {
        if (!value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isdigit(c) != 0; }) &&
            value.size() < 10)
            m_retry = std::atol(value.c_str());
    }
}

} // namespace orca_mcp_bridge
