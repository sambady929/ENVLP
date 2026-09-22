#pragma once
#include "Document.h"
#include "SchematicCanvas.h" // Tool

#include <wx/wx.h>

namespace symcirc {

// Left-side tool/component palette.
class PalettePanel : public wxPanel {
public:
    explicit PalettePanel(wxWindow* parent, Document* doc);

    std::function<void()> on_tool_changed; // canvas reads tool()/kind()

    Tool tool() const;
    syms::Kind place_kind() const;

private:
    Document* doc_;
    wxListBox* tools_;
    wxListBox* comps_;

    void on_tool_sel(wxCommandEvent& e);
    void on_comp_sel(wxCommandEvent& e);
    wxDECLARE_EVENT_TABLE();
};

} // namespace symcirc
