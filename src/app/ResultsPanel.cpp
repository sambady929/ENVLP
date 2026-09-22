#include "ResultsPanel.h"

#include <wx/clipbrd.h>

namespace symcirc {

ResultsPanel::ResultsPanel(wxWindow* parent) : wxPanel(parent) {
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    text_ = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
                           wxTE_MULTILINE | wxTE_READONLY | wxTE_RICH2);
    wxFont f(10, wxFONTFAMILY_TELETYPE, wxFONTSTYLE_NORMAL,
             wxFONTWEIGHT_NORMAL);
    text_->SetFont(f);

    auto* bar = new wxBoxSizer(wxHORIZONTAL);
    auto* copy = new wxButton(this, wxID_ANY, "Copy all");
    auto* clr = new wxButton(this, wxID_ANY, "Clear");
    bar->Add(copy, 0, wxRIGHT, 6);
    bar->Add(clr, 0, 0);
    bar->AddStretchSpacer();

    sizer->Add(text_, 1, wxEXPAND | wxALL, 4);
    sizer->Add(bar, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);
    SetSizer(sizer);

    copy->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (wxTheClipboard->Open()) {
            wxTheClipboard->SetData(new wxTextDataObject(text_->GetValue()));
            wxTheClipboard->Close();
        }
    });
    clr->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { clear(); });
}

void ResultsPanel::set_text(const std::string& utf8) {
    text_->SetValue(wxString::FromUTF8(utf8));
    text_->SetInsertionPointEnd();
}

void ResultsPanel::append(const std::string& utf8) {
    text_->AppendText(wxString::FromUTF8(utf8));
    text_->SetInsertionPointEnd();
}

void ResultsPanel::clear() { text_->Clear(); }

} // namespace symcirc
