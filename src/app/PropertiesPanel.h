#pragma once
#include "Document.h"

#include <wx/wx.h>
#include <wx/spinctrl.h>
#include <wx/wrapsizer.h>

#include <functional>
#include <string>

namespace envlp {

// Right-side property editor for the current selection:
//  - component: value, K links, size offset (dB)
//  - devices: non-ideality checkbox + coefficient dropdown + exponent
//    dropdown (engineering notation), e.g. Cgs = 100e-13, ro = 10e3
//  - net label: name
//  - analysis: input source, output, f0, threshold (always visible)
class PropertiesPanel : public wxScrolledWindow {
public:
    explicit PropertiesPanel(wxWindow* parent);

    // Rebuild the UI for the given selection ("" = nothing selected).
    // `doc` may be null (nothing to edit).
    void refresh(Document* doc, const std::string& selection);

    std::function<void()> on_edited; // any field changed
    std::function<void(const std::string&)> on_selection_changed;
    // Set/clear the net name of a wire (creates/removes a label) and set a
    // label's font size; both route through the canvas so undo is consistent.
    std::function<void(int, const std::string&)> on_wire_name;
    std::function<void(int, int)> on_label_font;

private:
    Document* doc_ = nullptr;
    std::string sel_;
    bool rebuilding_ = false;
    // Wrapping container for the parameter "cards" (created lazily; device
    // parameters tile into it instead of forming one long list).
    wxWrapSizer* cards_ = nullptr;

    void add_header(const wxString& text);
    wxTextCtrl* add_text(const wxString& label, const wxString& value,
                         std::function<void(const wxString&)> on_change);
    wxSpinCtrl* add_spin(const wxString& label, int value, int min, int max,
                         std::function<void(int)> on_change);
    void add_param_row(syms::Component* comp, const std::string& name,
                       const std::string& unit, bool parasitic,
                       const std::string& default_text);
    void add_mantissa_exp(syms::Component* comp, const std::string& name,
                          bool parasitic, const wxString& default_text,
                          const wxString& unit);
    // The wrapping container the cards tile into (created lazily).
    wxWrapSizer* cards_host();
    // A value editor: a number field plus an SI-suffix dropdown, so the
    // prefix is chosen from a list (unambiguous: "M (mega)" vs "m (milli)")
    // rather than typed. Writes "<number><suffix>" into `target`.
    void add_value_row(const wxString& label, std::string* target,
                       const std::string& unit);
    // A free-text scalar row (a source's DC/AC value): type any engineering
    // value ("10k", "2.5p", "1e-13").
    void add_scalar_row(const wxString& label, std::string* target,
                        const std::string& unit);
    bool rename_component(const std::string& old_ref, const wxString& new_ref);

    void on_analysis_changed(wxCommandEvent& e);
    wxDECLARE_EVENT_TABLE();
};

} // namespace envlp
