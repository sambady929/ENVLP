#pragma once
#include "Document.h"
#include "SchematicCanvas.h" // Tool

#include <wx/wx.h>
#include <wx/scrolwin.h>

#include <functional>
#include <string>
#include <vector>

namespace symcirc {

// Left-side component palette, styled after the analog-canvas shapes panel: a
// wrapping grid of compact tiles (artwork + abbreviated label). The active tool
// (Select / Wire / Delete) is owned by the canvas and the toolbar; clicking a
// tile enters placement mode for that component.
class PalettePanel : public wxScrolledWindow {
public:
    explicit PalettePanel(wxWindow* parent, Document* doc);

    std::function<void()> on_tool_changed; // canvas reads tool()/kind()

    Tool tool() const;
    syms::Kind place_kind() const;

    // Reflect the active tool in the palette (e.g. after a keyboard shortcut).
    void set_active(Tool t, syms::Kind k);

private:
    struct Tile {
        const char* label;
        syms::Kind kind;
    };

    Document* doc_;
    std::vector<Tile> tiles_;
    std::vector<wxBitmap> swatches_; // cached artwork per tile (drawn once)
    int active_idx_ = -1;
    int hover_idx_ = -1;

    // tile metrics, scaled for the display DPI (see the constructor). Kept as
    // members rather than constants because on a high-DPI (e.g. 200%) display
    // raw pixels would draw everything at half the intended visual size.
    int kTileW = 60;
    int kTileH = 58;
    int kPad = 3;
    int kSwatchW = 40;
    int kSwatchH = 30;

    int index_at(const wxPoint& p) const;
    wxRect tile_rect(int i) const;
    int wrap_width() const;
    int comp_index_for(syms::Kind k) const;

    void on_paint(wxPaintEvent&);
    void on_left_down(wxMouseEvent&);
    void on_motion(wxMouseEvent&);
    void on_leave(wxMouseEvent&);
    void on_size(wxSizeEvent&);
};

} // namespace symcirc
