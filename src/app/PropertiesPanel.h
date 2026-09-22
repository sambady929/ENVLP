#pragma once
#include "Document.h"

#include <wx/wx.h>
#include <wx/spinctrl.h>

#include <functional>
#include <string>

namespace symcirc {

// Right-side property editor for the current selection:
//  - component: value, size offset (dB), parasitic toggles + estimates
//  - net label: name
//  - analysis: input source, output, f0, threshold (always visible)
class PropertiesPanel : public wxScrolledWindow {
public:
    explicit PropertiesPanel(wxWindow* parent);

    // Rebuild the UI for the given selection ("" = nothing selected).
    // `doc` may be null (nothing to edit).
    void refresh(Document* doc, const std::string& selection);

    std::function<void()> on_edited; // any field changed

private:
    Document* doc_ = nullptr;
    std::string sel_;
    bool rebuilding_ = false;

    void add_header(const wxString& text);
    wxTextCtrl* add_text(const wxString& label, const wxString& value,
                         std::function<void(const wxString&)> on_change);
    wxSpinCtrl* add_spin(const wxString& label, int value, int min, int max,
                         std::function<void(int)> on_change);

    void on_analysis_changed(wxCommandEvent& e);
    wxDECLARE_EVENT_TABLE();
};

} // namespace symcirc
