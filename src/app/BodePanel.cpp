#include "BodePanel.h"
#include "core/Eng.h"
#include "core/Engine.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <vector>
#include <wx/dcbuffer.h>
#include <wx/filedlg.h>
#include <wx/image.h>
#include <wx/msgdlg.h>

namespace symcirc {

wxBEGIN_EVENT_TABLE(BodeCanvas, wxPanel)
    EVT_PAINT(BodeCanvas::on_paint)
wxEND_EVENT_TABLE()

wxBEGIN_EVENT_TABLE(BodePanel, wxPanel)
wxEND_EVENT_TABLE()

namespace {
// Default sweep when no result is loaded yet.
constexpr double kFstart = 1.0;
constexpr double kFend = 1e8;
constexpr int kMaxPoints = 4000;
} // namespace

// The frequency band to plot: the analysis result's sweep, or a default.
void BodeCanvas::band(double& f0, double& f1, int& n) const {
    if (res_ && res_->sweep.f_stop_hz > res_->sweep.f_start_hz) {
        f0 = res_->sweep.f_start_hz;
        f1 = res_->sweep.f_stop_hz;
        // keep the on-screen point count bounded for responsiveness
        n = std::min(kMaxPoints, std::max(50, res_->sweep.points_per_interval * 10));
    } else {
        f0 = kFstart;
        f1 = kFend;
        n = 400;
    }
}

BodeCanvas::BodeCanvas(wxWindow* parent) : wxPanel(parent) {
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetBackgroundColour(*wxWHITE);
}

void BodeCanvas::set_result(const syms::AnalysisResult* r) {
    res_ = r;
    Refresh();
}

void BodeCanvas::set_mode(PlotMode m) {
    mode_ = m;
    Refresh();
}

void BodeCanvas::sample(std::vector<double>& f, std::vector<double>& mag,
                        std::vector<double>& ph) const {
    f.clear();
    mag.clear();
    ph.clear();
    if (!res_) return;
    double f0, f1;
    int n;
    band(f0, f1, n);
    std::vector<double> freqs = syms::sweep_hz(f0, f1, n);
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
    double f0, f1;
    int npts;
    band(f0, f1, npts);
    const double lr = std::log10(f1 / f0);

    auto X = [&](double fr) { return mL + std::log10(fr / f0) / lr * (W - mL - mR); };
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
    // decade grid lines within the band
    {
        int e0 = int(std::floor(std::log10(f0) - 1e-9));
        int e1 = int(std::ceil(std::log10(f1) + 1e-9));
        for (int e = e0; e <= e1; ++e) {
            double fr = std::pow(10.0, e);
            if (fr < f0 * 0.999 || fr > f1 * 1.001) continue;
            double x = X(fr);
            s << "<line x1=\"" << x << "\" y1=\"" << mT << "\" x2=\"" << x << "\" y2=\""
              << (mT + Hh) << "\" stroke=\"#eeeef2\"/>\n";
            s << "<text x=\"" << (x + 2) << "\" y=\"" << (mT + Hh + gap + Hp + 14)
              << "\" fill=\"#8a8a90\">" << syms::eng::format_eng(fr, 1) << "</text>\n";
        }
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

bool BodeCanvas::save_png(const std::string& path) const {
    wxSize sz = GetClientSize();
    if (sz.x < 50 || sz.y < 50) sz = wxSize(900, 640);
    wxBitmap bmp(sz.x, sz.y, 24);
    {
        wxMemoryDC dc(bmp);
        dc.SetBackground(wxBrush(*wxWHITE));
        dc.Clear();
        switch (mode_) {
            case PlotMode::Bode: paint_bode(dc, sz); break;
            case PlotMode::Nyquist: paint_nyquist(dc, sz); break;
            case PlotMode::Nichols: paint_nichols(dc, sz); break;
        }
    }
    wxImage img = bmp.ConvertToImage();
    return img.SaveFile(wxString::FromUTF8(path), wxBITMAP_TYPE_PNG);
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

// ---------------------------------------------------------------------------
// painting
// ---------------------------------------------------------------------------
void BodeCanvas::on_paint(wxPaintEvent&) {
    wxAutoBufferedPaintDC dc(this);
    wxSize sz = GetClientSize();
    dc.SetBackground(*wxWHITE);
    dc.Clear();

    if (!res_) {
        dc.SetTextForeground(wxColour(150, 150, 155));
        dc.DrawText("Run an analysis (F5) to see the plot.", 12, 12);
        return;
    }
    switch (mode_) {
        case PlotMode::Bode: paint_bode(dc, sz); break;
        case PlotMode::Nyquist: paint_nyquist(dc, sz); break;
        case PlotMode::Nichols: paint_nichols(dc, sz); break;
    }
}

namespace {
// shared: sample magnitude/phase then give the caller the raw re/im too
struct Curve {
    std::vector<double> f, mag, ph, re, im;
};
void sample_curve(const syms::AnalysisResult* r, Curve& c,
                  const BodeCanvas& cv) {
    double f0, f1;
    int n;
    cv.band(f0, f1, n);
    std::vector<double> freqs = syms::sweep_hz(f0, f1, n);
    for (double fr : freqs) {
        double w = 2 * M_PI * fr;
        auto z = syms::eval_complex((r->num_raw / r->den_raw).normal(),
                                    r->params, w);
        c.f.push_back(fr);
        c.mag.push_back(std::isfinite(z.real())
                            ? 20.0 * std::log10(std::hypot(z.real(), z.imag()))
                            : -1e300);
        c.ph.push_back(std::atan2(z.imag(), z.real()) * 180.0 / M_PI);
        c.re.push_back(z.real());
        c.im.push_back(z.imag());
    }
}
} // namespace

void BodeCanvas::paint_bode(wxDC& dc, const wxSize& sz) const {
    const int mL = 56, mR = 12, mT = 12, mB = 42;
    const int W = sz.x - mL - mR;
    const int Hh = (sz.y - mT - mB - 14) / 2;
    const int Hp = sz.y - mT - mB - 14;
    if (W < 50 || Hh < 30) return;

    double f_lo, f_hi;
    int npts;
    band(f_lo, f_hi, npts);
    std::vector<double> mag, ph;
    std::vector<double> freqs = syms::sweep_hz(f_lo, f_hi, npts);
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

    double loglo = std::log10(f_lo), loghi = std::log10(f_hi);
    auto x_of = [&](double f) {
        double t = (std::log10(f) - loglo) / (loghi - loglo);
        return mL + t * W;
    };

    dc.SetPen(wxPen(wxColour(232, 232, 236)));
    dc.SetTextForeground(wxColour(140, 140, 145));
    {
        int e0 = int(std::floor(loglo - 1e-9));
        int e1 = int(std::ceil(loghi + 1e-9));
        for (int e = e0; e <= e1; ++e) {
            double f = std::pow(10.0, e);
            if (f < f_lo * 0.999 || f > f_hi * 1.001) continue;
            int x = int(x_of(f));
            dc.DrawLine(x, mT, x, mT + Hh);
            dc.DrawLine(x, mT + Hh + 14, x, mT + Hh + 14 + Hp);
            wxString lbl = wxString::FromUTF8(syms::eng::format_eng(f, 1));
            dc.DrawText(lbl, x + 2, mT + Hh + 14 + Hp + 4);
        }
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

void BodeCanvas::paint_nyquist(wxDC& dc, const wxSize& sz) const {
    Curve c;
    sample_curve(res_, c, *this);
    if (c.f.empty()) return;

    // auto-scale the real/imag extents with a little margin
    double lo = 1e300, hi = -1e300;
    for (size_t i = 0; i < c.f.size(); ++i) {
        if (!std::isfinite(c.re[i]) || !std::isfinite(c.im[i])) continue;
        lo = std::min({lo, c.re[i], c.im[i]});
        hi = std::max({hi, c.re[i], c.im[i]});
    }
    if (lo > hi) return;
    double span = std::max(hi - lo, 1e-12);
    lo -= span * 0.08;
    hi += span * 0.08;

    const int mL = 48, mR = 16, mT = 16, mB = 30;
    int W = sz.x - mL - mR, H = sz.y - mT - mB;
    if (W < 50 || H < 50) return;
    auto X = [&](double v) { return mL + (v - lo) / (hi - lo) * W; };
    auto Y = [&](double v) { return mT + H - (v - lo) / (hi - lo) * H; };

    dc.SetPen(wxPen(wxColour(215, 215, 220)));
    dc.DrawRectangle(mL, mT, W, H);
    dc.SetPen(wxPen(wxColour(160, 160, 168)));
    dc.DrawLine(int(X(0)), mT, int(X(0)), mT + H);
    dc.DrawLine(mL, int(Y(0)), mL + W, int(Y(0)));
    dc.SetTextForeground(wxColour(130, 130, 138));
    dc.DrawText("Re", mL + W - 18, int(Y(0)) - 16);
    dc.DrawText("Im", int(X(0)) + 4, mT + 4);
    char b1[48], b2[48];
    std::snprintf(b1, sizeof(b1), "%.3g", hi);
    std::snprintf(b2, sizeof(b2), "%.3g", lo);
    dc.DrawText(b1, mL + W - 46, mT + H + 4);
    dc.DrawText(b2, mL, mT + H + 4);

    dc.SetPen(wxPen(wxColour(190, 40, 40), 2));
    bool started = false;
    wxPoint prev(0, 0);
    for (size_t i = 0; i < c.f.size(); ++i) {
        if (!std::isfinite(c.re[i]) || !std::isfinite(c.im[i])) {
            started = false;
            continue;
        }
        wxPoint p(int(X(c.re[i])), int(Y(c.im[i])));
        if (started) dc.DrawLine(prev, p);
        prev = p;
        started = true;
    }
    dc.SetTextForeground(wxColour(190, 40, 40));
    dc.DrawText("Nyquist: Im vs Re of H(jw)", mL + 6, mT + 4);
}

void BodeCanvas::paint_nichols(wxDC& dc, const wxSize& sz) const {
    Curve c;
    sample_curve(res_, c, *this);
    if (c.f.empty()) return;

    // x: phase (deg), y: magnitude (dB), both auto-scaled
    double plo = 1e300, phi = -1e300, mlo = 1e300, mhi = -1e300;
    for (size_t i = 0; i < c.f.size(); ++i) {
        if (!std::isfinite(c.mag[i]) || !std::isfinite(c.ph[i])) continue;
        plo = std::min(plo, c.ph[i]);
        phi = std::max(phi, c.ph[i]);
        mlo = std::min(mlo, c.mag[i]);
        mhi = std::max(mhi, c.mag[i]);
    }
    if (plo > phi || mlo > mhi) return;
    plo -= 10;
    phi += 10;
    if (mhi - mlo < 1.0) mhi = mlo + 1.0;
    mlo -= 5;
    mhi += 5;

    const int mL = 56, mR = 16, mT = 16, mB = 34;
    int W = sz.x - mL - mR, H = sz.y - mT - mB;
    if (W < 50 || H < 50) return;
    auto X = [&](double deg) { return mL + (deg - plo) / (phi - plo) * W; };
    auto Y = [&](double db) { return mT + H - (db - mlo) / (mhi - mlo) * H; };

    dc.SetPen(wxPen(wxColour(215, 215, 220)));
    dc.DrawRectangle(mL, mT, W, H);
    for (int deg = int(std::ceil(plo / 45) * 45); deg <= phi; deg += 45) {
        int x = int(X(deg));
        dc.SetPen(wxPen(deg == 0 ? wxColour(205, 205, 212)
                                 : wxColour(240, 240, 244)));
        dc.DrawLine(x, mT, x, mT + H);
        dc.SetTextForeground(wxColour(130, 130, 138));
        dc.DrawText(wxString::Format("%d\u00b0", deg), x - 8, mT + H + 4);
    }
    for (int db = int(std::ceil(mlo / 20) * 20); db <= mhi; db += 20) {
        int y = int(Y(db));
        dc.SetPen(wxPen(wxColour(240, 240, 244)));
        dc.DrawLine(mL, y, mL + W, y);
        dc.SetTextForeground(wxColour(130, 130, 138));
        dc.DrawText(wxString::Format("%d dB", db), 4, y - 6);
    }

    dc.SetPen(wxPen(wxColour(40, 110, 60), 2));
    bool started = false;
    wxPoint prev(0, 0);
    for (size_t i = 0; i < c.f.size(); ++i) {
        if (!std::isfinite(c.mag[i]) || !std::isfinite(c.ph[i])) {
            started = false;
            continue;
        }
        wxPoint p(int(X(c.ph[i])), int(Y(c.mag[i])));
        if (started) dc.DrawLine(prev, p);
        prev = p;
        started = true;
    }
    dc.SetTextForeground(wxColour(40, 110, 60));
    dc.DrawText("Nichols: |H| (dB) vs phase", mL + 6, mT + 4);
    dc.SetTextForeground(wxColour(90, 90, 95));
    dc.DrawText("phase [deg]", mL + W - 70, mT + H + 4);
}

// ---------------------------------------------------------------------------
BodePanel::BodePanel(wxWindow* parent) : wxPanel(parent) {
    auto* root = new wxBoxSizer(wxVERTICAL);
    plot_ = new BodeCanvas(this);
    root->Add(plot_, 1, wxEXPAND);

    auto* bar = new wxBoxSizer(wxHORIZONTAL);
    auto* mode = new wxChoice(this, wxID_ANY);
    mode->Append("Bode");
    mode->Append("Nyquist");
    mode->Append("Nichols");
    mode->SetSelection(0);
    auto* svg = new wxButton(this, wxID_ANY, "Save SVG...");
    auto* png = new wxButton(this, wxID_ANY, "Save PNG...");
    auto* csv = new wxButton(this, wxID_ANY, "Save CSV...");
    bar->Add(new wxStaticText(this, wxID_ANY, "Plot:"), 0,
             wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    bar->Add(mode, 0, wxRIGHT, 8);
    bar->Add(svg, 0, wxRIGHT, 6);
    bar->Add(png, 0, wxRIGHT, 6);
    bar->Add(csv, 0, 0);
    bar->AddStretchSpacer();
    root->Add(bar, 0, wxEXPAND | wxALL, 4);
    SetSizer(root);

    mode->Bind(wxEVT_CHOICE, [this, mode](wxCommandEvent&) {
        switch (mode->GetSelection()) {
            case 1: plot_->set_mode(PlotMode::Nyquist); break;
            case 2: plot_->set_mode(PlotMode::Nichols); break;
            default: plot_->set_mode(PlotMode::Bode); break;
        }
    });

    svg->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        wxFileDialog dlg(this, "Save plot", "", "plot.svg",
                         "SVG (*.svg)|*.svg", wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (dlg.ShowModal() == wxID_OK && !plot_->save_svg(dlg.GetPath().ToStdString()))
            wxMessageBox("Could not write the SVG file.", "Save failed",
                         wxICON_ERROR, this);
    });
    png->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        wxFileDialog dlg(this, "Save plot", "", "plot.png",
                         "PNG (*.png)|*.png", wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (dlg.ShowModal() == wxID_OK && !plot_->save_png(dlg.GetPath().ToStdString()))
            wxMessageBox("Could not write the PNG file.", "Save failed",
                         wxICON_ERROR, this);
    });
    csv->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        wxFileDialog dlg(this, "Save plot data", "", "plot.csv",
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
