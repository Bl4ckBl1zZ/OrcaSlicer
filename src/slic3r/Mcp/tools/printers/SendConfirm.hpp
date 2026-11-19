#ifndef slic3r_Mcp_SendConfirm_hpp_
#define slic3r_Mcp_SendConfirm_hpp_

// The window that asks the user before an AI agent's print job is sent (or a print is stopped).
//
// It is deliberately out of reach of the MCP server:
//   - It is modeless (Show, not ShowModal), so wxModalDialogHook, and with it the DialogGuard that
//     answers dialogs during tool calls and jobs, never sees it. DialogGuard also skips it by name
//     should it ever be shown modally.
//   - No tool can click, close or list it: debug_window only ends modal dialogs, and nothing in the
//     MCP server keeps a pointer to it except this file.
//   - The answer is only the user's click (Send / Cancel / closing the window) or the timeout.
// Its buttons are native buttons so the test harness can click them through the operating system
// (tests/mcp/orca_mcp/confirm_ui.py), the same way a person does.
//
// Main thread only.

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {
struct ThumbnailData;
namespace GUI { namespace Mcp { namespace Printers {

// The window's name (wxWindow::GetName) and the start of its title.
inline constexpr const char* CONFIRM_WINDOW_NAME  = "mcp_print_confirm";
inline constexpr const char* CONFIRM_TITLE_PREFIX = "OrcaSlicer: confirm";

struct ConfirmRow
{
    int         filament { 0 };   // 1-based
    std::string filament_type;
    std::string filament_color;   // "#RRGGBB"
    std::string tray;             // "A1", "ext"
    std::string tray_type;
    std::string tray_color;       // "#RRGGBB"
    bool        warn { false };   // colour differs
};

struct ConfirmRequest
{
    bool        stop { false };       // false: send a print job; true: stop a running print
    std::string printer_name;
    std::string printer_model;
    std::string headline;             // "Plate 1 of \"Box\""
    std::shared_ptr<ThumbnailData> thumbnail; // may be null
    std::vector<ConfirmRow>        rows;
    std::vector<std::pair<std::string, std::string>> facts; // label, value
    std::vector<std::string>       warnings;
    double      timeout_s { 300. };
};

enum class ConfirmAnswer { Confirmed, Declined, TimedOut };

// Shows the window and returns. `done` is called once, on the main thread, with the answer; the
// window is gone by then. Only one window is open at a time (callers queue).
void ask_confirmation(ConfirmRequest request, std::function<void(ConfirmAnswer)> done);
bool confirmation_open();
const char* answer_name(ConfirmAnswer answer);

}}}} // namespace Slic3r::GUI::Mcp::Printers

#endif
