#include "McpPreferences.hpp"

#include <string>

#include <wx/clipbrd.h>
#include <wx/dataobj.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/string.h>
#include <wx/window.h>

#include "libslic3r/AppConfig.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/Preferences.hpp"
#include "slic3r/GUI/Widgets/CheckBox.hpp"
#include "slic3r/GUI/Widgets/Label.hpp"

#include "McpService.hpp"

namespace Slic3r { namespace GUI { namespace Mcp {

namespace {

wxString status_text()
{
    const ServiceStatus status = Service::instance().status();
    if (status.running) {
        const AppConfig* config = wxGetApp().app_config;
        if (config != nullptr && !config->get_bool("mcp_enabled"))
            // A test launch: the environment switched the server on, whatever the checkbox says.
            return wxString::Format(_L("Listening on %s (switched on by the environment)"), wxString::FromUTF8(status.url));
        return wxString::Format(_L("Listening on %s"), wxString::FromUTF8(status.url));
    }
    if (status.enabled && !status.error.empty())
        return wxString::Format(_L("Not running: %s"), wxString::FromUTF8(status.error));
    return _L("Stopped");
}

void copy_to_clipboard(const std::string& text)
{
    if (wxTheClipboard->Open()) {
        wxTheClipboard->SetData(new wxTextDataObject(wxString::FromUTF8(text)));
        wxTheClipboard->Close();
    }
}

} // namespace

void add_preferences_section(PreferencesDialog& dialog, wxWindow* parent, wxSizer* sizer)
{
    AppConfig* config = wxGetApp().app_config;
    if (config->get("mcp_port").empty())
        config->set("mcp_port", std::to_string(Service::DEFAULT_PORT));

    sizer->Add(dialog.create_item_title(_L("MCP server (AI agent control)")), 1, wxEXPAND);

    // Status line, updated by the controls below.
    auto* status = new wxStaticText(parent, wxID_ANY, status_text());
    status->SetForegroundColour(DESIGN_GRAY900_COLOR);
    status->SetFont(::Label::Body_14);
    auto refresh_status = [status]() {
        status->SetLabel(status_text());
        status->GetParent()->Layout();
    };

    // Enable.
    {
        const wxString tip = _L("Lets an AI agent on this computer (for example Claude Code) inspect and change the open project through the "
                                "Model Context Protocol. The server listens on this computer only and requires the access token. Off by default.");
        wxBoxSizer*    row = dialog.create_item_label(_L("Enable MCP server"), tip);
        auto*          checkbox = new ::CheckBox(parent);
        checkbox->SetValue(config->get_bool("mcp_enabled"));
        checkbox->SetToolTip(tip);
        row->Add(checkbox, 0, wxALIGN_CENTER);
        checkbox->Bind(wxEVT_TOGGLEBUTTON, [config, checkbox, refresh_status](wxCommandEvent& event) {
            config->set_bool("mcp_enabled", checkbox->GetValue());
            config->save();
            Service::instance().apply_preferences();
            refresh_status();
            event.Skip();
        });
        sizer->Add(row);
    }

    // Port.
    sizer->Add(dialog.create_item_input(_L("Port"), "", _L("TCP port on 127.0.0.1 the MCP server listens on. Client configurations hold this number."),
                                        "mcp_port", [refresh_status](wxString) {
                                            Service::instance().apply_preferences();
                                            refresh_status();
                                        }));

    // Status.
    {
        wxBoxSizer* row = dialog.create_item_label(_L("Status"), _L("Whether the MCP server is running."));
        row->Add(status, 0, wxALIGN_CENTER);
        sizer->Add(row);
    }

    // What the agent may touch.
    sizer->Add(dialog.create_item_checkbox(_L("Allow the agent to edit G-code settings"),
                                           _L("Start, end, layer-change and other G-code text in the printer, filament and process settings can be "
                                              "read by the agent but not changed, unless this is switched on. G-code can damage a printer."),
                                           "mcp_allow_gcode_edit"));
    // Printers (DESIGN.md D13). Off by default; only this dialog writes them, no MCP tool can.
    sizer->Add(dialog.create_item_checkbox(_L("Allow AI agents to read printer status"),
                                           _L("Lets the agent list your printers and read their state, job progress, temperatures and "
                                              "filament trays. Access codes, serial numbers and addresses are never shown to it."),
                                           "mcp_printer_read"));
    sizer->Add(dialog.create_item_checkbox(_L("Allow AI agents to send print jobs"),
                                           _L("Lets the agent ask to send a sliced plate to a printer, and to pause, resume or stop a "
                                              "print. Every job and every stop needs your click in a confirmation window first, "
                                              "unless \"Send print jobs without asking me\" is on."),
                                           "mcp_printer_send"));
    sizer->Add(dialog.create_item_checkbox(_L("Send print jobs without asking me"),
                                           _L("With \"Allow AI agents to send print jobs\" on, the agent's print jobs and stops go to the "
                                              "printer without a confirmation window, so you can start prints from another device. "
                                              "OrcaSlicer still checks that the printer is online, idle and loaded with matching filament, but "
                                              "nobody checks the bed is clear."),
                                           "mcp_printer_send_unattended"));

    // Connecting a client.
    sizer->Add(dialog.create_item_button(_L("Connect Claude Code"), _L("Copy command"),
                                         _L("Copies the command that registers this server with Claude Code. Run it in a terminal. It contains the access token."),
                                         "", []() { copy_to_clipboard(Service::instance().claude_code_command()); }));
    sizer->Add(dialog.create_item_button(_L("Client configuration (.mcp.json)"), _L("Copy JSON"),
                                         _L("Copies a .mcp.json entry for this server. It contains the access token."), "",
                                         []() { copy_to_clipboard(Service::instance().mcp_json_snippet()); }));
    sizer->Add(dialog.create_item_button(_L("Access token"), _L("Copy token"), _L("Copies the token clients must send as \"Authorization: Bearer <token>\"."), "",
                                         []() { copy_to_clipboard(Service::instance().token()); }));
    sizer->Add(dialog.create_item_button(_L("New access token"), _L("Regenerate"),
                                         _L("Replaces the access token. Clients configured with the old token stop working until they are given the new one."), "",
                                         [refresh_status]() {
                                             Service::instance().regenerate_token();
                                             refresh_status();
                                         }));
}

}}} // namespace Slic3r::GUI::Mcp
