#include "PalettePanel.h"
#include "Symbols.h"
#include "Theme.h"

#include <wx/dcbuffer.h>

namespace symcirc {

using syms::Kind;

namespace {
struct CompEntry { const char* label; Kind kind; };
// Compact labels (the reference abbreviates to fit a narrow tile: "Cap",
// "Res", "NPN", ...).
const CompEntry kComps[] = {
    {"Res",      Kind::R},
    {"Cap",      Kind::C},
    {"Ind",      Kind::L},
    {"V Src",    Kind::V},
    {"I Src",    Kind::I},
    {"GND",      Kind::GND},
    {"VDD",      Kind::VDD},
    {"Diode",    Kind::D},
    {"NMOS",     Kind::NMOS},
    {"PMOS",     Kind::PMOS},
    {"NPN",      Kind::NPN},
    {"PNP",      Kind::PNP},
    {"Xfmr",     Kind::T},
    {"K",        Kind::K},
    {"OpAmp",    Kind::OPAMP},
    {"FD Amp",   Kind::FDOPAMP},
    {"Amp",      Kind::AMP},
    {"Nullor",   Kind::NULLOR},
    {"VCVS",     Kind::E},
    {"VCCS",     Kind::G},
    {"CCVS",     Kind::CCVS},
    {"CCCS",     Kind::CCCS},
    {"1/s",      Kind::IS},
    {"s",        Kind::SBLK},
};
} // namespace

PalettePanel::PalettePanel(wxWindow* parent, Document* doc)
    : wxScrolledWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                       wxVSCROLL),
      doc_(doc) {
    for (const auto& e : kComps) tiles_.push_back({e.label, e.kind});
    // Render each tile's artwork ONCE; the paint handler then just blits them.
    // (Redrawing every symbol on each repaint -- including on every hover
    // motion -- was needless work.)
    for (const auto& t : tiles_)
        swatches_.push_back(symbol_swatch(t.kind, 40, 30));
    SetBackgroundColour(theme::surface);
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetScrollRate(0, 8);
    Bind(wxEVT_PAINT, &PalettePanel::on_paint, this);
    Bind(wxEVT_LEFT_DOWN, &PalettePanel::on_left_down, this);
    Bind(wxEVT_MOTION, &PalettePanel::on_motion, this);
    Bind(wxEVT_LEAVE_WINDOW, &PalettePanel::on_leave, this);
    Bind(wxEVT_SIZE, &PalettePanel::on_size, this);
}

// Height needed for all tiles at the current width (drives the scroll range).
static int palette_content_height(int tile_count, int wrap_w) {
    int per_row = std::max(1, (wrap_w - 2 * 3) / (60 + 3));
    int rows = (tile_count + per_row - 1) / per_row;
    return 3 + rows * (58 + 3);
}

int PalettePanel::wrap_width() const {
    wxSize cs = GetClientSize();
    return std::max(kTileW + 2 * kPad, cs.x);
}

wxRect PalettePanel::tile_rect(int i) const {
    int per_row = std::max(1, (wrap_width() - 2 * kPad) / (kTileW + kPad));
    int col = i % per_row, row = i / per_row;
    return wxRect(kPad + col * (kTileW + kPad), kPad + row * (kTileH + kPad),
                  kTileW, kTileH);
}

int PalettePanel::index_at(const wxPoint& p) const {
    // Convert from scrolled client coords to the unscrolled tile space.
    int sx, sy;
    const_cast<PalettePanel*>(this)->CalcUnscrolledPosition(p.x, p.y, &sx, &sy);
    wxPoint q(sx, sy);
    for (int i = 0; i < int(tiles_.size()); ++i)
        if (tile_rect(i).Contains(q)) return i;
    return -1;
}

int PalettePanel::comp_index_for(Kind k) const {
    for (int i = 0; i < int(tiles_.size()); ++i)
        if (tiles_[i].kind == k) return i;
    return -1;
}

Tool PalettePanel::tool() const { return Tool::Place; }

Kind PalettePanel::place_kind() const {
    if (active_idx_ >= 0 && active_idx_ < int(tiles_.size()))
        return tiles_[active_idx_].kind;
    return Kind::R;
}

void PalettePanel::set_active(Tool t, Kind k) {
    int want = (t == Tool::Place) ? comp_index_for(k) : -1;
    if (want == active_idx_) return;
    active_idx_ = want;
    Refresh(false);
}

void PalettePanel::on_size(wxSizeEvent& e) {
    // A resize can change the wrap, so the virtual height changes too.
    SetVirtualSize(wrap_width(),
                   palette_content_height(int(tiles_.size()), wrap_width()));
    Refresh(false);
    e.Skip();
}

void PalettePanel::on_paint(wxPaintEvent&) {
    wxAutoBufferedPaintDC dc(this);
    DoPrepareDC(dc);
    dc.SetBackground(wxBrush(theme::surface));
    dc.Clear();

    wxFont base = GetFont();
    wxFont label_font = base;
    label_font.SetPointSize(std::max(7, base.GetPointSize() - 1));

    for (int i = 0; i < int(tiles_.size()); ++i) {
        wxRect r = tile_rect(i);
        bool sel = (i == active_idx_);
        bool hov = (i == hover_idx_);
        // Card: muted surface, 1px border; hover lifts to white; active uses
        // the accent border + soft accent fill.
        dc.SetBrush(wxBrush(sel ? theme::accent_soft
                                : hov ? theme::surface : theme::surface_muted));
        dc.SetPen(wxPen(sel ? theme::accent
                            : hov ? theme::border_strong : theme::border));
        dc.DrawRoundedRectangle(r, 4);

        // Artwork: blit the cached swatch centred in the upper part.
        dc.DrawBitmap(swatches_[i], r.x + (r.width - 40) / 2, r.y + 4, true);

        // Label under the artwork.
        dc.SetFont(label_font);
        dc.SetTextForeground(sel ? theme::accent : theme::text);
        wxString lb = wxString::FromUTF8(tiles_[i].label);
        wxSize ts = dc.GetTextExtent(lb);
        dc.DrawText(lb, r.x + (r.width - ts.x) / 2,
                    r.y + r.height - ts.y - 4);
    }
}

void PalettePanel::on_left_down(wxMouseEvent& e) {
    int i = index_at(e.GetPosition());
    if (i < 0) return;
    active_idx_ = i;
    Refresh(false);
    if (on_tool_changed) on_tool_changed();
}

void PalettePanel::on_motion(wxMouseEvent& e) {
    int i = index_at(e.GetPosition());
    if (i != hover_idx_) {
        hover_idx_ = i;
        Refresh(false);
    }
}

void PalettePanel::on_leave(wxMouseEvent&) {
    if (hover_idx_ != -1) {
        hover_idx_ = -1;
        Refresh(false);
    }
}

} // namespace symcirc
