#include "PropertiesPanel.h"
#include "core/Eng.h"

#include <cstdlib>
#include <wx/checkbox.h>
#include <wx/spinctrl.h>

namespace symcirc {

using syms::Kind;

wxBEGIN_EVENT_TABLE(PropertiesPanel, wxScrolledWindow)
wxEND_EVENT_TABLE()

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

void PropertiesPanel::refresh(Document* doc, const std::string& selection) {
    rebuilding_ = true;
    doc_ = doc;
    sel_ = selection;

    // wipe previous rows (Clear(true) destroys the child windows too)
    if (auto* old = GetSizer()) old->Clear(true);
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    SetSizer(sizer, true);

    // ---------------- analysis settings (always shown) ----------------
    add_header("Analysis");
    if (doc_) {
        // input source: list independent sources
        wxArrayString srcs;
        for (const auto& c : doc_->circuit.comps)
            if (syms::is_independent_source(c.kind)) srcs.Add(c.ref);
        sizer->Add(new wxStaticText(this, wxID_ANY, "Input"), 0,
                   wxALIGN_CENTER_VERTICAL | wxLEFT | wxTOP, 4);
        auto* in = new wxComboBox(this, wxID_ANY, doc_->req.input_ref,
                                  wxDefaultPosition, wxDefaultSize, srcs,
                                  wxCB_DROPDOWN);
        sizer->Add(in, 1, wxEXPAND | wxRIGHT, 6);
        in->Bind(wxEVT_COMBOBOX,
                 [this](wxCommandEvent& e) {
                     if (rebuilding_) return;
                     doc_->req.input_ref = e.GetString().ToStdString();
                     doc_->dirty = true;
                     if (on_edited) on_edited();
                 });
        in->Bind(wxEVT_TEXT, [this](wxCommandEvent& e) {
            if (rebuilding_) return;
            doc_->req.input_ref = e.GetString().ToStdString();
            doc_->dirty = true;
            if (on_edited) on_edited();
        });

        add_text("Output", wxString::FromUTF8(doc_->req.output),
                 [this](const wxString& v) {
                     doc_->req.output = v.ToStdString();
                     doc_->dirty = true;
                 });

        add_text("f0 (Hz)", wxString::FromUTF8(
                                syms::eng::format_si(doc_->req.f0_hz)),
                 [this](const wxString& v) {
                     double f = doc_->req.f0_hz;
                     if (syms::eng::parse_value(v.ToStdString(), f) && f > 0)
                         doc_->req.f0_hz = f;
                     doc_->dirty = true;
                 });

        add_spin("Threshold (dB)", int(doc_->req.threshold_db), 0, 200,
                 [this](int v) {
                     doc_->req.threshold_db = v;
                     doc_->dirty = true;
                 });

        auto* glob = new wxCheckBox(
            this, wxID_ANY,
            "Rank against whole polynomial (uses f0)");
        glob->SetValue(doc_->req.global_ref);
        glob->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent& e) {
            if (rebuilding_) return;
            doc_->req.global_ref = e.IsChecked();
            doc_->dirty = true;
            if (on_edited) on_edited();
        });
        sizer->Add(glob, 0, wxALL, 4);
    }

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
                }
            } else if (sel_.rfind("#wire", 0) == 0) {
                int i = std::atoi(sel_.c_str() + 5);
                if (i >= 0 && i < int(doc_->wires.size())) {
                    add_header("Wire");
                    auto* info = new wxStaticText(
                        this, wxID_ANY,
                        wxString::Format("%d point(s)", int(doc_->wires[i].pts.size())));
                    sizer->Add(info, 0, wxALL, 4);
                    auto* rm = new wxButton(this, wxID_ANY, "Delete wire");
                    sizer->Add(rm, 0, wxALL, 4);
                    rm->Bind(wxEVT_BUTTON, [this, i](wxCommandEvent&) {
                        doc_->wires.erase(doc_->wires.begin() + i);
                        doc_->dirty = true;
                        if (on_edited) on_edited();
                    });
                }
            }
        } else if (const syms::Component* c = doc_->circuit.find(sel_)) {
            // We need a mutable copy pointer: find() is const.
            syms::Component* comp = nullptr;
            for (auto& cc : doc_->circuit.comps)
                if (cc.ref == sel_) comp = &cc;

            add_header(wxString::FromUTF8(
                syms::kind_display(c->kind) + "  " + c->ref));

            add_text("Value", wxString::FromUTF8(c->value_text),
                     [comp](const wxString& v) { comp->value_text = v.ToStdString(); });

            add_spin("Size (dB)", c->size_db, -200, 200,
                     [comp](int v) { comp->size_db = v; });

            // device parameters: always editable estimates; parasitic ones
            // additionally get an on/off checkbox
            for (const auto& pd : syms::param_defs(c->kind)) {
                bool on = comp->param_enabled(pd.name);
                wxTextCtrl* est = nullptr;
                wxSpinCtrl* db = nullptr;
                if (pd.parasitic) {
                    auto* row = new wxCheckBox(
                        this, wxID_ANY,
                        wxString::FromUTF8(pd.name + "  (" + pd.unit + ")"));
                    row->SetValue(on);
                    sizer->Add(row, 0, wxLEFT | wxTOP, 6);

                    auto* sub = new wxBoxSizer(wxHORIZONTAL);
                    est = new wxTextCtrl(
                        this, wxID_ANY,
                        wxString::FromUTF8(comp->param_text.count(pd.name)
                                               ? comp->param_text.at(pd.name)
                                               : pd.default_text));
                    db = new wxSpinCtrl(
                        this, wxID_ANY, wxEmptyString, wxDefaultPosition,
                        wxDefaultSize, wxSP_ARROW_KEYS, -200, 200,
                        comp->param_db.count(pd.name)
                            ? comp->param_db.at(pd.name)
                            : 0);
                    est->Enable(on);
                    db->Enable(on);
                    sub->Add(new wxStaticText(this, wxID_ANY, "est"), 0,
                             wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
                    sub->Add(est, 1, wxEXPAND | wxRIGHT, 4);
                    sub->Add(db, 0, 0);
                    sizer->Add(sub, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);

                    row->Bind(wxEVT_CHECKBOX, [comp, pd, this, est, db](
                                                  wxCommandEvent& e) {
                        comp->param_on[pd.name] = e.IsChecked();
                        est->Enable(e.IsChecked());
                        db->Enable(e.IsChecked());
                        doc_->dirty = true;
                        if (on_edited) on_edited();
                    });
                } else {
                    auto* lbl = new wxStaticText(
                        this, wxID_ANY,
                        wxString::FromUTF8(pd.name + "  (" + pd.unit +
                                           ")  -- always on"));
                    lbl->SetForegroundColour(wxColour(80, 80, 85));
                    sizer->Add(lbl, 0, wxLEFT | wxTOP, 6);

                    auto* sub = new wxBoxSizer(wxHORIZONTAL);
                    est = new wxTextCtrl(
                        this, wxID_ANY,
                        wxString::FromUTF8(comp->param_text.count(pd.name)
                                               ? comp->param_text.at(pd.name)
                                               : pd.default_text));
                    db = new wxSpinCtrl(
                        this, wxID_ANY, wxEmptyString, wxDefaultPosition,
                        wxDefaultSize, wxSP_ARROW_KEYS, -200, 200,
                        comp->param_db.count(pd.name)
                            ? comp->param_db.at(pd.name)
                            : 0);
                    sub->Add(new wxStaticText(this, wxID_ANY, "est"), 0,
                             wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
                    sub->Add(est, 1, wxEXPAND | wxRIGHT, 4);
                    sub->Add(db, 0, 0);
                    sizer->Add(sub, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);
                }

                est->Bind(wxEVT_TEXT, [comp, pd, est, this](wxCommandEvent&) {
                    comp->param_text[pd.name] = est->GetValue().ToStdString();
                    doc_->dirty = true;
                    if (on_edited) on_edited();
                });
                db->Bind(wxEVT_SPINCTRL, [comp, pd, db, this](wxSpinEvent&) {
                    comp->param_db[pd.name] = db->GetValue();
                    doc_->dirty = true;
                    if (on_edited) on_edited();
                });
            }

            auto* info = new wxStaticText(
                this, wxID_ANY,
                wxString::FromUTF8(syms::kind_display(c->kind)));
            info->SetForegroundColour(wxColour(115, 115, 120));
            sizer->Add(info, 0, wxALL | wxTOP, 8);
        }
    } else if (doc_) {
        auto* t = new wxStaticText(
            this, wxID_ANY,
            "Nothing selected.\n\nPick a component with the Select\ntool to edit its value,\nparasitics and size offsets.");
        t->SetForegroundColour(wxColour(115, 115, 120));
        sizer->Add(t, 0, wxALL, 8);
    }

    sizer->AddStretchSpacer();
    FitInside();
    rebuilding_ = false;
}

void PropertiesPanel::on_analysis_changed(wxCommandEvent&) {}

} // namespace symcirc
