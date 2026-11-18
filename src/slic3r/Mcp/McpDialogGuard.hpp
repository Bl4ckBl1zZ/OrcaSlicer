#ifndef slic3r_Mcp_DialogGuard_hpp_
#define slic3r_Mcp_DialogGuard_hpp_

// Modal dialogs and automation. wxModalDialogHook calls Enter() at the top of every
// wxDialog::ShowModal(). While a tool call runs (or a job that asked for it), the guard answers the
// dialog from the tool's policy instead of showing it, and records what a person would have read.
// At any other time the dialog is the user's: it is shown as usual and counted, so the dispatcher
// knows the UI is in the middle of something.
//
// Main thread only.

#include <memory>
#include <string>
#include <vector>

#include <wx/modalhook.h>

#include "McpTool.hpp"

class wxDialog;

namespace Slic3r { namespace GUI { namespace Mcp {

// Dialog handling of a long-running job (Jobs.hpp): the rules to answer with and what was seen.
struct DialogScope
{
    std::vector<DialogRule>   rules;
    std::vector<DialogRecord> records;
};

class DialogGuard : public wxModalDialogHook
{
public:
    DialogGuard() = default;
    ~DialogGuard() override;

    void install();
    void uninstall();

    // A tool call is on the stack: answer with `call_rules` first, then `tool_rules`, and append to
    // `records`. Calls do not nest.
    void begin_call(const std::vector<DialogRule>* call_rules, const std::vector<DialogRule>* tool_rules, std::vector<DialogRecord>* records);
    void end_call();

    // Dialogs that open outside a call while a scope is registered are answered with its rules.
    void add_scope(std::shared_ptr<DialogScope> scope);
    void remove_scope(const std::shared_ptr<DialogScope>& scope);

    // Dialogs of the user's currently on screen (not answered by the guard).
    int         user_dialogs_open() const { return static_cast<int>(m_user_dialogs.size()); }
    std::string user_dialog_title() const;

    // Title, text and class of a dialog; public for tests and for the debug tools.
    static DialogRecord describe(wxDialog* dialog);

protected:
    int  Enter(wxDialog* dialog) override;
    void Exit(wxDialog* dialog) override;

private:
    struct UserDialog
    {
        wxDialog*   dialog;
        std::string title;
    };

    bool                                      m_installed { false };
    const std::vector<DialogRule>*            m_call_rules { nullptr };
    const std::vector<DialogRule>*            m_tool_rules { nullptr };
    std::vector<DialogRecord>*                m_call_records { nullptr };
    std::vector<std::shared_ptr<DialogScope>> m_scopes;
    std::vector<UserDialog>                   m_user_dialogs;
};

}}} // namespace Slic3r::GUI::Mcp

#endif
