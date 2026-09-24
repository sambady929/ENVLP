#include "PropertiesPanel.h"
#include "core/Eng.h"

#include <cmath>
#include <cstdlib>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/combobox.h>
#include <wx/spinctrl.h>

namespace symcirc {

using syms::Kind;

wxBEGIN_EVENT_TABLE(PropertiesPanel, wxScrolledWindow)
wxEND_EVENT_TABLE()

namespace {
// 10 dB mantissa steps; combined with the exponent (steps of 3) these give
// the engineer's E-series-style orders of magnitude.
const wxArrayString kMantissas = [] {
    wxArrayString a;
    for (const char* s : {"1", "3.3", "10", "33", "100", "330", "1000"})
        a.Add(s);
    return a;
}();
// Exponent dropdown entries. Each row shows the power-of-ten *and* the SI
// prefix letter it maps to (e.g. "-3" -> "m" for milli). The numeric part is
// what we store / parse; the suffix is a hint, so the dropdown doubles as a
// cheat-sheet for which letter to expect on the schematic.
const wxArrayString kExponents = [] {
    wxArrayString a;
    struct E { int p; const char* suf; };
    static const E table[] = {
        { 18, "E"}, { 15, "P"}, { 12, "T"}, { 9, "G"}, { 6, "M"},
        {  3, "k"}, {  0, ""  }, {-3, "m"}, {-6, "u"}, {-9, "n"},
        {-12, "p"}, {-15, "f"}, {-18, "a"}, {-21, "z"},
    };
    for (const auto& e : table) {
        if (e.suf[0])
            a.Add(wxString::Format("%d (%s)", e.p, e.suf));
        else
            a.Add(wxString::Format("%d", e.p));
    }
    return a;
}();

// SI prefix letter for a power-of-ten exponent, or '\0' when none matches.
char si_letter_for_exp(int e) {
    switch (e) {
        case  18: return 'E';
        case  15: return 'P';
        case  12: return 'T';
        case   9: return 'G';
        case   6: return 'M';
        case   3: return 'k';
        case   0: return '\0';
        case  -3: return 'm';
        case  -6: return 'u';
        case  -9: return 'n';
        case -12: return 'p';
        case -15: return 'f';
        case -18: return 'a';
        case -21: return 'z';
    }
    return '\0';
}

void decompose(double v, double& mant, int& exp) {
    if (!(v > 0.0) || !std::isfinite(v)) {
        mant = 1.0;
        exp = 0;
        return;
    }
    exp = 3 * int(std::floor(std::log10(v) / 3.0));
    // clamp to the dropdown's range (-21 .. +18 in steps of 3)
    if (exp > 18) exp = 18;
    if (exp < -21) exp = -21;
    mant = v / std::pow(10.0, exp);
    // snap to the nearest preset mantissa (also keeps values tidy)
    static const double ms[] = {1, 3.3, 10, 33, 100, 330, 1000};
    double best = ms[0];
    double bestd = 1e300;
    for (double m : ms) {
        double d = std::fabs(std::log(mant / m));
        if (d < bestd) { bestd = d; best = m; }
    }
    mant = best;
    if (mant >= 1000.0) { mant = 1.0; exp += 3; }
}

wxString fmt_num(double m) {
    wxString s = wxString::Format("%.4g", m);
    return s;
}

// The exponent combo's current text: the plain exponent, with the SI letter
// in parentheses when one exists ("-3 (m)"). parse_exp_string reads either.
wxString exp_display(int exp) {
    char l = si_letter_for_exp(exp);
    if (l) return wxString::Format("%d (%c)", exp, l);
    return wxString::Format("%d", exp);
}

// Compose the value_text written back into the schematic: the bare mantissa,
// then the SI letter for the chosen exponent. e.g. mant=10 exp=-9 -> "10n".
// exp==0 has no suffix, so it reads back as the bare number (no unit).
wxString fmt_value(double mant, int exp) {
    if (exp == 0) return fmt_num(mant);
    char l = si_letter_for_exp(exp);
    if (l) return fmt_num(mant) + wxString(l);
    return fmt_num(mant) + "e" + wxString::Format("%d", exp);
}

// Parse the leading integer out of an exponent combo string ("-9" or
// "-9 (n)"). Returns true on success; out holds the exponent value.
bool parse_exp_string(const wxString& s, long& out) {
    wxString t = s;
    t = t.Trim(true).Trim(false);
    long sign = 1;
    if (!t.IsEmpty() && t[0] == '-') { sign = -1; t = t.Mid(1); }
    else if (!t.IsEmpty() && t[0] == '+') { t = t.Mid(1); }
    long v = 0;
    bool any = false;
    for (size_t i = 0; i < t.Length(); ++i) {
        wxChar ch = t[i];
        if (ch >= '0' && ch <= '9') { v = v * 10 + (ch - '0'); any = true; }
        else break;
    }
    if (!any) return false;
    out = sign * v;
    return true;
}
} // namespace

PropertiesPanel::PropertiesPanel(wxWindow* parent)
    : wxScrolledWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                       wxVSCROLL) {
    SetScrollRate(0, 10);
    SetBackgroundColour(wxColour(245, 245, 243));
}

void PropertiesPanel::add_header(const wxString& text) {
    auto* sizer = GetSizer();
    auto* h = new wxStaticText(this, wxID_ANY, text);
    wxFont f = h->GetFont();
    f.SetWeight(wxFONTWEIGHT_BOLD);
    h->SetFont(f);
    sizer->Add(h, 0, wxALL | wxTOP, 8);
}

wxTextCtrl* PropertiesPanel::add_text(const wxString& label,
                                      const wxString& value,
                                      std::function<void(const wxString&)> on_change) {
    auto* sizer = GetSizer();
    sizer->Add(new wxStaticText(this, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL,
               4);
    auto* tc = new wxTextCtrl(this, wxID_ANY, value);
    sizer->Add(tc, 1, wxEXPAND | wxRIGHT, 6);
    tc->Bind(wxEVT_TEXT, [this, on_change](wxCommandEvent& e) {
        if (rebuilding_) return;
        on_change(e.GetString());
        if (on_edited) on_edited();
    });
    return tc;
}

wxSpinCtrl* PropertiesPanel::add_spin(const wxString& label, int value, int min,
                                      int max,
                                      std::function<void(int)> on_change) {
    auto* sizer = GetSizer();
    sizer->Add(new wxStaticText(this, wxID_ANY, label), 0,
               wxALIGN_CENTER_VERTICAL, 4);
    auto* sc = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition,
                              wxDefaultSize, wxSP_ARROW_KEYS, min, max, value);
    sizer->Add(sc, 1, wxEXPAND | wxRIGHT, 6);
    sc->Bind(wxEVT_SPINCTRL, [this, on_change](wxSpinEvent& e) {
        if (rebuilding_) return;
        on_change(e.GetPosition());
        if (on_edited) on_edited();
    });
    return sc;
}

// mantissa + exponent dropdowns; stores "<m>e<exp>" into comp->param_text
void PropertiesPanel::add_mantissa_exp(syms::Component* comp,
                                       const std::string& name, bool parasitic,
                                       const wxString& default_text) {
    auto* sizer = GetSizer();

    double v = 0.0;
    wxString cur = default_text;
    if (comp->param_text.count(name) && !comp->param_text.at(name).empty())
        cur = wxString::FromUTF8(comp->param_text.at(name));
    if (!syms::eng::parse_value(cur.ToStdString(), v))
        syms::eng::parse_value(default_text.ToStdString(), v);
    double mant;
    int exp;
    decompose(v, mant, exp);

    auto* sub = new wxBoxSizer(wxHORIZONTAL);

    wxComboBox* man = nullptr;
    wxComboBox* ex = nullptr;
    if (parasitic) {
        auto* cb = new wxCheckBox(this, wxID_ANY,
                                  wxString::FromUTF8(name));
        cb->SetValue(comp->param_enabled(name));
        sizer->Add(cb, 0, wxLEFT | wxTOP, 6);
        man = new wxComboBox(this, wxID_ANY, fmt_num(mant), wxDefaultPosition,
                             wxSize(70, -1), kMantissas, wxCB_DROPDOWN);
        ex = new wxComboBox(this, wxID_ANY, exp_display(exp),
                            wxDefaultPosition, wxSize(60, -1), kExponents,
                            wxCB_DROPDOWN);
        man->Enable(cb->GetValue());
        ex->Enable(cb->GetValue());
        cb->Bind(wxEVT_CHECKBOX, [this, comp, name, man, ex](wxCommandEvent& e) {
            comp->param_on[name] = e.IsChecked();
            man->Enable(e.IsChecked());
            ex->Enable(e.IsChecked());
            doc_->dirty = true;
            if (on_edited) on_edited();
        });
    } else {
        auto* lbl = new wxStaticText(this, wxID_ANY,
                                     wxString::FromUTF8(name) + " -- always on");
        lbl->SetForegroundColour(wxColour(80, 80, 85));
        sizer->Add(lbl, 0, wxLEFT | wxTOP, 6);
        man = new wxComboBox(this, wxID_ANY, fmt_num(mant), wxDefaultPosition,
                             wxSize(70, -1), kMantissas, wxCB_DROPDOWN);
        ex = new wxComboBox(this, wxID_ANY, exp_display(exp),
                            wxDefaultPosition, wxSize(60, -1), kExponents,
                            wxCB_DROPDOWN);
    }
    sub->Add(man, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 2);
    sub->Add(new wxStaticText(this, wxID_ANY, "e"), 0,
             wxALIGN_CENTER_VERTICAL | wxRIGHT, 2);
    sub->Add(ex, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    sub->Add(new wxStaticText(this, wxID_ANY, wxString::FromUTF8("[" + name + "]")),
             0, wxALIGN_CENTER_VERTICAL);
    sizer->Add(sub, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);

    auto commit = [this, comp, name, man, ex] {
        double m = 0.0;
        if (!syms::eng::parse_value(man->GetValue().ToStdString(), m)) m = 1.0;
        long e = 0;
        if (!parse_exp_string(ex->GetValue(), e)) e = 0;
        comp->param_text[name] = fmt_value(m, int(e)).ToStdString();
        doc_->dirty = true;
        if (on_edited) on_edited();
    };
    man->Bind(wxEVT_COMBOBOX, [commit, this](wxCommandEvent&) {
        if (!rebuilding_) commit();
    });
    man->Bind(wxEVT_TEXT, [commit, this](wxCommandEvent&) {
        if (!rebuilding_) commit();
    });
    ex->Bind(wxEVT_COMBOBOX, [commit, this](wxCommandEvent&) {
        if (!rebuilding_) commit();
    });
    ex->Bind(wxEVT_TEXT, [commit, this](wxCommandEvent&) {
        if (!rebuilding_) commit();
    });
}

void PropertiesPanel::add_param_row(syms::Component* comp,
                                    const std::string& name,
                                    const std::string& unit, bool parasitic,
                                    const std::string& default_text) {
    (void)unit;
    add_mantissa_exp(comp, name, parasitic, wxString::FromUTF8(default_text));
}

// Value dropdowns for passives: pick a mantissa (1, 3.3, 10, 33, ...) and an
// exponent (multiples of 3) -> value_text = "<mant><SIPrefix>".
void PropertiesPanel::add_value_selector(syms::Component* comp, bool with_unit) {
    auto* sizer = GetSizer();
    double v = 1.0;
    if (!syms::eng::parse_value(comp->value_text, v) || !(v > 0.0)) v = 1.0;
    double mant = 1.0;
    int exp = 0;
    decompose(v, mant, exp);

    sizer->Add(new wxStaticText(this, wxID_ANY, "Value"), 0,
               wxALIGN_CENTER_VERTICAL | wxLEFT | wxTOP, 4);
    auto* sub = new wxBoxSizer(wxHORIZONTAL);
    auto* man = new wxComboBox(this, wxID_ANY, fmt_num(mant), wxDefaultPosition,
                               wxSize(70, -1), kMantissas, wxCB_DROPDOWN);
    auto* ex = new wxComboBox(this, wxID_ANY, exp_display(exp),
                              wxDefaultPosition, wxSize(60, -1), kExponents,
                              wxCB_DROPDOWN);
    sub->Add(man, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 2);
    sub->Add(new wxStaticText(this, wxID_ANY, "e"), 0,
             wxALIGN_CENTER_VERTICAL | wxRIGHT, 2);
    sub->Add(ex, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    if (with_unit) {
        std::string unit = syms::kind_display(comp->kind);
        sub->Add(new wxStaticText(this, wxID_ANY, wxString::FromUTF8(unit)), 0,
                 wxALIGN_CENTER_VERTICAL);
    }
    sizer->Add(sub, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);

    auto commit = [this, comp, man, ex] {
        double m = 1.0;
        if (!syms::eng::parse_value(man->GetValue().ToStdString(), m)) m = 1.0;
        long e = 0;
        if (!parse_exp_string(ex->GetValue(), e)) e = 0;
        comp->value_text = fmt_value(m, int(e)).ToStdString();
        doc_->dirty = true;
        if (on_edited) on_edited();
    };
    man->Bind(wxEVT_COMBOBOX, [commit, this](wxCommandEvent&) {
        if (!rebuilding_) commit();
    });
    man->Bind(wxEVT_TEXT, [commit, this](wxCommandEvent&) {
        if (!rebuilding_) commit();
    });
    ex->Bind(wxEVT_COMBOBOX, [commit, this](wxCommandEvent&) {
        if (!rebuilding_) commit();
    });
    ex->Bind(wxEVT_TEXT, [commit, this](wxCommandEvent&) {
        if (!rebuilding_) commit();
    });
}

// Change a component's reference and rename every keyed map (placements) and
// any inductor-coupling links that refer to it.
bool PropertiesPanel::rename_component(const std::string& old_ref,
                                       const wxString& new_ref) {
    std::string nr = new_ref.ToStdString();
    if (nr == old_ref) return true;
    if (nr.empty()) return false;
    if (doc_->circuit.find(nr)) return false; // duplicate
    syms::Component* comp = nullptr;
    for (auto& cc : doc_->circuit.comps)
        if (cc.ref == old_ref) comp = &cc;
    if (!comp) return false;
    comp->ref = nr;
    auto pl = doc_->placements.find(old_ref);
    if (pl != doc_->placements.end()) {
        Placement p = pl->second;
        doc_->placements.erase(pl);
        doc_->placements[nr] = p;
    }
    for (auto& cc : doc_->circuit.comps)
        for (auto& lk : cc.links)
            if (lk == old_ref) lk = nr;
    doc_->dirty = true;
    return true;
}

void PropertiesPanel::refresh(Document* doc, const std::string& selection) {
    rebuilding_ = true;
    doc_ = doc;
    sel_ = selection;

    // wipe previous rows (Clear(true) destroys the child windows too)
    if (auto* old = GetSizer()) old->Clear(true);
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    SetSizer(sizer, true);

    // ---------------- selection-specific rows ----------------
    if (doc_ && !sel_.empty()) {
        if (sel_[0] == '#') {
            if (sel_.rfind("#label", 0) == 0) {
                int i = std::atoi(sel_.c_str() + 6);
                if (i >= 0 && i < int(doc_->labels.size())) {
                    add_header("Net label");
                    add_text("Name", wxString::FromUTF8(doc_->labels[i].name),
                             [this, i](const wxString& v) {
                                 if (i < int(doc_->labels.size()))
                                     doc_->labels[i].name = v.ToStdString();
                             });
                    // font size (#7)
                    auto* row = new wxBoxSizer(wxHORIZONTAL);
                    row->Add(new wxStaticText(this, wxID_ANY, "Font size"), 0,
                             wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
                    auto* sc = new wxSpinCtrl(this, wxID_ANY, wxEmptyString,
                                              wxDefaultPosition, wxDefaultSize,
                                              wxSP_ARROW_KEYS, 6, 96,
                                              doc_->labels[i].font_size);
                    row->Add(sc, 1);
                    sizer->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);
                    sc->Bind(wxEVT_SPINCTRL, [this, i, sc](wxSpinEvent&) {
                        if (rebuilding_) return;
                        if (on_label_font) on_label_font(i, sc->GetValue());
                    });
                }
            } else if (sel_.rfind("#wire", 0) == 0) {
                int colon = int(sel_.find(':'));
                int i = std::atoi(colon < 0 ? sel_.c_str() + 5
                                            : sel_.substr(5, colon - 5).c_str());
                if (i >= 0 && i < int(doc_->wires.size())) {
                    bool whole = colon < 0;
                    int seg = colon < 0 ? -1 : std::atoi(sel_.c_str() + colon + 1);
                    add_header(whole ? "Wire" : "Wire segment");
                    // Net name is read-only: wires are SPICE nodes and get a
                    // default name (n1, n2, ...); the only way to override is
                    // to place a net label on the wire (N key). Clicking the
                    // resulting label selects *it* for editing.
                    std::string net = doc_->net_name_of_wire(i);
                    {
                        auto* sizer2 = GetSizer();
                        sizer2->Add(new wxStaticText(this, wxID_ANY, "Net name"),
                                    0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxTOP, 4);
                        auto* nm = new wxTextCtrl(this, wxID_ANY,
                                                  wxString::FromUTF8(net));
                        nm->SetEditable(false);
                        nm->SetBackgroundColour(wxColour(235, 235, 232));
                        sizer2->Add(nm, 1, wxEXPAND | wxRIGHT, 6);
                    }
                    auto* hint = new wxStaticText(
                        this, wxID_ANY,
                        "Auto-assigned node name. Place a net label (N) on the "
                        "wire to override it.");
                    hint->SetForegroundColour(wxColour(115, 115, 120));
                    hint->Wrap(220);
                    sizer->Add(hint, 0, wxALL, 4);
                    auto* info = new wxStaticText(
                        this, wxID_ANY,
                        wxString::Format("%d point(s)",
                                         int(doc_->wires[i].pts.size())));
                    sizer->Add(info, 0, wxALL, 4);
                    auto* rm = new wxButton(
                        this, wxID_ANY, whole ? "Delete wire" : "Delete segment");
                    sizer->Add(rm, 0, wxALL, 4);
                    rm->Bind(wxEVT_BUTTON, [this, i, whole, seg](wxCommandEvent&) {
                        if (i < 0 || i >= int(doc_->wires.size())) return;
                        if (!whole) {
                            (void)seg; // segment deletion handled on the canvas
                        }
                        doc_->wires.erase(doc_->wires.begin() + i);
                        doc_->dirty = true;
                        if (on_edited) on_edited();
                    });
                }
            }
        } else if (const syms::Component* c = doc_->circuit.find(sel_)) {
            syms::Component* comp = nullptr;
            for (auto& cc : doc_->circuit.comps)
                if (cc.ref == sel_) comp = &cc;

            add_header(wxString::FromUTF8(syms::kind_display(c->kind) + "  " +
                                          c->ref));

            // editable instance name
            {
                auto* sizer = GetSizer();
                sizer->Add(new wxStaticText(this, wxID_ANY, "Name"), 0,
                           wxALIGN_CENTER_VERTICAL | wxLEFT | wxTOP, 4);
                auto* nm = new wxTextCtrl(this, wxID_ANY,
                                          wxString::FromUTF8(c->ref));
                sizer->Add(nm, 1, wxEXPAND | wxRIGHT, 6);
                nm->Bind(wxEVT_TEXT, [this, old = c->ref, nm](wxCommandEvent&) {
                    if (rebuilding_) return;
                    std::string want = nm->GetValue().ToStdString();
                    if (want == old || want.empty()) return;
                    std::string key = old;
                    if (rename_component(key, nm->GetValue())) {
                        sel_ = want;
                        doc_->dirty = true;
                        if (on_selection_changed) on_selection_changed(want);
                    }
                });
            }

            // value selector for passives and ideal sources
            if (c->kind == Kind::R || c->kind == Kind::C ||
                c->kind == Kind::L || c->kind == Kind::V ||
                c->kind == Kind::I || c->kind == Kind::D ||
                c->kind == Kind::OPAMP || c->kind == Kind::FDOPAMP ||
                c->kind == Kind::AMP || c->kind == Kind::E ||
                c->kind == Kind::G)
                add_value_selector(comp, true);

            // device model parameters: checkbox (parasitic) + mantissa/exponent
            for (const auto& pd : syms::param_defs(c->kind))
                add_param_row(comp, pd.name, pd.unit, pd.parasitic,
                              pd.default_text);

            if (c->kind == Kind::K) {
                auto* names = new wxArrayString();
                names->Add("(none)");
                for (const auto& cc : doc_->circuit.comps)
                    if (cc.kind == Kind::L) names->Add(wxString::FromUTF8(cc.ref));
                int nL = int(names->GetCount());

                auto add_link_row = [&](int which, const wxString& label) {
                    auto* row = new wxBoxSizer(wxHORIZONTAL);
                    row->Add(new wxStaticText(this, wxID_ANY, label), 0,
                             wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
                    wxString cur = c->links.size() > size_t(which)
                                       ? wxString::FromUTF8(c->links[which])
                                       : wxString("(none)");
                    auto* ch = new wxChoice(this, wxID_ANY, wxDefaultPosition,
                                            wxDefaultSize, *names);
                    int sel = names->Index(cur);
                    ch->SetSelection(sel == wxNOT_FOUND ? 0 : sel);
                    row->Add(ch, 1, wxEXPAND);
                    sizer->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);
                    ch->Bind(wxEVT_CHOICE, [this, comp, which, ch](wxCommandEvent&) {
                        wxString v = ch->GetString(ch->GetSelection());
                        if (comp->links.size() < 2) comp->links.resize(2);
                        comp->links[which] =
                            (v == "(none)") ? std::string() : v.ToStdString();
                        doc_->dirty = true;
                        if (on_edited) on_edited();
                    });
                };
                add_link_row(0, "Couples L:");
                add_link_row(1, "with L:");
                (void)nL;

                auto* info = new wxStaticText(
                    this, wxID_ANY,
                    "Coupling coefficient K (value above).\n"
                    "Pick the two inductors this marker couples.");
                info->SetForegroundColour(wxColour(115, 115, 120));
                sizer->Add(info, 0, wxALL, 6);
            }

            auto* info = new wxStaticText(
                this, wxID_ANY, wxString::FromUTF8(syms::kind_display(c->kind)));
            info->SetForegroundColour(wxColour(115, 115, 120));
            sizer->Add(info, 0, wxALL | wxTOP, 8);
        }
    } else if (doc_) {
        auto* t = new wxStaticText(
            this, wxID_ANY,
            "Nothing selected.\n\nPick a component with the Select\ntool to edit "
            "its value, parasitics\nand size offsets.");
        t->SetForegroundColour(wxColour(115, 115, 120));
        sizer->Add(t, 0, wxALL, 8);
    }

    sizer->AddSpacer(6);
    FitInside(); // size the virtual area to the content so it scrolls (#5)
    rebuilding_ = false;
}

void PropertiesPanel::on_analysis_changed(wxCommandEvent&) {}

} // namespace symcirc
