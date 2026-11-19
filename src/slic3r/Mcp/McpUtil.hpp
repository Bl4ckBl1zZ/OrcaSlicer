#ifndef slic3r_Mcp_Util_hpp_
#define slic3r_Mcp_Util_hpp_

// Small dependency-free helpers shared by the MCP sources.

#include <cctype>
#include <cstddef>
#include <string>

#include "McpError.hpp"
#include "McpJson.hpp"

namespace Slic3r { namespace GUI { namespace Mcp {

// Standard base64 (RFC 4648 section 4), or the URL-safe alphabet without padding (section 5).
inline std::string base64_encode(const std::string& bytes, bool url_safe = false)
{
    static const char std_alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    static const char url_alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    const char* alphabet = url_safe ? url_alphabet : std_alphabet;
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        const unsigned v = (static_cast<unsigned char>(bytes[i]) << 16) | (static_cast<unsigned char>(bytes[i + 1]) << 8) |
                           static_cast<unsigned char>(bytes[i + 2]);
        out += alphabet[(v >> 18) & 63];
        out += alphabet[(v >> 12) & 63];
        out += alphabet[(v >> 6) & 63];
        out += alphabet[v & 63];
    }
    if (i + 1 == bytes.size()) {
        const unsigned v = static_cast<unsigned char>(bytes[i]) << 16;
        out += alphabet[(v >> 18) & 63];
        out += alphabet[(v >> 12) & 63];
        if (!url_safe)
            out += "==";
    } else if (i + 2 == bytes.size()) {
        const unsigned v = (static_cast<unsigned char>(bytes[i]) << 16) | (static_cast<unsigned char>(bytes[i + 1]) << 8);
        out += alphabet[(v >> 18) & 63];
        out += alphabet[(v >> 12) & 63];
        out += alphabet[(v >> 6) & 63];
        if (!url_safe)
            out += '=';
    }
    return out;
}

// Decodes standard or URL-safe base64, with or without padding. Returns false on a bad character.
inline bool base64_decode(const std::string& text, std::string& out)
{
    out.clear();
    unsigned acc = 0;
    int bits = 0;
    for (char ch : text) {
        int v;
        if (ch >= 'A' && ch <= 'Z') v = ch - 'A';
        else if (ch >= 'a' && ch <= 'z') v = ch - 'a' + 26;
        else if (ch >= '0' && ch <= '9') v = ch - '0' + 52;
        else if (ch == '+' || ch == '-') v = 62;
        else if (ch == '/' || ch == '_') v = 63;
        else if (ch == '=') break;
        else return false;
        acc = (acc << 6) | static_cast<unsigned>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((acc >> bits) & 0xFF);
        }
    }
    return true;
}

// Comparison whose duration does not depend on where the strings differ.
inline bool constant_time_equals(const std::string& a, const std::string& b)
{
    unsigned char diff = static_cast<unsigned char>(a.size() != b.size());
    const size_t n = a.size() < b.size() ? a.size() : b.size();
    for (size_t i = 0; i < n; ++i)
        diff = static_cast<unsigned char>(diff | (static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i])));
    return diff == 0;
}

inline std::string to_lower_ascii(std::string s)
{
    for (char& ch : s)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

inline bool contains_ci(const std::string& haystack, const std::string& needle)
{
    return needle.empty() || to_lower_ascii(haystack).find(to_lower_ascii(needle)) != std::string::npos;
}

// True when `text` holds an ASCII control character (below 0x20, or DEL).
inline bool has_control_chars(const std::string& text)
{
    for (char ch : text) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c < 0x20 || c == 0x7F)
            return true;
    }
    return false;
}

// Names and messages an agent gives (object, part, plate, preset and project names, pause
// messages) end up in G-code comments, file headers and the UI: a line break there would start a
// new G-code line. Fails with invalid_argument when `text` holds a control character; `what` names
// the argument.
inline void check_single_line(const std::string& text, const char* what)
{
    for (size_t i = 0; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c < 0x20 || c == 0x7F)
            fail("invalid_argument",
                 std::string("`") + what + "` must not contain control characters (line breaks, tabs ...); found one at byte " + std::to_string(i) + ".",
                 "Use a single line of printable text.", json::object({{"argument", what}, {"position", i}}));
    }
}

// Serialises without throwing on invalid UTF-8 (file names from odd sources): bad bytes become U+FFFD.
inline std::string dump_json(const json& value)
{
    return value.dump(-1, ' ', false, json::error_handler_t::replace);
}

}}} // namespace Slic3r::GUI::Mcp

#endif
