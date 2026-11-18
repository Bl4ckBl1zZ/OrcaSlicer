#ifndef slic3r_Mcp_Preferences_hpp_
#define slic3r_Mcp_Preferences_hpp_

// The "MCP server" section of the Preferences dialog.

class wxSizer;
class wxWindow;

namespace Slic3r { namespace GUI {

class PreferencesDialog;

namespace Mcp {

// Appends the section to `sizer`. `parent` is the window the dialog's rows are created on.
void add_preferences_section(PreferencesDialog& dialog, wxWindow* parent, wxSizer* sizer);

}}} // namespace Slic3r::GUI::Mcp

#endif
