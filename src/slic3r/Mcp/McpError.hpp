#ifndef slic3r_Mcp_Error_hpp_
#define slic3r_Mcp_Error_hpp_

#include <exception>
#include <string>
#include <utility>

#include "McpJson.hpp"

namespace Slic3r { namespace GUI { namespace Mcp {

// An error the calling agent can act on. Thrown by tool handlers and helpers; the dispatcher turns
// it into a tool result with isError = true and the body
//   {"error": {"code", "message", "hint", "details"}}
// Codes are the snake_case identifiers listed in docs/mcp/errors.md (stale_id, invalid_argument,
// ui_busy, ...). The message says what is wrong, the hint says what to do next.
class ToolError : public std::exception
{
public:
    ToolError(std::string code, std::string message, std::string hint = {}, json details = nullptr)
        : m_code(std::move(code)), m_message(std::move(message)), m_hint(std::move(hint)), m_details(std::move(details)) {}

    const char* what() const noexcept override { return m_message.c_str(); }
    const std::string& code() const { return m_code; }
    const std::string& message() const { return m_message; }
    const std::string& hint() const { return m_hint; }
    const json& details() const { return m_details; }

    // {"error": {...}}
    json to_json() const
    {
        json e = json::object();
        e["code"] = m_code;
        e["message"] = m_message;
        if (!m_hint.empty())
            e["hint"] = m_hint;
        if (!m_details.is_null())
            e["details"] = m_details;
        json out = json::object();
        out["error"] = std::move(e);
        return out;
    }

private:
    std::string m_code;
    std::string m_message;
    std::string m_hint;
    json        m_details;
};

[[noreturn]] inline void fail(std::string code, std::string message, std::string hint = {}, json details = nullptr)
{
    throw ToolError(std::move(code), std::move(message), std::move(hint), std::move(details));
}

}}} // namespace Slic3r::GUI::Mcp

#endif
