// =====================================================================================
//  TEST-ONLY CODE. NEVER SHIP.
//
//  A fake Bambu printer agent for the MCP printer tool tests (tests/mcp). It is compiled
//  only when CMake runs with -DSLIC3R_MCP_TEST_PRINTERS=ON, and even then it does nothing
//  unless the environment variable ORCA_MCP_FAKE_PRINTERS names a readable fixture file.
//
//  The fake replaces the "bbl" printer agent: it answers discovery, LAN connect, status,
//  print and control calls from a fixture, records every print job and control command
//  to a dump directory, and never touches the network, the Bambu plug-in or a real printer.
// =====================================================================================
#ifndef slic3r_Mcp_testing_FakePrinters_hpp_
#define slic3r_Mcp_testing_FakePrinters_hpp_

#include <string>

namespace Slic3r {
class NetworkAgent;
class DeviceManager;
} // namespace Slic3r

namespace Slic3r { namespace GUI { namespace Mcp { namespace Testing {

// Call before NetworkAgentFactory::register_all_agents(). A no-op unless ORCA_MCP_FAKE_PRINTERS
// names a readable, valid fixture; then the fake is registered under BBL_PRINTER_AGENT_ID ("bbl")
// so the real Bambu agent's later registration is ignored. Idempotent: only the first call acts.
void register_fake_printer_agent();

// Call on the main thread after GUI_App::init_networking_callbacks(). Routes SSDP from the fake
// into DeviceManager::on_machine_alive, puts the fixture access codes into the in-memory
// app_config (as the GUI's connect dialog does) and starts discovery if the fake is already the
// live printer agent. Discovery also works if the fake is installed later by switch_printer_agent.
void attach_fake_printers(NetworkAgent* agent, DeviceManager* devices);

// True once register_fake_printer_agent() loaded a fixture and registered the fake.
bool fake_printers_active();

// Directory that receives the recorded print jobs and commands; "" if the fake is inactive.
std::string fake_printers_dump_dir();

}}}} // namespace Slic3r::GUI::Mcp::Testing

#endif // slic3r_Mcp_testing_FakePrinters_hpp_
