#pragma once
#include "Document.h"
#include "SchematicCanvas.h" // Tool

#include <wx/wx.h>
#include <wx/imaglist.h>
#include <wx/listctrl.h>

#include <functional>
#include <vector>

namespace symcirc {

// Left-side component palette: a grid of component glyphs. The active tool
// (Select / Wire / Delete) is owned by the canvas and the toolbar; clicking a
// glyph enters placement mode for that component.
class PalettePanel : public wxPanel {
public:
    explicit PalettePanel(wxWindow* parent, Document* doc);

    std::function<void()> on_tool_changed; // canvas reads tool()/kind()

    Tool tool() const;
    syms::Kind place_kind() const;

    // Reflect the active tool in the palette (e.g. after a keyboard shortcut).
    void set_active(Tool t, syms::Kind k);

private:
    Document* doc_;
    wxImageList* glyphs_;
    wxListCtrl* comps_;
    bool updating_ = false;

    int comp_index_for(syms::Kind k) const;

    void on_kind_selected(wxListEvent& e);
    wxDECLARE_EVENT_TABLE();
};

} // namespace symcirc
