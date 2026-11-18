#include "McpDialogGuard.hpp"

#include <algorithm>
#include <iterator>
#include <typeinfo>
#include <utility>

#include <boost/log/trivial.hpp>

#include <wx/dialog.h>
#include <wx/dirdlg.h>
#include <wx/filedlg.h>
#include <wx/html/htmlwin.h>
#include <wx/msgdlg.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/window.h>

#include "McpUtil.hpp"

namespace Slic3r { namespace GUI { namespace Mcp {

namespace {

const size_t MAX_DIALOG_TEXT = 2000;

std::string utf8(const wxString& text)
{
    return std::string(text.utf8_str().data());
}

std::string trimmed(std::string text)
{
    const char* blanks = " \t\r\n";
    const size_t first = text.find_first_not_of(blanks);
    if (first == std::string::npos)
        return {};
    const size_t last = text.find_last_not_of(blanks);
    return text.substr(first, last - first + 1);
}

void append_text(std::vector<std::string>& out, const wxString& text)
{
    const std::string value = trimmed(utf8(text));
    if (!value.empty() && (out.empty() || out.back() != value))
        out.push_back(value);
}

// Static texts, HTML panes and read-only text boxes of a dialog, in creation order.
void collect_text(wxWindow* window, std::vector<std::string>& out, int depth)
{
    if (window == nullptr || depth > 12)
        return;
    for (wxWindow* child : window->GetChildren()) {
        if (child == nullptr || !child->IsShown())
            continue;
        if (auto* label = dynamic_cast<wxStaticText*>(child))
            append_text(out, label->GetLabel());
        else if (auto* html = dynamic_cast<wxHtmlWindow*>(child))
            append_text(out, html->ToText());
        else if (auto* text = dynamic_cast<wxTextCtrl*>(child)) {
            if (!text->IsEditable() || text->IsMultiLine())
                append_text(out, text->GetValue());
        } else
            collect_text(child, out, depth + 1);
    }
}

struct Buttons
{
    bool ok { false };
    bool cancel { false };
    bool yes { false };
    bool no { false };
    bool close { false };
};

Buttons buttons_of(wxDialog* dialog)
{
    Buttons buttons;
    if (auto* message = dynamic_cast<wxMessageDialogBase*>(dialog)) {
        const long style = message->GetMessageDialogStyle();
        buttons.yes = buttons.no = (style & wxYES_NO) != 0;
        buttons.cancel = (style & wxCANCEL) != 0;
        buttons.ok = (style & wxOK) != 0 || !buttons.yes;
        return buttons;
    }
    if (dynamic_cast<wxFileDialog*>(dialog) != nullptr || dynamic_cast<wxDirDialog*>(dialog) != nullptr) {
        buttons.ok = buttons.cancel = true;
        return buttons;
    }
    buttons.ok = dialog->FindWindow(wxID_OK) != nullptr;
    buttons.cancel = dialog->FindWindow(wxID_CANCEL) != nullptr;
    buttons.yes = dialog->FindWindow(wxID_YES) != nullptr;
    buttons.no = dialog->FindWindow(wxID_NO) != nullptr;
    buttons.close = dialog->FindWindow(wxID_CLOSE) != nullptr;
    return buttons;
}

int default_answer(const Buttons& buttons)
{
    if (buttons.cancel) return wxID_CANCEL;
    if (buttons.no) return wxID_NO;
    if (buttons.close) return wxID_CLOSE;
    if (buttons.ok) return wxID_OK;
    return wxID_CANCEL;
}

int answer_id(DialogAnswer answer, const Buttons& buttons)
{
    switch (answer) {
    case DialogAnswer::Ok: return wxID_OK;
    case DialogAnswer::Cancel: return wxID_CANCEL;
    case DialogAnswer::Yes: return wxID_YES;
    case DialogAnswer::No: return wxID_NO;
    case DialogAnswer::Close: return wxID_CLOSE;
    case DialogAnswer::Default: break;
    }
    return default_answer(buttons);
}

std::string answer_name(int id)
{
    switch (id) {
    case wxID_OK: return "ok";
    case wxID_CANCEL: return "cancel";
    case wxID_YES: return "yes";
    case wxID_NO: return "no";
    case wxID_CLOSE: return "close";
    default: return std::to_string(id);
    }
}

const DialogRule* match(const std::vector<DialogRule>* rules, const DialogRecord& record)
{
    if (rules == nullptr)
        return nullptr;
    for (const DialogRule& rule : *rules)
        if (contains_ci(record.title, rule.title_contains) && contains_ci(record.message, rule.text_contains))
            return &rule;
    return nullptr;
}

} // namespace

DialogGuard::~DialogGuard() { uninstall(); }

void DialogGuard::install()
{
    if (!m_installed) {
        Register();
        m_installed = true;
    }
}

void DialogGuard::uninstall()
{
    if (m_installed) {
        Unregister();
        m_installed = false;
    }
    m_user_dialogs.clear();
    m_scopes.clear();
}

void DialogGuard::begin_call(const std::vector<DialogRule>* call_rules, const std::vector<DialogRule>* tool_rules, std::vector<DialogRecord>* records)
{
    m_call_rules = call_rules;
    m_tool_rules = tool_rules;
    m_call_records = records;
}

void DialogGuard::end_call()
{
    m_call_rules = nullptr;
    m_tool_rules = nullptr;
    m_call_records = nullptr;
}

void DialogGuard::add_scope(std::shared_ptr<DialogScope> scope)
{
    if (scope)
        m_scopes.push_back(std::move(scope));
}

void DialogGuard::remove_scope(const std::shared_ptr<DialogScope>& scope)
{
    m_scopes.erase(std::remove(m_scopes.begin(), m_scopes.end(), scope), m_scopes.end());
}

std::string DialogGuard::user_dialog_title() const
{
    return m_user_dialogs.empty() ? std::string() : m_user_dialogs.back().title;
}

DialogRecord DialogGuard::describe(wxDialog* dialog)
{
    DialogRecord record;
    if (dialog == nullptr)
        return record;
    record.kind = typeid(*dialog).name();
    record.title = trimmed(utf8(dialog->GetTitle()));

    std::vector<std::string> parts;
    if (auto* message = dynamic_cast<wxMessageDialogBase*>(dialog)) {
        if (record.title.empty())
            record.title = trimmed(utf8(message->GetCaption()));
        append_text(parts, message->GetMessage());
        append_text(parts, message->GetExtendedMessage());
    } else if (auto* file = dynamic_cast<wxFileDialog*>(dialog)) {
        append_text(parts, "File dialog: " + file->GetMessage());
    } else if (auto* dir = dynamic_cast<wxDirDialog*>(dialog)) {
        append_text(parts, "Folder dialog: " + dir->GetMessage());
    } else {
        collect_text(dialog, parts, 0);
    }
    for (const std::string& part : parts) {
        if (!record.message.empty())
            record.message += "\n";
        record.message += part;
    }
    if (record.message.size() > MAX_DIALOG_TEXT)
        record.message = record.message.substr(0, MAX_DIALOG_TEXT) + "...";
    return record;
}

int DialogGuard::Enter(wxDialog* dialog)
{
    DialogRecord record = describe(dialog);

    if (m_call_records != nullptr) {
        // A tool call is running: nobody is there to read the dialog.
        const DialogRule* rule = match(m_call_rules, record);
        if (rule == nullptr)
            rule = match(m_tool_rules, record);
        const Buttons buttons = buttons_of(dialog);
        const int     id = rule != nullptr ? answer_id(rule->answer, buttons) : default_answer(buttons);
        record.answered = answer_name(id);
        record.blocked = rule == nullptr || !rule->informational;
        BOOST_LOG_TRIVIAL(info) << "MCP: answered dialog \"" << record.title << "\" (" << record.kind << ") with " << record.answered
                                << (record.blocked ? " [blocked]" : "");
        m_call_records->push_back(std::move(record));
        return id;
    }

    // Outside a call, a job may have asked for particular dialogs to be answered (a slicing error
    // while nobody watches). Anything else is the user's own dialog.
    for (auto scope = m_scopes.rbegin(); scope != m_scopes.rend(); ++scope) {
        if (const DialogRule* rule = match(&(*scope)->rules, record); rule != nullptr) {
            const int id = answer_id(rule->answer, buttons_of(dialog));
            record.answered = answer_name(id);
            record.blocked = !rule->informational;
            BOOST_LOG_TRIVIAL(info) << "MCP: job answered dialog \"" << record.title << "\" with " << record.answered;
            (*scope)->records.push_back(std::move(record));
            return id;
        }
    }

    m_user_dialogs.push_back(UserDialog { dialog, record.title.empty() ? record.kind : record.title });
    return wxID_NONE;
}

void DialogGuard::Exit(wxDialog* dialog)
{
    for (auto it = m_user_dialogs.rbegin(); it != m_user_dialogs.rend(); ++it)
        if (it->dialog == dialog) {
            m_user_dialogs.erase(std::next(it).base());
            break;
        }
}

}}} // namespace Slic3r::GUI::Mcp
