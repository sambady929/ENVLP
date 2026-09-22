#include "PalettePanel.h"

namespace symcirc {

using syms::Kind;

wxBEGIN_EVENT_TABLE(PalettePanel, wxPanel)
    EVT_LISTBOX(wxID_ANY, PalettePanel::on_tool_sel)
wxEND_EVENT_TABLE()

namespace {
struct CompEntry { const char* label; Kind kind; };
const CompEntry kComps[] = {
    {"Resistor",     Kind::R},
    {"Capacitor",    Kind::C},
    {"Inductor",     Kind::L},
    {"V source",     Kind::V},
    {"I source",     Kind::I},
    {"VCVS (E)",     Kind::E},
    {"VCCS (G)",     Kind::G},
    {"N-MOSFET",     Kind::NMOS},
    {"P-MOSFET",     Kind::PMOS},
    {"NPN BJT",      Kind::NPN},
    {"PNP BJT",      Kind::PNP},
    {"Ground",       Kind::GND},
};
constexpr int kToolCount = 3; // Select / Wire / Delete rows
} // namespace

PalettePanel::PalettePanel(wxWindow* parent, Document* doc)
    : wxPanel(parent), doc_(doc) {
    auto* root = new wxBoxSizer(wxVERTICAL);

    root->Add(new wxStaticText(this, wxID_ANY, "Tools"), 0, wxALL, 4);

    tools_ = new wxListBox(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                           wxArrayString());
    tools_->Append("Select / Move");
    tools_->Append("Wire");
    tools_->Append("Delete");
    tools_->SetSelection(0);
    root->Add(tools_, 0, wxEXPAND | wxLEFT | wxRIGHT, 4);

    root->Add(new wxStaticText(this, wxID_ANY, "Components"), 0,
              wxALL | wxTOP, 8);

    wxArrayString names;
    for (const auto& e : kComps) names.Add(e.label);
    comps_ = new wxListBox(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                           names);
    root->Add(comps_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);

    auto* hint = new wxStaticText(
        this, wxID_ANY,
        "Click a component,\nthen click the canvas.\nR = rotate, Del = delete");
    hint->SetForegroundColour(wxColour(110, 110, 115));
    root->Add(hint, 0, wxALL, 6);

    SetSizerAndFit(root);
}

Tool PalettePanel::tool() const {
    // a selected component means "place mode"
    if (comps_->GetSelection() != wxNOT_FOUND) return Tool::Place;
    switch (tools_->GetSelection()) {
    case 1: return Tool::Wire;
    case 2: return Tool::Delete;
    default: return Tool::Select;
    }
}

Kind PalettePanel::place_kind() const {
    int sel = comps_->GetSelection();
    if (sel < 0 || sel >= int(sizeof(kComps) / sizeof(kComps[0])))
        return Kind::R;
    return kComps[sel].kind;
}

void PalettePanel::on_tool_sel(wxCommandEvent& e) {
    if (e.GetEventObject() == comps_) {
        // picking a component switches to Place mode
        tools_->SetSelection(wxNOT_FOUND);
    } else {
        // picking a tool clears the component choice
        if (e.GetSelection() >= 0) comps_->SetSelection(wxNOT_FOUND);
    }
    if (on_tool_changed) on_tool_changed();
}

void PalettePanel::on_comp_sel(wxCommandEvent& e) {
    on_tool_sel(e);
}

} // namespace symcirc
