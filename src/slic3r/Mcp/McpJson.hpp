#ifndef slic3r_Mcp_Json_hpp_
#define slic3r_Mcp_Json_hpp_

// JSON type used throughout the MCP module. Insertion-ordered, so results read "id, name, ..." in
// the order the code writes them and tools/list is byte-stable between calls.

#include <nlohmann/json.hpp>

namespace Slic3r { namespace GUI { namespace Mcp {

using json = nlohmann::ordered_json;

}}} // namespace Slic3r::GUI::Mcp

#endif
