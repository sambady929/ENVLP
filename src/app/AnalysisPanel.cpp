#include "AnalysisPanel.h"
#include "core/Eng.h"

#include <wx/choice.h>
#include <wx/statline.h>

#include <sstream>

namespace symcirc {

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
                       wxVSCROLL) {
    SetScrollRate(0, 10);
    SetBackgroundColour(wxColour(245, 245, 243));
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

void AnalysisPanel::on_add_choice() {}

// ---------------------------------------------------------------------------
void AnalysisPanel::refresh(Document* doc) {
    rebuilding_ = true;
    doc_ = doc;

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

    // run buttons
    {
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        auto* all = new wxButton(this, wxID_ANY, "Run all");
        auto* res = new wxButton(this, wxID_ANY, "Results tab");
        row->Add(all, 1, wxRIGHT, 4);
        row->Add(res, 0);
        root->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);
        all->Bind(wxEVT_BUTTON,
                  [this](wxCommandEvent&) { if (on_run_all) on_run_all(-1); });
        res->Bind(wxEVT_BUTTON,
                  [this](wxCommandEvent&) { if (on_results) on_results(); });
    }

    // card list
    int idx = 0;
    for (auto& c : cards_) {
        auto* box = new wxPanel(this);
        box->SetBackgroundColour(wxColour(252, 252, 250));
        auto* s = new wxBoxSizer(wxVERTICAL);

        auto* top = new wxBoxSizer(wxHORIZONTAL);
        auto* en = new wxCheckBox(box, wxID_ANY, "");
        en->SetValue(c.enabled);
        top->Add(en, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        auto* ttl = new wxStaticText(box, wxID_ANY,
                                     wxString::Format("%d. %s", idx + 1,
                                                      wxString::FromUTF8(c.title)));
        wxFont tf = ttl->GetFont();
        tf.SetWeight(wxFONTWEIGHT_BOLD);
        ttl->SetFont(tf);
        top->Add(ttl, 1, wxALIGN_CENTER_VERTICAL);
        auto* run = new wxButton(box, wxID_ANY, "Run");
        auto* del = new wxButton(box, wxID_ANY, "x");
        top->Add(run, 0, wxRIGHT, 4);
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
        add_field("in", wxString::FromUTF8(c.input_ref),
                  [&c](const wxString& v) { c.input_ref = v.ToStdString(); });
        add_field("out", wxString::FromUTF8(c.output),
                  [&c](const wxString& v) { c.output = v.ToStdString(); });
        if (c.kind == AnalysisKind::LoopGain)
            add_field("probe", wxString::FromUTF8(c.probe_ref),
                      [&c](const wxString& v) { c.probe_ref = v.ToStdString(); });

        if (c.kind != AnalysisKind::DC && c.kind != AnalysisKind::Noise) {
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

            auto* opts = new wxBoxSizer(wxHORIZONTAL);
            auto* prune = new wxCheckBox(box, wxID_ANY, "ignore negligible");
            prune->SetValue(c.prune);
            auto* gro = new wxCheckBox(box, wxID_ANY, "gm*ro>>1");
            gro->SetValue(c.gm_ro);
            auto* af = new wxCheckBox(box, wxID_ANY, "approx roots");
            af->SetValue(c.approx_factor);
            opts->Add(prune, 0, wxRIGHT, 8);
            opts->Add(gro, 0, wxRIGHT, 8);
            opts->Add(af, 0);
            s->Add(opts, 0, wxLEFT | wxRIGHT | wxBOTTOM, 4);
            prune->Bind(wxEVT_CHECKBOX, [this, &c](wxCommandEvent& e) {
                c.prune = e.IsChecked();
                if (doc_) doc_->dirty = true;
                if (on_changed) on_changed();
            });
            gro->Bind(wxEVT_CHECKBOX, [this, &c](wxCommandEvent& e) {
                c.gm_ro = e.IsChecked();
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
        en->Bind(wxEVT_CHECKBOX, [this, &c](wxCommandEvent& e) {
            c.enabled = e.IsChecked();
            if (doc_) doc_->dirty = true;
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
          << (c.gm_ro ? 1 : 0) << " " << (c.enabled ? 1 : 0) << " " << q(title)
          << " " << (c.approx_factor ? 1 : 0) << "\n";
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
        int ty = 0, npts = 10, pr = 1, par = 1, gro = 1, en = 1;
        ls >> fs >> fe >> ty >> npts >> pr >> par >> gro >> en;
        std::string title = unq(ls);
        int af = 1;
        ls >> af;
        c.kind = analysis_kind_from_name(kind);
        c.sweep.f_start_hz = fs > 0 ? fs : 1.0;
        c.sweep.f_stop_hz = fe > c.sweep.f_start_hz ? fe : c.sweep.f_start_hz * 1e3;
        c.sweep.type = ty == 1 ? syms::SweepType::Octave
                               : ty == 2 ? syms::SweepType::Linear
                                         : syms::SweepType::Decade;
        c.sweep.points_per_interval = npts > 0 ? npts : 10;
        c.prune = pr != 0;
        c.use_parallel = par != 0;
        c.gm_ro = gro != 0;
        c.enabled = en != 0;
        c.approx_factor = af != 0;
        c.title = title.empty() ? analysis_kind_name(c.kind) : title;
        cards_.push_back(c);
    }
    return true;
}

} // namespace symcirc
