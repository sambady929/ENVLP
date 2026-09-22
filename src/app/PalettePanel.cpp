#include "PalettePanel.h"
#include "Symbols.h"

namespace symcirc {

using syms::Kind;

wxBEGIN_EVENT_TABLE(PalettePanel, wxPanel)
    EVT_LISTBOX(wxID_ANY, PalettePanel::on_tool_sel)
    EVT_LIST_ITEM_SELECTED(wxID_ANY, PalettePanel::on_kind_selected)
wxEND_EVENT_TABLE()

namespace {
struct CompEntry { const char* label; Kind kind; };
// Common parts are listed here with glyphs; the "I" instance menu on the
// canvas offers the same set (plus the rarer blocks) with shortcut hints.
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
    {"1/s block",    Kind::IS},
    {"s block",      Kind::SBLK},
};
constexpr int kCompCount = int(sizeof(kComps) / sizeof(kComps[0]));
} // namespace

PalettePanel::PalettePanel(wxWindow* parent, Document* doc)
    : wxPanel(parent), doc_(doc) {
    auto* root = new wxBoxSizer(wxVERTICAL);

    root->Add(new wxStaticText(this, wxID_ANY, "Tools"), 0, wxALL, 4);

    tools_ = new wxListBox(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 70),
                           wxArrayString());
    tools_->Append("Select / Move");
    tools_->Append("Wire (W)");
    tools_->Append("Delete");
    tools_->SetSelection(0);
    root->Add(tools_, 0, wxEXPAND | wxLEFT | wxRIGHT, 4);

    root->Add(new wxStaticText(this, wxID_ANY, "Components"), 0,
              wxALL | wxTOP, 8);

    glyphs_ = new wxImageList(34, 34, true);
    for (const auto& e : kComps) glyphs_->Add(symbol_swatch(e.kind, 34, 34));

    comps_ = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 260),
                            wxLC_ICON | wxLC_SINGLE_SEL | wxBORDER_SIMPLE);
    comps_->AssignImageList(glyphs_, wxIMAGE_LIST_NORMAL);
    for (int i = 0; i < kCompCount; ++i)
        comps_->InsertItem(i, kComps[i].label, i);
    root->Add(comps_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);

    auto* hint = new wxStaticText(
        this, wxID_ANY,
        "Keys: R C L V B M K G D T W\n"
        "M again = PMOS.  Space rotates,\n"
        "Shift/Space flips.  I = instance menu.");
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
    if (comps_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED) != -1)
        return Tool::Place;
    switch (tools_->GetSelection()) {
    case 1: return Tool::Wire;
    case 2: return Tool::Delete;
    default: return Tool::Select;
    }
}

Kind PalettePanel::place_kind() const {
    long sel = comps_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (sel < 0 || sel >= kCompCount) return Kind::R;
    return kComps[sel].kind;
}

void PalettePanel::set_active(Tool t, syms::Kind k) {
    if (t == Tool::Place) {
        int idx = comp_index_for(k);
        comps_->SetItemState(-1, 0, wxLIST_STATE_SELECTED);
        if (idx >= 0) {
            comps_->SetItemState(idx, wxLIST_STATE_SELECTED,
                                 wxLIST_STATE_SELECTED);
            comps_->EnsureVisible(idx);
        }
        tools_->SetSelection(wxNOT_FOUND);
    } else {
        comps_->SetItemState(-1, 0, wxLIST_STATE_SELECTED);
        tools_->SetSelection(t == Tool::Wire ? 1 : t == Tool::Delete ? 2 : 0);
    }
}

void PalettePanel::on_kind_selected(wxListEvent&) {
    tools_->SetSelection(wxNOT_FOUND);
    if (on_tool_changed) on_tool_changed();
}

void PalettePanel::on_tool_sel(wxCommandEvent& e) {
    if (e.GetSelection() != wxNOT_FOUND) {
        comps_->SetItemState(-1, 0, wxLIST_STATE_SELECTED);
    }
    if (on_tool_changed) on_tool_changed();
}

} // namespace symcirc
