#pragma once
#include "Document.h"
#include "SchematicCanvas.h" // Tool

#include <wx/wx.h>
#include <wx/imaglist.h>
#include <wx/listctrl.h>

namespace symcirc {

// Left-side tool/component palette: component glyphs plus Select/Wire/Delete.
class PalettePanel : public wxPanel {
public:
    explicit PalettePanel(wxWindow* parent, Document* doc);

    std::function<void()> on_tool_changed; // canvas reads tool()/kind()

    Tool tool() const;
    syms::Kind place_kind() const;

    // Reflect the active tool in the lists (e.g. after a keyboard shortcut).
    void set_active(Tool t, syms::Kind k);

private:
    Document* doc_;
    wxListBox* tools_;
    wxImageList* glyphs_;
    wxListCtrl* comps_;

    int comp_index_for(syms::Kind k) const;

    void on_kind_selected(wxListEvent& e);
    void on_tool_sel(wxCommandEvent& e);
    wxDECLARE_EVENT_TABLE();
};

} // namespace symcirc
