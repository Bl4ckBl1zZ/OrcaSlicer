#ifndef slic3r_Mcp_LayerEvents_hpp_
#define slic3r_Mcp_LayerEvents_hpp_

// plate_layer_events: colour change, filament change, pause, custom G-code at a layer (the layer
// slider's "Add pause / Add custom G-code / Change filament" menu), per plate.

namespace Slic3r { namespace GUI { namespace Mcp {
class ToolRegistry;
namespace Paint {

void register_layer_events(ToolRegistry& registry);

}}}} // namespace Slic3r::GUI::Mcp::Paint

#endif
