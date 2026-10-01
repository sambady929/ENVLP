#include "ResultsPanel.h"

#include <wx/clipbrd.h>

namespace envlp {

namespace {
void copy_to_clipboard(const wxString& s) {
    if (wxTheClipboard->Open()) {
        wxTheClipboard->SetData(new wxTextDataObject(s));
        wxTheClipboard->Close();
    }
}
} // namespace

ResultsPanel::ResultsPanel(wxWindow* parent) : wxPanel(parent) {
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    text_ = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
                           wxTE_MULTILINE | wxTE_READONLY);
    wxFont f(10, wxFONTFAMILY_TELETYPE, wxFONTSTYLE_NORMAL,
             wxFONTWEIGHT_NORMAL);
    text_->SetFont(f);

    auto* bar = new wxBoxSizer(wxHORIZONTAL);
    auto* copy = new wxButton(this, wxID_ANY, "Copy all");
    auto* latex = new wxButton(this, wxID_ANY, "Copy LaTeX");
    auto* clr = new wxButton(this, wxID_ANY, "Clear");
    bar->Add(copy, 0, wxRIGHT, 6);
    bar->Add(latex, 0, wxRIGHT, 6);
    bar->Add(clr, 0, 0);
    bar->AddStretchSpacer();

    sizer->Add(text_, 1, wxEXPAND | wxALL, 4);
    sizer->Add(bar, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);
    SetSizer(sizer);

    copy->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        copy_to_clipboard(text_->GetValue());
    });
    latex->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        wxString l = wxString::FromUTF8(latex_);
        if (l.empty()) l = text_->GetValue();
        copy_to_clipboard(l);
    });
    clr->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { clear(); });
}

void ResultsPanel::set_text(const std::string& utf8) {
    text_->SetValue(wxString::FromUTF8(utf8));
    text_->SetInsertionPointEnd();
}

void ResultsPanel::set_latex(const std::string& latex) { latex_ = latex; }

void ResultsPanel::append(const std::string& utf8) {
    text_->AppendText(wxString::FromUTF8(utf8));
    text_->SetInsertionPointEnd();
}

void ResultsPanel::clear() {
    text_->Clear();
    latex_.clear();
}

} // namespace envlp
