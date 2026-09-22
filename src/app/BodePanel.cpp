#include "BodePanel.h"
#include "core/Eng.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <vector>
#include <wx/dcbuffer.h>
#include <wx/filedlg.h>
#include <wx/msgdlg.h>

namespace symcirc {

wxBEGIN_EVENT_TABLE(BodeCanvas, wxPanel)
    EVT_PAINT(BodeCanvas::on_paint)
wxEND_EVENT_TABLE()

wxBEGIN_EVENT_TABLE(BodePanel, wxPanel)
wxEND_EVENT_TABLE()

namespace {
constexpr double kFstart = 1.0;
constexpr double kFend = 1e8;
constexpr int kN = 400;
} // namespace

BodeCanvas::BodeCanvas(wxWindow* parent) : wxPanel(parent) {
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetBackgroundColour(*wxWHITE);
}

void BodeCanvas::set_result(const syms::AnalysisResult* r) {
    res_ = r;
    Refresh();
}

void BodeCanvas::sample(std::vector<double>& f, std::vector<double>& mag,
                        std::vector<double>& ph) const {
    f.clear();
    mag.clear();
    ph.clear();
    if (!res_) return;
    std::vector<double> freqs = syms::sweep_hz(kFstart, kFend, kN);
    for (double fr : freqs) {
        double w = 2 * M_PI * fr;
        f.push_back(fr);
        mag.push_back(syms::mag_db_at(*res_, w));
        ph.push_back(syms::phase_deg_at(*res_, w));
    }
}

bool BodeCanvas::save_csv(const std::string& path) const {
    std::ofstream o(path, std::ios::binary);
    if (!o) return false;
    std::vector<double> f, mag, ph;
    sample(f, mag, ph);
    o << "freq_hz,mag_db,phase_deg\n";
    char buf[128];
    for (size_t i = 0; i < f.size(); ++i) {
        std::snprintf(buf, sizeof(buf), "%.10g,%.10g,%.10g\n", f[i], mag[i], ph[i]);
        o << buf;
    }
    return bool(o);
}

bool BodeCanvas::save_svg(const std::string& path) const {
    std::vector<double> f, mag, ph;
    sample(f, mag, ph);
    if (f.empty()) return false;

    const int W = 760, Hh = 260, Hp = 200, mL = 60, mR = 20, mT = 20, gap = 40;
    double mag_lo = 1e300, mag_hi = -1e300;
    for (double m : mag)
        if (std::isfinite(m)) { mag_lo = std::min(mag_lo, m); mag_hi = std::max(mag_hi, m); }
    if (mag_lo > mag_hi) { mag_lo = -20; mag_hi = 20; }
    double span = std::max(mag_hi - mag_lo, 1.0);
    mag_lo -= span * 0.08;
    mag_hi += span * 0.08;
    const double ph_lo = -200, ph_hi = 200;
    const double lr = std::log10(kFend / kFstart);

    auto X = [&](double fr) { return mL + std::log10(fr / kFstart) / lr * (W - mL - mR); };
    auto Ym = [&](double db) { return mT + Hh - (db - mag_lo) / (mag_hi - mag_lo) * Hh; };
    auto Yp = [&](double deg) { return mT + Hh + gap + Hp - (deg - ph_lo) / (ph_hi - ph_lo) * Hp; };

    std::ostringstream s;
    int totalH = mT + Hh + gap + Hp + 40;
    s << "<?xml version=\"1.0\"?>\n<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << W
      << "\" height=\"" << totalH << "\" viewBox=\"0 0 " << W << " " << totalH << "\">\n";
    s << "<rect width=\"100%\" height=\"100%\" fill=\"#fff\"/>\n";
    s << "<g font-family=\"Arial\" font-size=\"11\">\n";
    // frames
    s << "<rect x=\"" << mL << "\" y=\"" << mT << "\" width=\"" << (W - mL - mR)
      << "\" height=\"" << Hh << "\" fill=\"none\" stroke=\"#c8c8cf\"/>\n";
    s << "<rect x=\"" << mL << "\" y=\"" << (mT + Hh + gap) << "\" width=\""
      << (W - mL - mR) << "\" height=\"" << Hp << "\" fill=\"none\" stroke=\"#c8c8cf\"/>\n";
    // decade grid
    for (int e = 0; e <= 8; ++e) {
        double fr = std::pow(10.0, e);
        double x = X(fr);
        s << "<line x1=\"" << x << "\" y1=\"" << mT << "\" x2=\"" << x << "\" y2=\""
          << (mT + Hh) << "\" stroke=\"#eeeef2\"/>\n";
        s << "<text x=\"" << (x + 2) << "\" y=\"" << (mT + Hh + gap + Hp + 14)
          << "\" fill=\"#8a8a90\">" << (e == 0 ? "1" : ("1e" + std::to_string(e)))
          << "</text>\n";
    }
    // magnitude curve
    s << "<polyline fill=\"none\" stroke=\"#c82828\" stroke-width=\"1.6\" points=\"";
    for (size_t i = 0; i < f.size(); ++i)
        if (std::isfinite(mag[i])) s << X(f[i]) << "," << Ym(mag[i]) << " ";
    s << "\"/>\n";
    // phase curve
    s << "<polyline fill=\"none\" stroke=\"#285ac8\" stroke-width=\"1.6\" points=\"";
    for (size_t i = 0; i < f.size(); ++i)
        if (std::isfinite(ph[i])) s << X(f[i]) << "," << Yp(ph[i]) << " ";
    s << "\"/>\n";
    s << "<text x=\"" << (mL + 6) << "\" y=\"" << (mT + 16) << "\" fill=\"#c82828\">Magnitude (dB)</text>\n";
    s << "<text x=\"" << (mL + 6) << "\" y=\"" << (mT + Hh + gap + 16) << "\" fill=\"#285ac8\">Phase (deg)</text>\n";
    s << "<text x=\"" << (mL + 6) << "\" y=\"" << (mT + Hh + gap - 6) << "\" fill=\"#666\">"
      << "H(s) = " << escape_xml(res_ ? res_->pruned.text : std::string()) << "</text>\n";
    s << "</g></svg>\n";

    std::ofstream o(path, std::ios::binary);
    if (!o) return false;
    o << s.str();
    return bool(o);
}

std::string BodeCanvas::escape_xml(std::string s) {
    std::string o;
    for (char c : s) {
        if (c == '&') o += "&amp;";
        else if (c == '<') o += "&lt;";
        else if (c == '>') o += "&gt;";
        else o += c;
    }
    return o;
}

void BodeCanvas::on_paint(wxPaintEvent&) {
    wxAutoBufferedPaintDC dc(this);
    wxSize sz = GetClientSize();
    dc.SetBackground(*wxWHITE);
    dc.Clear();

    if (!res_) {
        dc.SetTextForeground(wxColour(150, 150, 155));
        dc.DrawText("Run an analysis (F5) to see the Bode plot.", 12, 12);
        return;
    }

    const int mL = 56, mR = 12, mT = 12, mB = 42;
    const int W = sz.x - mL - mR;
    const int Hh = (sz.y - mT - mB - 14) / 2;
    const int Hp = sz.y - mT - mB - 14;
    if (W < 50 || Hh < 30) return;

    std::vector<double> mag, ph;
    std::vector<double> freqs = syms::sweep_hz(kFstart, kFend, kN);
    double mag_min = 1e300, mag_max = -1e300;
    for (size_t i = 0; i < freqs.size(); ++i) {
        double w = 2 * M_PI * freqs[i];
        double m = syms::mag_db_at(*res_, w);
        double p = syms::phase_deg_at(*res_, w);
        mag.push_back(m);
        ph.push_back(p);
        if (std::isfinite(m)) {
            mag_min = std::min(mag_min, m);
            mag_max = std::max(mag_max, m);
        }
    }
    if (mag_min > mag_max) return;
    double msp = std::max(mag_max - mag_min, 1.0);
    mag_min -= msp * 0.08;
    mag_max += msp * 0.08;
    const double ph_min = -200, ph_max = 200;

    auto x_of = [&](double f) {
        double t = std::log10(f / kFstart) / std::log10(kFend / kFstart);
        return mL + t * W;
    };

    dc.SetPen(wxPen(wxColour(232, 232, 236)));
    dc.SetTextForeground(wxColour(140, 140, 145));
    for (int e = 0; e <= 8; ++e) {
        double f = std::pow(10.0, e);
        int x = int(x_of(f));
        dc.DrawLine(x, mT, x, mT + Hh);
        dc.DrawLine(x, mT + Hh + 14, x, mT + Hh + 14 + Hp);
        wxString lbl = (e == 0) ? "1" : wxString::Format("1e%d", e);
        dc.DrawText(lbl, x + 2, mT + Hh + 14 + Hp + 4);
    }

    auto y_mag = [&](double db) {
        return mT + Hh - (db - mag_min) / (mag_max - mag_min) * Hh;
    };
    dc.SetPen(wxPen(wxColour(210, 210, 215)));
    dc.DrawRectangle(mL, mT, W, Hh);
    int db_step = int(std::max(10.0, std::ceil((mag_max - mag_min) / 5)));
    for (int db = int(std::floor(mag_min / db_step) * db_step); db <= mag_max;
         db += db_step) {
        int y = int(y_mag(db));
        if (y < mT || y > mT + Hh) continue;
        dc.SetPen(wxPen(wxColour(240, 240, 244)));
        dc.DrawLine(mL, y, mL + W, y);
        dc.SetTextForeground(wxColour(140, 140, 145));
        dc.DrawText(wxString::Format("%d dB", db), 4, y - 6);
    }

    auto y_ph = [&](double deg) {
        return mT + Hh + 14 + Hp - (deg - ph_min) / (ph_max - ph_min) * Hp;
    };
    dc.SetPen(wxPen(wxColour(210, 210, 215)));
    dc.DrawRectangle(mL, mT + Hh + 14, W, Hp);
    for (int deg : {-180, -90, 0, 90, 180}) {
        int y = int(y_ph(deg));
        if (y < mT + Hh + 14 || y > mT + Hh + 14 + Hp) continue;
        dc.SetPen(wxPen(deg == 0 ? wxColour(205, 205, 212)
                                 : wxColour(240, 240, 244)));
        dc.DrawLine(mL, y, mL + W, y);
        dc.SetTextForeground(wxColour(140, 140, 145));
        dc.DrawText(wxString::Format("%d\u00b0", deg), 6, y - 6);
    }

    dc.SetPen(wxPen(wxColour(200, 40, 40), 2));
    bool started = false;
    wxPoint prev(0, 0);
    for (size_t i = 0; i < freqs.size(); ++i) {
        if (!std::isfinite(mag[i])) { started = false; continue; }
        wxPoint p(int(x_of(freqs[i])), int(y_mag(mag[i])));
        if (started) dc.DrawLine(prev, p);
        prev = p;
        started = true;
    }
    dc.SetPen(wxPen(wxColour(40, 90, 200), 2));
    started = false;
    for (size_t i = 0; i < freqs.size(); ++i) {
        if (!std::isfinite(ph[i])) { started = false; continue; }
        wxPoint p(int(x_of(freqs[i])), int(y_ph(ph[i])));
        if (started) dc.DrawLine(prev, p);
        prev = p;
        started = true;
    }

    dc.SetTextForeground(wxColour(200, 40, 40));
    dc.DrawText("Magnitude", mL + 6, mT + 4);
    dc.SetTextForeground(wxColour(40, 90, 200));
    dc.DrawText("Phase", mL + 6, mT + Hh + 18);
    dc.SetTextForeground(wxColour(90, 90, 95));
    dc.DrawText("frequency [Hz]", mL + W - 78, mT + Hh + 14 + Hp + 4);
}

// ---------------------------------------------------------------------------
BodePanel::BodePanel(wxWindow* parent) : wxPanel(parent) {
    auto* root = new wxBoxSizer(wxVERTICAL);
    plot_ = new BodeCanvas(this);
    root->Add(plot_, 1, wxEXPAND);

    auto* bar = new wxBoxSizer(wxHORIZONTAL);
    auto* svg = new wxButton(this, wxID_ANY, "Save SVG...");
    auto* csv = new wxButton(this, wxID_ANY, "Save CSV...");
    bar->Add(svg, 0, wxRIGHT, 6);
    bar->Add(csv, 0, 0);
    bar->AddStretchSpacer();
    root->Add(bar, 0, wxEXPAND | wxALL, 4);
    SetSizer(root);

    svg->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        wxFileDialog dlg(this, "Save Bode plot", "", "bode.svg",
                         "SVG (*.svg)|*.svg", wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (dlg.ShowModal() == wxID_OK && !plot_->save_svg(dlg.GetPath().ToStdString()))
            wxMessageBox("Could not write the SVG file.", "Save failed",
                         wxICON_ERROR, this);
    });
    csv->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        wxFileDialog dlg(this, "Save Bode data", "", "bode.csv",
                         "CSV (*.csv)|*.csv", wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (dlg.ShowModal() == wxID_OK && !plot_->save_csv(dlg.GetPath().ToStdString()))
            wxMessageBox("Could not write the CSV file.", "Save failed",
                         wxICON_ERROR, this);
    });
}

void BodePanel::set_result(const syms::AnalysisResult* r) {
    plot_->set_result(r);
    Refresh();
}

} // namespace symcirc
