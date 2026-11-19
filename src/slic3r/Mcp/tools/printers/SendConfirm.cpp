#include "SendConfirm.hpp"

#include <algorithm>
#include <cmath>

#include <wx/button.h>
#include <wx/dialog.h>
#include <wx/image.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>
#include <wx/timer.h>

#include <boost/log/trivial.hpp>

#include "libslic3r/GCode/ThumbnailData.hpp"
#include "slic3r/GUI/I18N.hpp"

namespace Slic3r { namespace GUI { namespace Mcp { namespace Printers {

namespace {

wxColour colour_of(const std::string& hex)
{
    wxColour c;
    if (hex.size() >= 7 && c.Set(wxString::FromUTF8(hex.substr(0, 7))))
        return c;
    return wxColour(128, 128, 128);
}

wxBitmap thumbnail_bitmap(const ThumbnailData* data, int max_side)
{
    if (data == nullptr || data->width == 0 || data->height == 0 || data->pixels.size() < size_t(data->width) * data->height * 4)
        return wxNullBitmap;
    const int w = int(data->width), h = int(data->height);
    wxImage   image(w, h);
    image.InitAlpha();
    unsigned char* rgb   = image.GetData();
    unsigned char* alpha = image.GetAlpha();
    for (int y = 0; y < h; ++y) {
        // ThumbnailData rows run bottom-up (OpenGL read-back).
        const unsigned char* src = &data->pixels[size_t(h - 1 - y) * w * 4];
        for (int x = 0; x < w; ++x) {
            const size_t px = size_t(y) * w + x;
            rgb[px * 3 + 0] = src[x * 4 + 0];
            rgb[px * 3 + 1] = src[x * 4 + 1];
            rgb[px * 3 + 2] = src[x * 4 + 2];
            alpha[px]       = src[x * 4 + 3];
        }
    }
    const double scale = std::min(1.0, double(max_side) / double(std::max(w, h)));
    if (scale < 1.0)
        image.Rescale(std::max(1, int(w * scale)), std::max(1, int(h * scale)), wxIMAGE_QUALITY_HIGH);
    return wxBitmap(image);
}

wxWindow* swatch(wxWindow* parent, const std::string& hex)
{
    auto* p = new wxPanel(parent, wxID_ANY, wxDefaultPosition, parent->FromDIP(wxSize(16, 16)), wxBORDER_SIMPLE);
    p->SetBackgroundColour(colour_of(hex));
    return p;
}

class ConfirmDialog;
ConfirmDialog* g_open = nullptr;

class ConfirmDialog : public wxDialog
{
public:
    ConfirmDialog(ConfirmRequest request, std::function<void(ConfirmAnswer)> done)
        // No parent: the window must stay visible when the main window is minimised, and it must not
        // be a child the main window's modal machinery knows about.
        : wxDialog(nullptr,
                   wxID_ANY,
                   wxString::FromUTF8(std::string(CONFIRM_TITLE_PREFIX) + (request.stop ? " stopping a print" : " print job")),
                   wxDefaultPosition,
                   wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE | wxSTAY_ON_TOP | wxDIALOG_NO_PARENT,
                   CONFIRM_WINDOW_NAME)
        , m_done(std::move(done))
        , m_remaining(std::max(1, int(std::lround(request.timeout_s))))
        , m_stop(request.stop)
        , m_timer(this)
    {
        build(request);
        Bind(wxEVT_TIMER, &ConfirmDialog::on_tick, this);
        Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent&) { answer(ConfirmAnswer::Declined); });
        Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& e) {
            if (e.GetKeyCode() == WXK_ESCAPE)
                answer(ConfirmAnswer::Declined);
            else
                e.Skip();
        });
        m_timer.Start(1000);
    }

    ~ConfirmDialog() override
    {
        m_timer.Stop();
        if (g_open == this)
            g_open = nullptr;
    }

private:
    void build(const ConfirmRequest& r)
    {
        auto* root = new wxBoxSizer(wxVERTICAL);
        const int gap = FromDIP(10);

        auto* head = new wxStaticText(this, wxID_ANY,
                                      r.stop ? _L("An AI agent asks to STOP the print on this printer.")
                                             : _L("An AI agent asks to send a print job to this printer."));
        wxFont bold = head->GetFont();
        bold.SetWeight(wxFONTWEIGHT_BOLD);
        bold.SetPointSize(bold.GetPointSize() + 1);
        head->SetFont(bold);
        root->Add(head, 0, wxALL, gap);

        auto* body = new wxBoxSizer(wxHORIZONTAL);
        if (wxBitmap bmp = thumbnail_bitmap(r.thumbnail.get(), FromDIP(200)); bmp.IsOk())
            body->Add(new wxStaticBitmap(this, wxID_ANY, bmp), 0, wxRIGHT | wxALIGN_TOP, gap);

        auto* info = new wxFlexGridSizer(2, FromDIP(4), FromDIP(12));
        auto  row  = [&](const std::string& label, const std::string& value, bool strong = false) {
            auto* l = new wxStaticText(this, wxID_ANY, wxString::FromUTF8(label));
            auto* v = new wxStaticText(this, wxID_ANY, wxString::FromUTF8(value));
            if (strong)
                v->SetFont(bold);
            info->Add(l);
            info->Add(v);
        };
        row(_u8L("Printer"), r.printer_name + (r.printer_model.empty() ? "" : "  (" + r.printer_model + ")"), true);
        if (!r.headline.empty())
            row(r.stop ? _u8L("Job") : _u8L("Plate"), r.headline);
        for (const auto& [label, value] : r.facts)
            row(label, value);
        body->Add(info, 1, wxEXPAND);
        root->Add(body, 0, wxLEFT | wxRIGHT | wxEXPAND, gap);

        if (!r.rows.empty()) {
            root->Add(new wxStaticText(this, wxID_ANY, _L("Filament to printer tray:")), 0, wxLEFT | wxRIGHT | wxTOP, gap);
            auto* grid = new wxFlexGridSizer(6, FromDIP(4), FromDIP(6));
            for (const ConfirmRow& m : r.rows) {
                grid->Add(new wxStaticText(this, wxID_ANY, wxString::Format("%d", m.filament)), 0, wxALIGN_CENTER_VERTICAL);
                grid->Add(swatch(this, m.filament_color), 0, wxALIGN_CENTER_VERTICAL);
                grid->Add(new wxStaticText(this, wxID_ANY, wxString::FromUTF8(m.filament_type + "  \xE2\x86\x92")), 0, wxALIGN_CENTER_VERTICAL);
                grid->Add(swatch(this, m.tray_color), 0, wxALIGN_CENTER_VERTICAL);
                grid->Add(new wxStaticText(this, wxID_ANY, wxString::FromUTF8(m.tray + "  " + m.tray_type)), 0, wxALIGN_CENTER_VERTICAL);
                auto* w = new wxStaticText(this, wxID_ANY, m.warn ? _L("colour differs") : wxString());
                if (m.warn)
                    w->SetForegroundColour(wxColour(200, 120, 0));
                grid->Add(w, 0, wxALIGN_CENTER_VERTICAL);
            }
            root->Add(grid, 0, wxLEFT | wxRIGHT | wxTOP, gap);
        }

        for (const std::string& warning : r.warnings) {
            auto* w = new wxStaticText(this, wxID_ANY, wxString::FromUTF8("\xE2\x9A\xA0 " + warning));
            w->SetForegroundColour(wxColour(200, 120, 0));
            w->Wrap(FromDIP(460));
            root->Add(w, 0, wxLEFT | wxRIGHT | wxTOP, gap);
        }

        m_countdown = new wxStaticText(this, wxID_ANY, wxEmptyString);
        root->Add(m_countdown, 0, wxALL, gap);
        update_countdown();

        auto* buttons = new wxBoxSizer(wxHORIZONTAL);
        buttons->AddStretchSpacer();
        // Native buttons on purpose (see the header). Cancel is the default button: Enter never sends.
        m_ok     = new wxButton(this, wxID_ANY, r.stop ? _L("Stop print") : _L("Send"));
        auto* no = new wxButton(this, wxID_ANY, _L("Cancel"));
        no->SetDefault();
        no->SetFocus();
        // A click that was meant for something else must not send: the button wakes up after a moment.
        m_ok->Disable();
        m_ok->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { answer(ConfirmAnswer::Confirmed); });
        no->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { answer(ConfirmAnswer::Declined); });
        buttons->Add(m_ok, 0, wxRIGHT, gap);
        buttons->Add(no, 0);
        root->Add(buttons, 0, wxEXPAND | wxALL, gap);

        SetSizerAndFit(root);
        CentreOnScreen();
    }

    void update_countdown()
    {
        if (m_countdown != nullptr)
            m_countdown->SetLabel(wxString::Format(m_stop ? _L("Closes without stopping in %d:%02d.") : _L("Closes without sending in %d:%02d."),
                                                   m_remaining / 60, m_remaining % 60));
    }

    void on_tick(wxTimerEvent&)
    {
        ++m_ticks;
        if (m_ticks >= 1 && m_ok != nullptr && !m_ok->IsEnabled() && !m_answered)
            m_ok->Enable();
        --m_remaining;
        if (m_remaining <= 0) {
            answer(ConfirmAnswer::TimedOut);
            return;
        }
        update_countdown();
    }

    void answer(ConfirmAnswer a)
    {
        if (m_answered)
            return;
        m_answered = true;
        m_timer.Stop();
        BOOST_LOG_TRIVIAL(info) << "MCP: print confirmation answered: " << answer_name(a);
        if (g_open == this)
            g_open = nullptr;
        Hide();
        auto done = std::move(m_done);
        Destroy(); // deferred by wx for top-level windows
        if (done)
            done(a);
    }

    std::function<void(ConfirmAnswer)> m_done;
    int                                m_remaining;
    int                                m_ticks { 0 };
    bool                               m_answered { false };
    bool                               m_stop { false };
    wxTimer                            m_timer;
    wxStaticText*                      m_countdown { nullptr };
    wxButton*                          m_ok { nullptr };
};

} // namespace

void ask_confirmation(ConfirmRequest request, std::function<void(ConfirmAnswer)> done)
{
    auto* dlg = new ConfirmDialog(std::move(request), std::move(done));
    g_open    = dlg;
    dlg->Show();
    dlg->Raise();
    dlg->RequestUserAttention(wxUSER_ATTENTION_ERROR);
}

bool confirmation_open() { return g_open != nullptr; }

const char* answer_name(ConfirmAnswer answer)
{
    switch (answer) {
    case ConfirmAnswer::Confirmed: return "confirmed";
    case ConfirmAnswer::Declined: return "declined";
    case ConfirmAnswer::TimedOut: return "timed_out";
    }
    return "declined";
}

}}}} // namespace Slic3r::GUI::Mcp::Printers
