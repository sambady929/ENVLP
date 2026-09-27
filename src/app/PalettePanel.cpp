#include "PalettePanel.h"
#include "Symbols.h"

namespace symcirc {

using syms::Kind;

wxBEGIN_EVENT_TABLE(PalettePanel, wxPanel)
    EVT_LIST_ITEM_SELECTED(wxID_ANY, PalettePanel::on_kind_selected)
wxEND_EVENT_TABLE()

namespace {
struct CompEntry { const char* label; Kind kind; };
const CompEntry kComps[] = {
    {"Resistor",     Kind::R},
    {"Capacitor",    Kind::C},
    {"Inductor",     Kind::L},
    {"Voltage src",  Kind::V},
    {"Current src",  Kind::I},
    {"Ground",       Kind::GND},
    {"VDD",          Kind::VDD},
    {"Diode",        Kind::D},
    {"N-MOSFET",     Kind::NMOS},
    {"P-MOSFET",     Kind::PMOS},
    {"NPN BJT",      Kind::NPN},
    {"PNP BJT",      Kind::PNP},
    {"Transformer",  Kind::T},
    {"Ind. coupling", Kind::K},
    {"Op-amp",       Kind::OPAMP},
    {"Fully diff op-amp", Kind::FDOPAMP},
    {"Amplifier",    Kind::AMP},
    {"Nullor",       Kind::NULLOR},
    {"VCVS (E)",     Kind::E},
    {"VCCS (G)",     Kind::G},
    {"CCVS (H)",     Kind::CCVS},
    {"CCCS (F)",     Kind::CCCS},
    {"1/s block",    Kind::IS},
    {"s block",      Kind::SBLK},
};
constexpr int kCompCount = int(sizeof(kComps) / sizeof(kComps[0]));
} // namespace

PalettePanel::PalettePanel(wxWindow* parent, Document* doc)
    : wxPanel(parent), doc_(doc) {
    auto* root = new wxBoxSizer(wxVERTICAL);

    auto* header = new wxStaticText(this, wxID_ANY, "Components");
    wxFont hf = header->GetFont();
    hf.SetWeight(wxFONTWEIGHT_BOLD);
    header->SetFont(hf);
    root->Add(header, 0, wxALL, 4);

    glyphs_ = new wxImageList(34, 34, true);
    for (const auto& e : kComps) glyphs_->Add(symbol_swatch(e.kind, 34, 34));

    // wxLC_ICON wraps into multiple columns, so the palette grows with the
    // available height instead of clipping a long single column.
    comps_ = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxSize(190, -1),
                            wxLC_ICON | wxLC_SINGLE_SEL | wxBORDER_SIMPLE);
    comps_->AssignImageList(glyphs_, wxIMAGE_LIST_NORMAL);
    for (int i = 0; i < kCompCount; ++i)
        comps_->InsertItem(i, kComps[i].label, i);
    root->Add(comps_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);

    auto* hint = new wxStaticText(
        this, wxID_ANY,
        "Place: click a glyph, then the\n"
        "canvas. Keys: R C L V B M D T W\n"
        "N net   I menu   F fit\n"
        "U undo   Shift+U redo   Del delete\n"
        "Space rotate, Shift+Space flip.");
    hint->SetForegroundColour(wxColour(110, 110, 115));
    root->Add(hint, 0, wxALL, 6);

    SetSizerAndFit(root);
}

int PalettePanel::comp_index_for(syms::Kind k) const {
    for (int i = 0; i < kCompCount; ++i)
        if (kComps[i].kind == k) return i;
    return -1;
}

Tool PalettePanel::tool() const {
    // The palette only ever starts component placement; Select / Wire / Delete
    // are set on the canvas directly (toolbar or keys).
    return Tool::Place;
}

Kind PalettePanel::place_kind() const {
    long sel = comps_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (sel < 0 || sel >= kCompCount) return Kind::R;
    return kComps[sel].kind;
}

void PalettePanel::set_active(Tool t, syms::Kind k) {
    // Idempotent: only touch the list when the active tile actually changes.
    // Otherwise every placement calls EnsureVisible (which can scroll the
    // palette) and repaints the list -- a visible hitch while placing parts.
    int want = (t == Tool::Place) ? comp_index_for(k) : -1;
    if (want == active_idx_) return;
    active_idx_ = want;
    updating_ = true;
    comps_->SetItemState(-1, 0, wxLIST_STATE_SELECTED);
    if (want >= 0) {
        comps_->SetItemState(want, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
    }
    updating_ = false;
}

void PalettePanel::on_kind_selected(wxListEvent&) {
    if (updating_) return; // programmatic selection, not a user click
    if (on_tool_changed) on_tool_changed();
}

} // namespace symcirc
