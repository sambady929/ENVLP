#include "AnalysisPanel.h"
#include "Theme.h"
#include "core/Eng.h"
#include "core/SpiceModel.h"

#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/combobox.h>
#include <wx/dialog.h>
#include <wx/filedlg.h>
#include <wx/statline.h>

#include <sstream>

namespace envlp {

using syms::AnalysisKind;

wxBEGIN_EVENT_TABLE(AnalysisPanel, wxScrolledWindow)
wxEND_EVENT_TABLE()

namespace {
struct KindEntry { const char* name; AnalysisKind kind; };
const KindEntry kKinds[] = {
    {"Transfer function (H(s))", AnalysisKind::TransferFunction},
    {"AC (small-signal)", AnalysisKind::AC},
    {"DC operating point", AnalysisKind::DC},
    {"PSR / PSRR", AnalysisKind::PSRR},
    {"Loop gain (return ratio)", AnalysisKind::LoopGain},
    {"Short-circuit current", AnalysisKind::ShortCircuitCurrent},
    {"Input impedance", AnalysisKind::InputImpedance},
    {"Output impedance", AnalysisKind::OutputImpedance},
    {"Differential (Adm/Acm/CMRR)", AnalysisKind::Differential},
    {"Noise", AnalysisKind::Noise},
};
constexpr int kKindCount = int(sizeof(kKinds) / sizeof(kKinds[0]));
} // namespace

const char* analysis_kind_name(AnalysisKind k) {
    for (const auto& e : kKinds)
        if (e.kind == k) return e.name;
    return "?";
}

AnalysisKind analysis_kind_from_name(const std::string& n) {
    for (const auto& e : kKinds)
        if (n == e.name) return e.kind;
    return AnalysisKind::TransferFunction;
}

AnalysisPanel::AnalysisPanel(wxWindow* parent)
    : wxScrolledWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                       wxVSCROLL | wxHSCROLL) {
    SetScrollRate(FromDIP(10), FromDIP(10));
    SetBackgroundColour(theme::chrome_bg);
}

void AnalysisPanel::add_card(AnalysisKind kind) {
    AnalysisCard c;
    c.kind = kind;
    c.title = analysis_kind_name(kind);
    if (doc_) {
        c.input_ref = doc_->req.input_ref;
        c.output = doc_->req.output;
        c.sweep = doc_->req.sweep;
        c.prune = doc_->req.prune;
        c.use_parallel = doc_->req.use_parallel;
        c.approx_factor = doc_->req.approx_factor;
    }
    cards_.push_back(c);
    if (doc_) doc_->dirty = true;
    refresh(doc_);
    if (on_changed) on_changed();
}

void AnalysisPanel::remove_card(int i) {
    if (i < 0 || i >= int(cards_.size())) return;
    cards_.erase(cards_.begin() + i);
    if (doc_) doc_->dirty = true;
    refresh(doc_);
    if (on_changed) on_changed();
}

void AnalysisPanel::move_card(int i, int dir) {
    int j = i + dir;
    if (i < 0 || j < 0 || j >= int(cards_.size()) || i >= int(cards_.size()))
        return;
    std::swap(cards_[i], cards_[j]);
    if (doc_) doc_->dirty = true;
    refresh(doc_);
    if (on_changed) on_changed();
}


// A one-line summary of the active DC settings, shown next to the options
// button so the card reads informatively without opening the dialog.
static wxString dc_settings_summary(const syms::TechParams& tech) {
    wxString s = wxString::Format("VDD %g V", tech.vdd);
    switch (tech.dc_mode) {
        case syms::DcMode::GmOverId:
            s += wxString::Format(", Vthn %g V, Vthp %g V", tech.vthn,
                                  tech.vthp);
            break;
        case syms::DcMode::SquareLaw:
            s += wxString::Format(", Vthn %g V, Vthp %g V, uCox %g/%g",
                                  tech.vthn, tech.vthp, tech.uncox, tech.upcox);
            if (tech.assume_saturation) s += ", assume saturated";
            break;
    }
    return s;
}

// The DC card's mode selector plus a button that opens the mode-specific
// options dialog. Everything else (supply, process values, SPICE model) lives
// in that dialog, so the card stays in/out/mode/options.
void AnalysisPanel::build_dc_settings(wxWindow* box, wxSizer* s, AnalysisCard& c) {
    if (!doc_) return;
    syms::TechParams& tech = doc_->tech;

    auto* mode_row = new wxBoxSizer(wxHORIZONTAL);
    mode_row->Add(new wxStaticText(box, wxID_ANY, "mode"), 0,
                  wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    auto* mode = new wxChoice(box, wxID_ANY);
    mode->Append("1. gm/Id symbolic");
    mode->Append("2. Square law symbolic");
    mode->SetSelection(tech.dc_mode == syms::DcMode::GmOverId
                           ? 0
                           : tech.dc_mode == syms::DcMode::SquareLaw ? 1 : 0);
    mode_row->Add(mode, 1);
    s->Add(mode_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);
    mode->Bind(wxEVT_CHOICE, [this, mode](wxCommandEvent&) {
        if (rebuilding_) return;
        doc_->tech.dc_mode = mode->GetSelection() == 0
                                 ? syms::DcMode::GmOverId
                                 : syms::DcMode::SquareLaw;
        doc_->dirty = true;
        if (on_tech_changed) on_tech_changed();
        if (on_changed) on_changed();
        // The options/summary row depends on the mode: rebuild after the
        // event handler returns (the control is owned by the old layout).
        CallAfter([this] { refresh(doc_); });
    });

    auto* row = new wxBoxSizer(wxHORIZONTAL);
    auto* btn = new wxButton(box, wxID_ANY, "DC analysis options...");
    row->Add(btn, 0, wxRIGHT, 6);
    auto* sum = new wxStaticText(box, wxID_ANY, dc_settings_summary(tech));
    sum->SetForegroundColour(theme::text_muted);
    wxFont sf = sum->GetFont();
    sf.SetPointSize(std::max(7, sf.GetPointSize() - 1));
    sum->SetFont(sf);
    row->Add(sum, 1, wxALIGN_CENTER_VERTICAL);
    s->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);
    btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { open_dc_options(); });
}

// Modal "DC analysis options" dialog. The fields shown depend on the mode
// selected on the card: the supply rail is always present; Vth applies to the
// symbolic modes; uCox to the square-law mode; the SPICE model source to the
// numeric mode.
void AnalysisPanel::open_dc_options() {
    if (!doc_) return;
    syms::TechParams& tech = doc_->tech;
    const bool square = tech.dc_mode == syms::DcMode::SquareLaw;

    wxDialog dlg(this, wxID_ANY, "DC analysis options");
    auto* top = new wxBoxSizer(wxVERTICAL);
    auto* grid = new wxFlexGridSizer(2, 8, 6);
    grid->AddGrowableCol(1, 1);
    auto add_row = [&](const wxString& label, wxWindow* w) {
        grid->Add(new wxStaticText(&dlg, wxID_ANY, label), 0,
                  wxALIGN_CENTER_VERTICAL);
        grid->Add(w, 1, wxEXPAND);
    };

    // Supply rail: shared by every mode.
    auto* vdd = new wxTextCtrl(&dlg, wxID_ANY, wxString::Format("%g", tech.vdd));
    add_row("Supply VDD (V):", vdd);

    auto* vthn = new wxTextCtrl(&dlg, wxID_ANY, wxString::Format("%g", tech.vthn));
    add_row("Vthn (V):", vthn);
    auto* vthp = new wxTextCtrl(&dlg, wxID_ANY, wxString::Format("%g", tech.vthp));
    add_row("Vthp (V):", vthp);

    wxTextCtrl* uncox = nullptr;
    wxTextCtrl* upcox = nullptr;
    wxCheckBox* sat = nullptr;
    if (square) {
        uncox = new wxTextCtrl(&dlg, wxID_ANY,
                               wxString::Format("%g", tech.uncox));
        add_row("uN*Cox (A/V^2):", uncox);
        upcox = new wxTextCtrl(&dlg, wxID_ANY,
                               wxString::Format("%g", tech.upcox));
        add_row("uP*Cox (A/V^2):", upcox);
        sat = new wxCheckBox(&dlg, wxID_ANY, "assume all in saturation");
        sat->SetValue(tech.assume_saturation);
        grid->AddSpacer(1);
        grid->Add(sat, 1, wxEXPAND);
    }
    wxCheckBox* clm = nullptr;
    if (!square) {
        // Channel-length modulation is only stampable in the gm/Id mode (square
        // law's gds = lambda*Id depends on the unknown current).
        clm = new wxCheckBox(&dlg, wxID_ANY,
                             "include channel-length modulation");
        clm->SetValue(tech.include_clm);
        grid->AddSpacer(1);
        grid->Add(clm, 1, wxEXPAND);
    }

    top->Add(grid, 0, wxEXPAND | wxALL, 12);
    auto* buttons = new wxStdDialogButtonSizer();
    buttons->AddButton(new wxButton(&dlg, wxID_OK));
    buttons->AddButton(new wxButton(&dlg, wxID_CANCEL));
    buttons->Realize();
    top->Add(buttons, 0, wxALIGN_RIGHT | wxALL, 10);
    dlg.SetSizerAndFit(top);

    if (dlg.ShowModal() != wxID_OK) return;

    double v = 0.0;
    if (vdd->GetValue().ToDouble(&v)) tech.vdd = v;
    if (vthn->GetValue().ToDouble(&v)) tech.vthn = v;
    if (vthp->GetValue().ToDouble(&v)) tech.vthp = v;
    if (uncox && uncox->GetValue().ToDouble(&v)) tech.uncox = v;
    if (upcox && upcox->GetValue().ToDouble(&v)) tech.upcox = v;
    if (sat) tech.assume_saturation = sat->GetValue();
    if (clm) tech.include_clm = clm->GetValue();

    doc_->dirty = true;
    if (on_tech_changed) on_tech_changed();
    if (on_changed) on_changed();
    // Defer the rebuild: this runs from a button inside the card's layout,
    // and refresh() destroys and recreates that layout.
    CallAfter([this] { refresh(doc_); });
}

// ---------------------------------------------------------------------------
void AnalysisPanel::refresh(Document* doc) {
    rebuilding_ = true;
    doc_ = doc;

    // Rebuilding destroys and recreates every card, which without this flashed
    // the whole panel blank for a moment on each add/remove/move. Freezing the
    // window suppresses the intermediate repaints.
    Freeze();

    if (auto* old = GetSizer()) old->Clear(true);
    auto* root = new wxBoxSizer(wxVERTICAL);
    SetSizer(root, true);

    auto* header = new wxStaticText(this, wxID_ANY, "Analysis steps");
    wxFont hf = header->GetFont();
    hf.SetWeight(wxFONTWEIGHT_BOLD);
    header->SetFont(hf);
    root->Add(header, 0, wxALL, 6);

    // add-card row
    {
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        wxArrayString names;
        for (const auto& e : kKinds) names.Add(e.name);
        auto* add = new wxChoice(this, wxID_ANY, wxDefaultPosition,
                                 wxDefaultSize, names);
        add->SetSelection(0);
        row->Add(add, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        auto* btn = new wxButton(this, wxID_ANY, "+ Add");
        row->Add(btn, 0);
        root->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);
        btn->Bind(wxEVT_BUTTON, [this, add](wxCommandEvent&) {
            int sel = add->GetSelection();
            if (sel >= 0 && sel < kKindCount) add_card(kKinds[sel].kind);
        });
    }

    // run button row
    {
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        auto* res = new wxButton(this, wxID_ANY, "Results tab");
        row->AddStretchSpacer(1);
        row->Add(res, 0);
        root->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);
        res->Bind(wxEVT_BUTTON,
                  [this](wxCommandEvent&) { if (on_results) on_results(); });
    }

    // card list
    int idx = 0;
    for (auto& c : cards_) {
        auto* box = new wxPanel(this);
        box->SetBackgroundColour(theme::card_bg);
        auto* s = new wxBoxSizer(wxVERTICAL);

        auto* top = new wxBoxSizer(wxHORIZONTAL);
        auto* ttl = new wxStaticText(box, wxID_ANY,
                                     wxString::Format("%d. %s", idx + 1,
                                                      wxString::FromUTF8(c.title)));
        wxFont tf = ttl->GetFont();
        tf.SetWeight(wxFONTWEIGHT_BOLD);
        ttl->SetFont(tf);
        top->Add(ttl, 1, wxALIGN_CENTER_VERTICAL);
        // Experimental cards are marked so the user knows the output may
        // change shape between versions.
        bool experimental = c.kind == AnalysisKind::DC ||
                            c.kind == AnalysisKind::Differential;
        if (experimental) {
            auto* badge = new wxStaticText(box, wxID_ANY, " experimental ");
            badge->SetForegroundColour(wxColour(150, 90, 20));
            wxFont bf = badge->GetFont();
            bf.SetPointSize(std::max(7, bf.GetPointSize() - 2));
            badge->SetFont(bf);
            top->Add(badge, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        }
        auto* run = new wxButton(box, wxID_ANY, "Run");
        // Card order is meaningful (the .scx card list, and therefore the order
        // the results come in). Small arrow buttons instead of word labels.
        auto* up = new wxButton(box, wxID_ANY, wxString(wxUniChar(0x2191)),
                                wxDefaultPosition, wxSize(26, -1),
                                wxBU_EXACTFIT);
        auto* dn = new wxButton(box, wxID_ANY, wxString(wxUniChar(0x2193)),
                                wxDefaultPosition, wxSize(26, -1),
                                wxBU_EXACTFIT);
        auto* del = new wxButton(box, wxID_ANY, wxString(wxUniChar(0x00D7)),
                                 wxDefaultPosition, wxSize(26, -1),
                                 wxBU_EXACTFIT);
        up->Bind(wxEVT_BUTTON, [this, idx](wxCommandEvent&) {
            move_card(idx, -1);
        });
        dn->Bind(wxEVT_BUTTON, [this, idx](wxCommandEvent&) {
            move_card(idx, +1);
        });
        top->Add(run, 0, wxRIGHT, 4);
        top->Add(up, 0, wxRIGHT, 1);
        top->Add(dn, 0, wxRIGHT, 3);
        top->Add(del, 0);
        s->Add(top, 0, wxEXPAND | wxALL, 4);

        auto add_field = [&](const wxString& label, const wxString& value,
                             std::function<void(const wxString&)> set) {
            auto* row = new wxBoxSizer(wxHORIZONTAL);
            row->Add(new wxStaticText(box, wxID_ANY, label), 0,
                     wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
            auto* tc = new wxTextCtrl(box, wxID_ANY, value);
            row->Add(tc, 1);
            s->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);
            tc->Bind(wxEVT_TEXT, [this, set, tc](wxCommandEvent&) {
                if (rebuilding_) return;
                set(tc->GetValue());
                if (doc_) doc_->dirty = true;
                if (on_changed) on_changed();
            });
        };
        // AC drives every independent source (the output is the superposition),
        // output impedance turns all sources off, the differential card uses its
        // own port instead of a single input source, the short-circuit card is
        // driven like the AC card, and DC drives every source with its DC value
        // -- so the "in" field is omitted for all of those.
        bool needs_input = c.kind != AnalysisKind::AC &&
                           c.kind != AnalysisKind::DC &&
                           c.kind != AnalysisKind::OutputImpedance &&
                           c.kind != AnalysisKind::ShortCircuitCurrent &&
                           c.kind != AnalysisKind::Differential;
        if (needs_input)
            add_field("in", wxString::FromUTF8(c.input_ref),
                      [&c](const wxString& v) { c.input_ref = v.ToStdString(); });
        if (c.kind == AnalysisKind::Differential) {
            add_field("in+", wxString::FromUTF8(c.in_port_p),
                      [&c](const wxString& v) { c.in_port_p = v.ToStdString(); });
            add_field("in-", wxString::FromUTF8(c.in_port_n),
                      [&c](const wxString& v) { c.in_port_n = v.ToStdString(); });
        }
        // Input impedance is the input source's own response and DC lists every
        // node, so neither needs an output. Output impedance needs the output
        // node but no input. Everything else needs both.
        bool needs_output = c.kind != AnalysisKind::InputImpedance &&
                            c.kind != AnalysisKind::DC;
        // A differential card measures an output PORT, not a single node. Show
        // out+ and out- separately; they combine to "V(a)-V(b)" for the solver
        // (a bare "V(node)" means that node relative to ground).
        if (c.kind == AnalysisKind::Differential) {
            auto strip_v = [](std::string t) {
                if (t.size() > 3 && (t[0] == 'V' || t[0] == 'v')) t = t.substr(2);
                if (!t.empty() && t.back() == ')') t.pop_back();
                return t;
            };
            auto parse_out = [&](const std::string& s) {
                std::string op, on;
                size_t dash = s.find("-V(");
                if (dash == std::string::npos) dash = s.find(" - V(");
                if (dash != std::string::npos) {
                    op = strip_v(s.substr(0, dash));
                    size_t d = s.find("V(", dash);
                    on = strip_v(d == std::string::npos ? std::string() :
                                                      s.substr(d));
                } else {
                    op = strip_v(s);
                }
                return std::pair<std::string, std::string>(op, on);
            };
            auto compose = [](const std::string& op, const std::string& on) {
                if (on.empty()) return "V(" + op + ")";
                return "V(" + op + ")-V(" + on + ")";
            };
            auto pr = parse_out(c.output);
            add_field("out+", wxString::FromUTF8(pr.first),
                      [&c, parse_out, compose](const wxString& v) {
                          c.output = compose(v.ToStdString(),
                                             parse_out(c.output).second);
                      });
            add_field("out-", wxString::FromUTF8(pr.second),
                      [&c, parse_out, compose](const wxString& v) {
                          c.output = compose(parse_out(c.output).first,
                                             v.ToStdString());
                      });
        } else if (needs_output)
            add_field("out", wxString::FromUTF8(c.output),
                      [&c](const wxString& v) { c.output = v.ToStdString(); });
        if (c.kind == AnalysisKind::LoopGain)
            add_field("probe", wxString::FromUTF8(c.probe_ref),
                      [&c](const wxString& v) { c.probe_ref = v.ToStdString(); });

        if (c.kind != AnalysisKind::DC) {
            // Noise uses the sweep as its integration band, so it keeps the
            // sweep row too (only DC has no frequency axis).
            // Frequency sweep in the usual SPICE terms: start, stop, the
            // interval type (decade / octave / linear) and points per interval.
            auto* sweep_row = new wxBoxSizer(wxHORIZONTAL);
            sweep_row->Add(new wxStaticText(box, wxID_ANY, "sweep"),
                           0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
            auto* sw = new wxTextCtrl(
                box, wxID_ANY,
                wxString::FromUTF8(syms::eng::format_eng(c.sweep.f_start_hz, 3) +
                                   " " +
                                   syms::eng::format_eng(c.sweep.f_stop_hz, 3)));
            sweep_row->Add(sw, 1, wxRIGHT, 4);
            wxArrayString types;
            types.Add("decade");
            types.Add("octave");
            types.Add("linear");
            auto* ty = new wxChoice(box, wxID_ANY, wxDefaultPosition,
                                    wxDefaultSize, types);
            ty->SetSelection(c.sweep.type == syms::SweepType::Decade
                                 ? 0
                                 : c.sweep.type == syms::SweepType::Octave ? 1
                                                                           : 2);
            sweep_row->Add(ty, 0, wxRIGHT, 4);
            auto* ppi = new wxTextCtrl(
                box, wxID_ANY, wxString::Format("%d", c.sweep.points_per_interval),
                wxDefaultPosition, wxSize(46, -1));
            sweep_row->Add(ppi, 0, wxRIGHT, 2);
            sweep_row->Add(new wxStaticText(box, wxID_ANY, "/int"), 0,
                           wxALIGN_CENTER_VERTICAL);
            s->Add(sweep_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);
            sw->Bind(wxEVT_TEXT, [this, &c, sw](wxCommandEvent&) {
                if (rebuilding_) return;
                double a = c.sweep.f_start_hz, b = c.sweep.f_stop_hz;
                std::istringstream is(sw->GetValue().ToStdString());
                if ((is >> a)) {
                    // "start" and optionally "stop"
                    double bb;
                    if (is >> bb) {
                        if (a > 0) c.sweep.f_start_hz = a;
                        if (bb > c.sweep.f_start_hz) c.sweep.f_stop_hz = bb;
                    } else if (a > 0) {
                        // treat a single value as the start; keep the stop
                        c.sweep.f_start_hz = a;
                    }
                }
                if (doc_) doc_->dirty = true;
                if (on_changed) on_changed();
            });
            ty->Bind(wxEVT_CHOICE, [this, &c, ty](wxCommandEvent&) {
                if (rebuilding_) return;
                int sel = ty->GetSelection();
                c.sweep.type = sel == 1 ? syms::SweepType::Octave
                                        : sel == 2 ? syms::SweepType::Linear
                                                   : syms::SweepType::Decade;
                if (doc_) doc_->dirty = true;
                if (on_changed) on_changed();
            });
            ppi->Bind(wxEVT_TEXT, [this, &c, ppi](wxCommandEvent&) {
                if (rebuilding_) return;
                long n = c.sweep.points_per_interval;
                if (ppi->GetValue().ToLong(&n) && n >= 1 && n <= 100000)
                    c.sweep.points_per_interval = int(n);
                if (doc_) doc_->dirty = true;
                if (on_changed) on_changed();
            });
        }

        // The DC card carries the process/model settings (previously a modal
        // dialog) so everything that affects the operating point sits with it.
        if (c.kind == AnalysisKind::DC) build_dc_settings(box, s, c);

        // Per-card analysis options (every card, including DC).
        {
            auto* opts = new wxBoxSizer(wxHORIZONTAL);
            auto* prune = new wxCheckBox(box, wxID_ANY, "ignore negligible");
            prune->SetValue(c.prune);
            auto* af = new wxCheckBox(box, wxID_ANY, "approx roots");
            af->SetValue(c.approx_factor);
            opts->Add(prune, 0, wxRIGHT, 8);
            opts->Add(af, 0);
            s->Add(opts, 0, wxLEFT | wxRIGHT | wxBOTTOM, 4);
            prune->Bind(wxEVT_CHECKBOX, [this, &c](wxCommandEvent& e) {
                c.prune = e.IsChecked();
                if (doc_) doc_->dirty = true;
                if (on_changed) on_changed();
            });
            af->Bind(wxEVT_CHECKBOX, [this, &c](wxCommandEvent& e) {
                c.approx_factor = e.IsChecked();
                if (doc_) doc_->dirty = true;
                if (on_changed) on_changed();
            });
        }

        box->SetSizer(s);
        root->Add(box, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);

        int card_index = idx;
        run->Bind(wxEVT_BUTTON, [this, card_index](wxCommandEvent&) {
            if (on_run_one) on_run_one(card_index);
        });
        del->Bind(wxEVT_BUTTON, [this, card_index](wxCommandEvent&) {
            remove_card(card_index);
        });
        ++idx;
    }

    if (cards_.empty()) {
        auto* t = new wxStaticText(
            this, wxID_ANY,
            "No analysis steps yet.\nPick one above and press + Add.");
        t->SetForegroundColour(wxColour(115, 115, 120));
        root->Add(t, 0, wxALL, 8);
    }

    root->AddSpacer(6);
    FitInside(); // sets the virtual size to the full content height so the
                 // panel scrolls instead of clipping added cards (#5)
    Layout();
    Thaw();
    rebuilding_ = false;
}

// ---------------------------------------------------------------------------
// serialisation
//   card <kind> <in> <out> <probe> <fstart> <fstop> <stype> <npts> <prune>
//        <par> <en> <title> <approx>
// ---------------------------------------------------------------------------
std::string AnalysisPanel::serialize() const {
    std::ostringstream o;
    auto q = [](const std::string& s) {
        std::string r = "\"";
        for (char ch : s) {
            if (ch == '"' || ch == '\\') r += '\\';
            r += ch;
        }
        r += '"';
        return r;
    };
    auto type_of = [](syms::SweepType t) {
        return t == syms::SweepType::Octave
                   ? 1
                   : t == syms::SweepType::Linear ? 2 : 0;
    };
    for (const auto& c : cards_) {
        std::string title = c.title;
        for (char& ch : title) if (ch == '"') ch = '\'';
        o << "card " << q(analysis_kind_name(c.kind)) << " " << q(c.input_ref)
          << " " << q(c.output) << " " << q(c.probe_ref) << " "
          << c.sweep.f_start_hz << " " << c.sweep.f_stop_hz << " "
          << type_of(c.sweep.type) << " " << c.sweep.points_per_interval << " "
          << (c.prune ? 1 : 0) << " " << (c.use_parallel ? 1 : 0) << " "
          << (c.enabled ? 1 : 0) << " " << q(title)
          << " " << (c.approx_factor ? 1 : 0) << " " << q(c.in_port_p) << " "
          << q(c.in_port_n)
          // Appended pruning thresholds (absent in older files).
          << " " << c.component_threshold_ratio << " "
          << c.pole_zero_threshold_ratio << " "
          << (c.pole_ref == syms::AnalysisRequest::PoleRef::UgBw ? 1 : 0)
          << "\n";
    }
    return o.str();
}

bool AnalysisPanel::deserialize(const std::string& data) {
    auto unq = [](std::istringstream& ls) -> std::string {
        std::string s;
        ls >> std::ws;
        if (ls.peek() != '"') { ls >> s; return s; }
        ls.get();
        char c;
        while (ls.get(c) && c != '"') {
            if (c == '\\' && ls.peek() != EOF) ls.get(c);
            s += c;
        }
        return s;
    };
    std::istringstream in(data);
    std::string line;
    cards_.clear();
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string kw;
        ls >> kw;
        if (kw != "card") continue;
        AnalysisCard c;
        std::string kind = unq(ls);
        c.input_ref = unq(ls);
        c.output = unq(ls);
        c.probe_ref = unq(ls);
        double fs = 1.0, fe = 1e9;
        int ty = 0, npts = 10, pr = 1, par = 1, en = 1;
        ls >> fs >> fe >> ty >> npts >> pr >> par;
        // The "gm*ro>>1" field was removed. An older file still carries it as
        // an extra numeric token between use_parallel and enabled; detect it by
        // whether the token after the next one is the quoted title or a number.
        int x = 1;
        ls >> x;
        ls >> std::ws;
        if (ls.peek() == '"') {
            en = x;                    // no gm*ro field: x is enabled
        } else {
            ls >> en;                  // old: x was the (ignored) gm*ro field
        }
        std::string title = unq(ls);
        int af = 1;
        ls >> af;
        // Differential port nodes (appended; absent in older files).
        ls >> std::ws;
        if (ls.peek() == '"') c.in_port_p = unq(ls);
        ls >> std::ws;
        if (ls.peek() == '"') c.in_port_n = unq(ls);
        // Appended pruning thresholds (absent in older files). Parse with
        // peek() so a file that stops after the port nodes still loads.
        ls >> std::ws;
        if (ls.peek() != EOF && ls.peek() != '\n') {
            double ctr = 10.0, pzr = 1000.0;
            int poleref = 0;
            if (ls >> ctr) {
                if (ctr > 1.0) c.component_threshold_ratio = ctr;
                if (ls >> pzr) {
                    if (pzr > 1.0) c.pole_zero_threshold_ratio = pzr;
                    if (ls >> poleref)
                        c.pole_ref = poleref ? syms::AnalysisRequest::PoleRef::UgBw
                                             : syms::AnalysisRequest::PoleRef::Dominant;
                }
            }
        }
        c.kind = analysis_kind_from_name(kind);
        c.sweep.f_start_hz = fs > 0 ? fs : 1.0;
        c.sweep.f_stop_hz = fe > c.sweep.f_start_hz ? fe : c.sweep.f_start_hz * 1e3;
        c.sweep.type = ty == 1 ? syms::SweepType::Octave
                               : ty == 2 ? syms::SweepType::Linear
                                         : syms::SweepType::Decade;
        c.sweep.points_per_interval = npts > 0 ? npts : 10;
        c.prune = pr != 0;
        c.use_parallel = par != 0;
        c.enabled = en != 0;
        c.approx_factor = af != 0;
        c.title = title.empty() ? analysis_kind_name(c.kind) : title;
        cards_.push_back(c);
    }
    return true;
}

} // namespace envlp
