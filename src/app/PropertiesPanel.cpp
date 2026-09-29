#include "PropertiesPanel.h"
#include "Theme.h"
#include "core/Eng.h"

#include <chrono>
#include <cmath>
#include <cstdio>
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
    SetScrollRate(FromDIP(10), FromDIP(10));
    SetBackgroundColour(theme::chrome_bg);
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

namespace {
// A small bordered "card" panel (title + one row of controls), used to build
// the tiled parameter grid in the properties pane.
wxPanel* new_card(wxWindow* parent, const wxString& title) {
    auto* p = new wxPanel(parent);
    p->SetBackgroundColour(*wxWHITE);
    auto* s = new wxBoxSizer(wxVERTICAL);
    auto* t = new wxStaticText(p, wxID_ANY, title);
    wxFont f = t->GetFont();
    f.SetPointSize(std::max(7, f.GetPointSize() - 1));
    f.SetWeight(wxFONTWEIGHT_BOLD);
    t->SetFont(f);
    t->SetForegroundColour(wxColour(70, 78, 92));
    s->Add(t, 0, wxLEFT | wxRIGHT | wxTOP, parent->FromDIP(4));
    p->SetSizer(s);
    return p;
}
} // namespace

// The wrapping container the parameter cards tile into. Created once per
// refresh, right after the header, so device parameters read as a compact
// grid rather than a single vertical list.
wxWrapSizer* PropertiesPanel::cards_host() {
    if (cards_) return cards_;
    auto* sizer = GetSizer();
    cards_ = new wxWrapSizer(wxHORIZONTAL);
    sizer->Add(cards_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);
    return cards_;
}

// One free-text field per parameter: type any engineering value ("10k",
// "2.5p", "1e-13"). A single editable field -- rather than a pair of
// mantissa/exponent drop-downs -- keeps the panel snappy: creating dozens of
// wxComboBox controls on every selection cost ~280 ms and was the selection
// lag. The field is committed as typed.
void PropertiesPanel::add_mantissa_exp(syms::Component* comp,
                                       const std::string& name, bool parasitic,
                                       const wxString& default_text) {
    auto* host = cards_host();
    auto* card = new_card(this, wxString::FromUTF8(name));

    wxString cur = default_text;
    if (comp->param_text.count(name) && !comp->param_text.at(name).empty())
        cur = wxString::FromUTF8(comp->param_text.at(name));

    auto* row = new wxBoxSizer(wxHORIZONTAL);
    wxCheckBox* cb = nullptr;
    if (parasitic) {
        cb = new wxCheckBox(card, wxID_ANY, wxEmptyString);
        cb->SetValue(comp->param_enabled(name));
        row->Add(cb, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(2));
    }
    auto* tc = new wxTextCtrl(card, wxID_ANY, cur, wxDefaultPosition,
                              wxSize(FromDIP(72), -1));
    if (cb) tc->Enable(cb->GetValue());
    row->Add(tc, 0, wxALIGN_CENTER_VERTICAL);
    card->GetSizer()->Add(row, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(4));
    host->Add(card, 0, wxALL, FromDIP(4));
    card->Fit();

    if (cb)
        cb->Bind(wxEVT_CHECKBOX, [this, comp, name, tc](wxCommandEvent& e) {
            comp->param_on[name] = e.IsChecked();
            tc->Enable(e.IsChecked());
            doc_->dirty = true;
            if (on_edited) on_edited();
        });
    tc->Bind(wxEVT_TEXT, [this, comp, name, tc](wxCommandEvent&) {
        if (rebuilding_) return;
        comp->param_text[name] = tc->GetValue().ToStdString();
        doc_->dirty = true;
        if (on_edited) on_edited();
    });
}

void PropertiesPanel::add_param_row(syms::Component* comp,
                                    const std::string& name,
                                    const std::string& unit, bool parasitic,
                                    const std::string& default_text) {
    (void)unit;
    add_mantissa_exp(comp, name, parasitic, wxString::FromUTF8(default_text));
}

// A free-text value field (a source's DC or AC value). The user may type
// anything `parse_value` accepts -- a bare number, an SI-suffixed value
// ("10k"), or just "0" -- rather than being forced into a mantissa/exponent
// dropdown pair.
void PropertiesPanel::add_scalar_row(const wxString& label, std::string* target,
                                     const std::string& unit) {
    auto* host = cards_host();
    auto* card = new_card(this, label);
    auto* sub = new wxBoxSizer(wxHORIZONTAL);
    auto* tc = new wxTextCtrl(card, wxID_ANY, wxString::FromUTF8(*target),
                              wxDefaultPosition, wxSize(FromDIP(90), -1));
    sub->Add(tc, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(3));
    sub->Add(new wxStaticText(card, wxID_ANY, wxString::FromUTF8(unit)), 0,
             wxALIGN_CENTER_VERTICAL);
    card->GetSizer()->Add(sub, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(4));
    card->Fit();
    host->Add(card, 0, wxALL, FromDIP(4));

    tc->Bind(wxEVT_TEXT, [this, target, tc](wxCommandEvent&) {
        if (rebuilding_) return;
        *target = tc->GetValue().ToStdString(); // keep exactly what was typed
        doc_->dirty = true;
        if (on_edited) on_edited();
    });
}

// One free-text value field for passives / gain blocks: type any engineering
// value ("1k", "-6k", "2.5p"). Far cheaper to build than a mantissa/exponent
// pair of drop-downs, which was the selection lag.
void PropertiesPanel::add_value_selector(syms::Component* comp, bool with_unit) {
    auto* host = cards_host();

    // Op-amps / gain blocks: their "value" is the DC gain, so label it
    // "Gain" (GBW is a separate parameter shown below).
    const char* label = (comp->kind == syms::Kind::OPAMP ||
                         comp->kind == syms::Kind::FDOPAMP ||
                         comp->kind == syms::Kind::AMP)
                            ? "Gain"
                            : "Value";
    auto* card = new_card(this, wxString::FromUTF8(label));
    auto* sub = new wxBoxSizer(wxHORIZONTAL);

    wxString cur = comp->value_text.empty() ? wxString("1")
                                            : wxString::FromUTF8(comp->value_text);
    auto* tc = new wxTextCtrl(card, wxID_ANY, cur, wxDefaultPosition,
                              wxSize(FromDIP(80), -1));
    sub->Add(tc, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(2));
    if (with_unit) {
        std::string unit = (comp->kind == syms::Kind::VDD)
                               ? std::string("V")
                               : syms::kind_display(comp->kind);
        sub->Add(new wxStaticText(card, wxID_ANY, wxString::FromUTF8(unit)), 0,
                 wxALIGN_CENTER_VERTICAL);
    }
    card->GetSizer()->Add(sub, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(4));
    card->Fit();
    host->Add(card, 0, wxALL, FromDIP(4));

    tc->Bind(wxEVT_TEXT, [this, comp, tc](wxCommandEvent&) {
        if (rebuilding_) return;
        comp->value_text = tc->GetValue().ToStdString();
        doc_->dirty = true;
        if (on_edited) on_edited();
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
    // Carry the wire endpoint bindings across the rename. A wire end bound to
    // the old reference would otherwise dangle -- and, since next_ref() can
    // hand the freed name back to a later component, silently re-attach to the
    // wrong symbol (the same class of bug as deleting a source).
    for (auto& w : doc_->wires) {
        if (w.a.kind == WireEnd::Kind::Pin && w.a.ref == old_ref) w.a.ref = nr;
        if (w.b.kind == WireEnd::Kind::Pin && w.b.ref == old_ref) w.b.ref = nr;
    }
    doc_->dirty = true;
    return true;
}

void PropertiesPanel::refresh(Document* doc, const std::string& selection) {
    rebuilding_ = true;
    doc_ = doc;
    // Freeze so the many Add()/layout calls below do not repaint one-by-one --
    // this is the bulk of the "panel is slow and clunky" cost.
    Freeze();
    sel_ = selection;

    // Wipe previous rows (Clear(true) destroys the child windows too) and
    // rebuild. The wrapping card container is recreated lazily.
    if (auto* old = GetSizer()) old->Clear(true);
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    SetSizer(sizer, true);
    cards_ = nullptr;

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
                    hint->Wrap(FromDIP(240));
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

            // --- identity row: Name + "Copy of" side by side (compact) ------
            // The two short fields share one row rather than each stretching the
            // full panel width, which looked clunky.
            bool copyable = syms::is_device(c->kind) || syms::is_copyable(c->kind);
            {
                auto* row = new wxBoxSizer(wxHORIZONTAL);
                row->Add(new wxStaticText(this, wxID_ANY, "Name"), 0,
                         wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
                auto* nm = new wxTextCtrl(this, wxID_ANY,
                                          wxString::FromUTF8(c->ref),
                                          wxDefaultPosition, wxSize(FromDIP(90), -1));
                row->Add(nm, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));

                wxChoice* ch = nullptr;
                if (copyable) {
                    row->Add(new wxStaticText(this, wxID_ANY, "Copy of"), 0,
                             wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
                    auto* names = new wxArrayString();
                    names->Add("(none)");
                    for (const auto& cc : doc_->circuit.comps) {
                        if (cc.ref == c->ref) continue;
                        if (!syms::can_mirror(cc, *c)) continue;
                        names->Add(wxString::FromUTF8(cc.ref));
                    }
                    ch = new wxChoice(this, wxID_ANY, wxDefaultPosition,
                                      wxSize(FromDIP(90), -1), *names);
                    wxString cur = c->mirror_ref.empty()
                                       ? wxString("(none)")
                                       : wxString::FromUTF8(c->mirror_ref);
                    int sel = names->Index(cur);
                    ch->SetSelection(sel == wxNOT_FOUND ? 0 : sel);
                    row->Add(ch, 0, wxALIGN_CENTER_VERTICAL);
                }
                GetSizer()->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP,
                                FromDIP(4));

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
                if (ch)
                    ch->Bind(wxEVT_CHOICE, [this, ch](wxCommandEvent&) {
                        if (rebuilding_) return;
                        syms::Component* cc = nullptr;
                        for (auto& x : doc_->circuit.comps)
                            if (x.ref == sel_) cc = &x;
                        if (!cc) return;
                        wxString v = ch->GetString(ch->GetSelection());
                        cc->mirror_ref = (v == "(none)")
                                             ? std::string()
                                             : v.ToStdString();
                        if (cc->mirror_mult < 1) cc->mirror_mult = 1;
                        doc_->dirty = true;
                        if (on_edited) on_edited();
                        // Rebuild immediately so the value/parameter rows hide
                        // (or reappear) at once, and pin the scroll back to the
                        // top so the header stays visible.
                        CallAfter([this] {
                            refresh(doc_, sel_);
                            Scroll(0, 0);
                        });
                    });
            }

            // When this component is a copy of another, its value/parameters are
            // inherited (and scaled by m): show only the copy controls, not the
            // duplicated value rows.
            bool is_copy = !comp->mirror_ref.empty();

            if (is_copy) {
                auto* mc = new wxBoxSizer(wxHORIZONTAL);
                mc->Add(new wxStaticText(this, wxID_ANY, "Copies (m)"), 0,
                        wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
                auto* msc = new wxSpinCtrl(this, wxID_ANY, wxEmptyString,
                                           wxDefaultPosition, wxSize(FromDIP(80), -1),
                                           wxSP_ARROW_KEYS, 1, 100000,
                                           comp->multiplicity());
                mc->Add(msc, 0, wxALIGN_CENTER_VERTICAL);
                GetSizer()->Add(mc, 0,
                                wxLEFT | wxRIGHT | wxBOTTOM | wxTOP, FromDIP(4));
                msc->Bind(wxEVT_SPINCTRL, [this, msc](wxSpinEvent&) {
                    if (rebuilding_) return;
                    syms::Component* cc = nullptr;
                    for (auto& x : doc_->circuit.comps)
                        if (x.ref == sel_) cc = &x;
                    if (!cc) return;
                    cc->mirror_mult = msc->GetValue();
                    doc_->dirty = true;
                    if (on_edited) on_edited();
                });
                auto* note = new wxStaticText(
                    this, wxID_ANY,
                    wxString::Format("Inherits %s; value scaled by m.",
                                     comp->mirror_ref.c_str()));
                note->SetForegroundColour(wxColour(115, 115, 120));
                note->Wrap(FromDIP(240));
                GetSizer()->Add(note, 0, wxALL, FromDIP(4));
            }

            // Supply rail: a free-text voltage so the user can enter any value
            // (3.3, 5, 12, 1.8, ...), not a fixed mantissa/exponent menu.
            if (!is_copy && c->kind == Kind::VDD)
                add_scalar_row("Supply", &comp->value_text, "V");

            // Value selector for passives and the non-source blocks. Voltage
            // and current sources are handled separately below (they carry DC
            // and AC values, not a single "value").
            if (!is_copy &&
                (c->kind == Kind::R || c->kind == Kind::C ||
                 c->kind == Kind::L || c->kind == Kind::D ||
                 c->kind == Kind::OPAMP || c->kind == Kind::FDOPAMP ||
                 c->kind == Kind::AMP || c->kind == Kind::E ||
                 c->kind == Kind::G))
                add_value_selector(comp, true);

            // Independent V/I sources carry two typeable values, DC and AC.
            // The unit is fixed by the source kind: a voltage source is in
            // volts, a current source in amps.
            if (!is_copy && (c->kind == Kind::V || c->kind == Kind::I)) {
                const char* unit = (c->kind == Kind::V) ? "V" : "A";
                add_header("Source values");
                add_scalar_row("DC", &comp->dc_text, unit);
                add_scalar_row("AC", &comp->ac_text, unit);
            }

            // Device model parameters (checkbox + mantissa/exponent). W and L
            // are only meaningful in the numeric DC mode; in the symbolic modes
            // W/L are symbols (or unused), so hide them to avoid confusion.
            if (!is_copy)
                for (const auto& pd : syms::param_defs(c->kind)) {
                    bool is_geometry = (pd.name == "W" || pd.name == "L");
                    if (is_geometry &&
                        doc_->tech.dc_mode != syms::DcMode::Numeric)
                        continue;
                    add_param_row(comp, pd.name, pd.unit, pd.parasitic,
                                  pd.default_text);
                }

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
    // Scroll back to the top so the selected part's name/header is always the
    // first thing visible in the panel.
    Scroll(0, 0);
    Thaw();
    Refresh(false);
    rebuilding_ = false;
}

void PropertiesPanel::on_analysis_changed(wxCommandEvent&) {}

} // namespace symcirc
