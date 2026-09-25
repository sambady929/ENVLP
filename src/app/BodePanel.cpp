// BodePanel: Bode (magnitude & phase), Nyquist, and Nichols plot canvas +
// toolbar with axis-range spin controls and major/minor decade + dB grid.
#include "BodePanel.h"
#include "core/Eng.h"
#include "core/Engine.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <utility>
#include <vector>
#include <wx/dcbuffer.h>
#include <wx/filedlg.h>
#include <wx/image.h>
#include <wx/msgdlg.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace symcirc {

wxBEGIN_EVENT_TABLE(BodeCanvas, wxPanel)
    EVT_PAINT(BodeCanvas::on_paint)
    EVT_SIZE(BodeCanvas::on_size)
wxEND_EVENT_TABLE()

namespace {
// Default sweep when no result is loaded yet.
constexpr double kFstart = 1.0;
constexpr double kFend = 1e8;
constexpr int kMaxPoints = 4000;

// Continuous phase unwrap. The input is in DEGREES (atan2 output in
// (-180, 180]), so the jump threshold is 180 deg and the correction 360 deg.
// The previous version used M_PI (radians) as the threshold, so every >3 deg
// step got "unwrapped" by 360 -- the phase curve was a staircase of garbage
// bearing no relation to the transfer function.
std::vector<double> unwrap_phase(const std::vector<double>& phase_in) {
    std::vector<double> out(phase_in.size(), 0.0);
    if (phase_in.empty()) return out;
    out[0] = phase_in[0];
    double prev = phase_in[0];
    for (size_t i = 1; i < phase_in.size(); ++i) {
        double cur = phase_in[i];
        double d = cur - prev;
        while (d > 180.0) { cur -= 360.0; d -= 360.0; }
        while (d < -180.0) { cur += 360.0; d += 360.0; }
        out[i] = cur;
        prev = cur;
    }
    return out;
}
} // namespace

// The frequency band to plot: the analysis result's sweep, or a default.
void BodeCanvas::band(double& f0, double& f1, int& n) const {
    if (res_ && res_->sweep.f_stop_hz > res_->sweep.f_start_hz) {
        f0 = res_->sweep.f_start_hz;
        f1 = res_->sweep.f_stop_hz;
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

void BodeCanvas::set_x_range(double lo, double hi) {
    x_lo_ = std::max(1e-12, lo);
    x_hi_ = std::max(x_lo_ * 1.0001, hi);
    auto_range_ = false;
    Refresh();
}

void BodeCanvas::set_y_range(double lo, double hi) {
    y_lo_ = lo;
    y_hi_ = std::max(y_lo_ + 0.1, hi);
    auto_range_ = false;
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
    std::vector<double> raw_ph;
    raw_ph.reserve(freqs.size());
    for (double fr : freqs) {
        double w = 2 * M_PI * fr;
        f.push_back(fr);
        mag.push_back(syms::mag_db_at(*res_, w));
        raw_ph.push_back(syms::phase_deg_at(*res_, w));
    }
    ph = unwrap_phase(raw_ph);
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
    double ph_lo = -200, ph_hi = 200;
    if (!ph.empty()) {
        double pmin = 1e300, pmax = -1e300;
        for (double p : ph) if (std::isfinite(p)) {
            pmin = std::min(pmin, p); pmax = std::max(pmax, p);
        }
        if (pmin <= pmax) { ph_lo = pmin - 10; ph_hi = pmax + 10; }
    }
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
    if (!title_.empty())
        s << "<text x=\"" << mL << "\" y=\"" << (mT - 4) << "\" font-size=\"13\" fill=\"#222\">"
          << escape_xml(title_) << "</text>\n";
    s << "<rect x=\"" << mL << "\" y=\"" << mT << "\" width=\"" << (W - mL - mR)
      << "\" height=\"" << Hh << "\" fill=\"none\" stroke=\"#c8c8cf\"/>\n";
    s << "<rect x=\"" << mL << "\" y=\"" << (mT + Hh + gap) << "\" width=\""
      << (W - mL - mR) << "\" height=\"" << Hp << "\" fill=\"none\" stroke=\"#c8c8cf\"/>\n";
    // Major + minor decade lines
    {
        int e0 = int(std::floor(std::log10(f0) - 1e-9));
        int e1 = int(std::ceil(std::log10(f1) + 1e-9));
        for (int e = e0; e <= e1; ++e) {
            double fr = std::pow(10.0, e);
            if (fr < f0 * 0.999 || fr > f1 * 1.001) continue;
            double x = X(fr);
            s << "<line x1=\"" << x << "\" y1=\"" << mT << "\" x2=\"" << x << "\" y2=\""
              << (mT + Hh) << "\" stroke=\"#d8d8e0\"/>\n";
            s << "<text x=\"" << (x + 2) << "\" y=\"" << (mT + Hh + gap + Hp + 14)
              << "\" fill=\"#666\">" << syms::eng::format_eng(fr, 1) << "</text>\n";
            // minor lines at 2..9 within this decade
            for (int k = 2; k <= 9; ++k) {
                double fr2 = fr * k;
                if (fr2 < f0 || fr2 > f1) continue;
                double x2 = X(fr2);
                s << "<line x1=\"" << x2 << "\" y1=\"" << mT << "\" x2=\"" << x2 << "\" y2=\""
                  << (mT + Hh) << "\" stroke=\"#eeeef2\"/>\n";
            }
        }
    }
    // magnitude horizontal grid
    {
        double step = 10.0;
        if (mag_hi - mag_lo > 200) step = 50;
        else if (mag_hi - mag_lo > 80) step = 20;
        int db = int(std::floor(mag_lo / step) * int(step));
        for (; db <= mag_hi; db += int(step)) {
            double y = Ym(db);
            s << "<line x1=\"" << mL << "\" y1=\"" << y << "\" x2=\"" << (W - mR) << "\" y2=\""
              << y << "\" stroke=\"#eeeef2\"/>\n";
            s << "<text x=\"" << 4 << "\" y=\"" << (y - 2) << "\" fill=\"#8a8a90\">"
              << db << " dB</text>\n";
        }
    }
    // phase horizontal grid
    {
        int step = 45;
        int deg = int(std::ceil(ph_lo / step)) * step;
        for (; deg <= ph_hi; deg += step) {
            double y = Yp(deg);
            s << "<line x1=\"" << mL << "\" y1=\"" << y << "\" x2=\"" << (W - mR) << "\" y2=\""
              << y << "\" stroke=\"#eeeef2\"/>\n";
            s << "<text x=\"" << 4 << "\" y=\"" << (y - 2) << "\" fill=\"#8a8a90\">"
              << deg << "\u00b0</text>\n";
        }
    }
    s << "<polyline fill=\"none\" stroke=\"#c82828\" stroke-width=\"1.6\" points=\"";
    for (size_t i = 0; i < f.size(); ++i)
        if (std::isfinite(mag[i])) s << X(f[i]) << "," << Ym(mag[i]) << " ";
    s << "\"/>\n";
    s << "<polyline fill=\"none\" stroke=\"#285ac8\" stroke-width=\"1.6\" points=\"";
    for (size_t i = 0; i < f.size(); ++i)
        if (std::isfinite(ph[i])) s << X(f[i]) << "," << Yp(ph[i]) << " ";
    s << "\"/>\n";
    s << "<text x=\"" << (mL + 6) << "\" y=\"" << (mT + 16) << "\" fill=\"#c82828\">Magnitude (dB)</text>\n";
    s << "<text x=\"" << (mL + 6) << "\" y=\"" << (mT + Hh + gap + 16) << "\" fill=\"#285ac8\">Phase (deg)</text>\n";
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

void BodeCanvas::on_size(wxSizeEvent& e) {
    // Re-render at the new client size so the plot scales with the window.
    Refresh();
    e.Skip();
}

namespace {
// shared: sample magnitude/phase then give the caller the raw re/im too,
// with phase unwrapped so the curve is continuous across +/-pi boundaries.
struct Curve {
    std::vector<double> f, mag, ph, re, im;
};
void sample_curve(const syms::AnalysisResult* r, Curve& c,
                  const BodeCanvas& cv) {
    double f0, f1;
    int n;
    cv.band(f0, f1, n);
    std::vector<double> freqs = syms::sweep_hz(f0, f1, n);
    std::vector<double> raw_ph;
    raw_ph.reserve(freqs.size());
    for (double fr : freqs) {
        double w = 2 * M_PI * fr;
        auto z = syms::eval_complex((r->num_raw / r->den_raw).normal(),
                                    r->params, w);
        c.f.push_back(fr);
        c.mag.push_back(std::isfinite(z.real())
                            ? 20.0 * std::log10(std::hypot(z.real(), z.imag()))
                            : -1e300);
        raw_ph.push_back(std::atan2(z.imag(), z.real()) * 180.0 / M_PI);
        c.re.push_back(z.real());
        c.im.push_back(z.imag());
    }
    c.ph = unwrap_phase(raw_ph);
}
} // namespace

// Compute the data limits for Bode (and Nichols) given the canvas's
// auto/manual range settings. Pulls from the data unless `auto_range_`
// is false, in which case the manual range is honoured.
void compute_axes(const syms::AnalysisResult& res, const Curve& c,
                  bool auto_range, double x_lo_in, double x_hi_in,
                  double y_lo_in, double y_hi_in,
                  double& f_lo, double& f_hi, double& mag_lo,
                  double& mag_hi, double& ph_lo, double& ph_hi) {
    if (auto_range) {
        // X = frequency band of the result.
        if (res.sweep.f_stop_hz > res.sweep.f_start_hz) {
            f_lo = res.sweep.f_start_hz;
            f_hi = res.sweep.f_stop_hz;
        } else {
            f_lo = 1.0; f_hi = 1e8;
        }
        // Y magnitude auto.
        double mlo = 1e300, mhi = -1e300;
        for (double m : c.mag)
            if (std::isfinite(m)) { mlo = std::min(mlo, m); mhi = std::max(mhi, m); }
        if (mlo > mhi) { mlo = -20; mhi = 20; }
        double sp = std::max(mhi - mlo, 1.0);
        mlo -= sp * 0.08; mhi += sp * 0.08;
        mag_lo = mlo; mag_hi = mhi;
        // Phase auto from unwrapped samples.
        double plo = 1e300, phi = -1e300;
        for (double p : c.ph)
            if (std::isfinite(p)) { plo = std::min(plo, p); phi = std::max(phi, p); }
        if (plo > phi) { plo = -180; phi = 180; }
        ph_lo = plo - 10; ph_hi = phi + 10;
    } else {
        f_lo = std::max(1e-12, x_lo_in);
        f_hi = std::max(f_lo * 1.0001, x_hi_in);
        mag_lo = y_lo_in; mag_hi = y_hi_in;
        // Phase keeps a sensible default; user can't set it directly yet.
        ph_lo = -200; ph_hi = 200;
    }
}

void BodeCanvas::paint_bode(wxDC& dc, const wxSize& sz) const {
    const int mL = 72, mR = 16;
    const int top = 34;    // y of the magnitude plot's top (title + label above)
    const int gap = 20;    // gap between the two plots, holds the phase label
    const int bottom = 44; // bottom margin for the frequency tick labels
    const int W = sz.x - mL - mR;
    const int Hh = (sz.y - top - gap - bottom) / 2;
    const int Hp = Hh;
    if (W < 50 || Hh < 30) return;

    Curve c;
    sample_curve(res_, c, *this);
    if (c.f.empty()) return;

    double f_lo, f_hi, mag_lo, mag_hi, ph_lo, ph_hi;
    compute_axes(*res_, c, auto_range_, x_lo_, x_hi_, y_lo_, y_hi_,
                 f_lo, f_hi, mag_lo, mag_hi, ph_lo, ph_hi);

    // Title, then the axis labels above each figure.
    if (!title_.empty()) {
        dc.SetTextForeground(wxColour(40, 40, 45));
        wxFont bold = dc.GetFont(); bold.SetWeight(wxFONTWEIGHT_BOLD);
        dc.SetFont(bold);
        dc.DrawText(wxString::FromUTF8(title_), mL, 2);
        dc.SetFont(wxNullFont);
    }
    dc.SetTextForeground(wxColour(200, 40, 40));
    dc.DrawText("Magnitude (dB)", mL, top - 16);
    dc.SetTextForeground(wxColour(40, 90, 200));
    dc.DrawText("Phase (deg)", mL, top + Hh + gap - 16);

    double loglo = std::log10(f_lo), loghi = std::log10(f_hi);
    auto x_of = [&](double f) {
        double t = (std::log10(f) - loglo) / (loghi - loglo);
        return mL + t * W;
    };
    auto y_mag = [&](double db) {
        return top + Hh - (db - mag_lo) / (mag_hi - mag_lo) * Hh;
    };
    auto y_ph = [&](double deg) {
        return top + Hh + gap + Hp - (deg - ph_lo) / (ph_hi - ph_lo) * Hp;
    };

    // ---- vertical grid lines (major decades + minor 2..9 within each) ----
    int e0 = int(std::floor(loglo - 1e-9));
    int e1 = int(std::ceil(loghi + 1e-9));
    for (int e = e0; e <= e1; ++e) {
        double f = std::pow(10.0, e);
        if (f < f_lo * 0.999 || f > f_hi * 1.001) continue;
        int x = int(x_of(f));
        if (show_major_grid_) {
            dc.SetPen(wxPen(wxColour(178, 184, 196)));
            dc.DrawLine(x, top, x, top + Hh);
            dc.DrawLine(x, top + Hh + gap, x, top + Hh + gap + Hp);
        }
        dc.SetTextForeground(wxColour(110, 110, 118));
        wxString lbl = wxString::FromUTF8(syms::eng::format_eng(f, 1));
        dc.DrawText(lbl, x + 2, top + Hh + gap + Hp + 4);
        if (show_minor_grid_) {
            dc.SetPen(wxPen(wxColour(232, 234, 240)));
            for (int k = 2; k <= 9; ++k) {
                double f2 = f * k;
                if (f2 < f_lo || f2 > f_hi) continue;
                int x2 = int(x_of(f2));
                dc.DrawLine(x2, top, x2, top + Hh);
                dc.DrawLine(x2, top + Hh + gap, x2, top + Hh + gap + Hp);
            }
        }
    }

    // ---- magnitude horizontal grid + frame ----
    dc.SetPen(wxPen(wxColour(200, 202, 210)));
    dc.DrawRectangle(mL, top, W, Hh);
    double db_step = 20.0;
    if (mag_hi - mag_lo > 200) db_step = 50;
    else if (mag_hi - mag_lo > 80) db_step = 20;
    else db_step = 10;
    int db = int(std::floor(mag_lo / db_step) * int(db_step));
    for (; db <= mag_hi; db += int(db_step)) {
        int y = int(y_mag(db));
        if (y < top || y > top + Hh) continue;
        if (show_major_grid_) {
            dc.SetPen(wxPen(wxColour(230, 232, 238)));
            dc.DrawLine(mL, y, mL + W, y);
        }
        dc.SetTextForeground(wxColour(110, 110, 118));
        dc.DrawText(wxString::Format("%+d dB", db), 4, y - 6);
    }

    // ---- phase horizontal grid + frame ----
    dc.SetPen(wxPen(wxColour(200, 202, 210)));
    dc.DrawRectangle(mL, top + Hh + gap, W, Hp);
    int ph_step = 45;
    if (ph_hi - ph_lo > 720) ph_step = 90;
    else if (ph_hi - ph_lo > 240) ph_step = 60;
    else ph_step = 30;
    int deg = int(std::ceil(ph_lo / ph_step)) * ph_step;
    for (; deg <= ph_hi; deg += ph_step) {
        int y = int(y_ph(deg));
        if (y < top + Hh + gap || y > top + Hh + gap + Hp) continue;
        if (show_major_grid_) {
            dc.SetPen(wxPen(deg == 0 ? wxColour(196, 198, 208)
                                     : wxColour(230, 232, 238)));
            dc.DrawLine(mL, y, mL + W, y);
        }
        dc.SetTextForeground(wxColour(110, 110, 118));
        dc.DrawText(wxString::Format("%d", deg) + wxString(wxUniChar(0x00B0)),
                    6, y - 6);
    }

    // magnitude curve
    dc.SetPen(wxPen(wxColour(200, 40, 40), 2));
    {
        bool started = false;
        wxPoint prev(0, 0);
        for (size_t i = 0; i < c.f.size(); ++i) {
            if (!std::isfinite(c.mag[i])) { started = false; continue; }
            wxPoint p(int(x_of(c.f[i])), int(y_mag(c.mag[i])));
            if (started) dc.DrawLine(prev, p);
            prev = p;
            started = true;
        }
    }
    // phase curve (already unwrapped by sample_curve)
    dc.SetPen(wxPen(wxColour(40, 90, 200), 2));
    {
        bool started = false;
        wxPoint prev(0, 0);
        for (size_t i = 0; i < c.f.size(); ++i) {
            if (!std::isfinite(c.ph[i])) { started = false; continue; }
            wxPoint p(int(x_of(c.f[i])), int(y_ph(c.ph[i])));
            if (started) dc.DrawLine(prev, p);
            prev = p;
            started = true;
        }
    }

    dc.SetTextForeground(wxColour(90, 90, 98));
    {
        wxString fl = "Frequency (Hz)";
        wxSize fs = dc.GetTextExtent(fl);
        dc.DrawText(fl, mL + (W - fs.x) / 2, top + Hh + gap + Hp + 22);
    }
}

void BodeCanvas::paint_nyquist(wxDC& dc, const wxSize& sz) const {
    Curve c;
    sample_curve(res_, c, *this);
    if (c.f.empty()) return;

    // Symmetric auto-scale that always includes the critical point -1 + j0
    // (the Nyquist stability criterion is about encircling it), plus a small
    // margin around the data.
    double maxr = 0.0;
    for (size_t i = 0; i < c.f.size(); ++i) {
        if (!std::isfinite(c.re[i]) || !std::isfinite(c.im[i])) continue;
        maxr = std::max(maxr, std::fabs(c.re[i]));
        maxr = std::max(maxr, std::fabs(c.im[i]));
    }
    double lim = std::max(maxr * 1.15, 1.25);
    double lo = -lim, hi = lim;

    const int mL = 56, mR = 16, mT = 16, mB = 30;
    int W = sz.x - mL - mR, H = sz.y - mT - mB;
    if (W < 50 || H < 50) return;
    auto X = [&](double v) { return mL + (v - lo) / (hi - lo) * W; };
    auto Y = [&](double v) { return mT + H - (v - lo) / (hi - lo) * H; };

    if (!title_.empty()) {
        dc.SetTextForeground(wxColour(40, 40, 45));
        wxFont bold = dc.GetFont(); bold.SetWeight(wxFONTWEIGHT_BOLD);
        dc.SetFont(bold);
        dc.DrawText(wxString::FromUTF8(title_), mL, mT - 2);
        dc.SetFont(wxNullFont);
    }

    dc.SetPen(wxPen(wxColour(225, 225, 230)));
    dc.DrawRectangle(mL, mT, W, H);
    // grid lines at 1/5 intervals
    dc.SetPen(wxPen(wxColour(240, 240, 244)));
    for (int k = 1; k < 5; ++k) {
        double v = lo + (hi - lo) * k / 5;
        dc.DrawLine(int(X(v)), mT, int(X(v)), mT + H);
        dc.DrawLine(mL, int(Y(v)), mL + W, int(Y(v)));
    }
    // axes through the origin
    dc.SetPen(wxPen(wxColour(160, 160, 168)));
    dc.DrawLine(int(X(0)), mT, int(X(0)), mT + H);
    dc.DrawLine(mL, int(Y(0)), mL + W, int(Y(0)));
    // the critical point -1 + j0, drawn as a marker
    int cx = int(X(-1)), cy = int(Y(0));
    dc.SetPen(wxPen(wxColour(220, 20, 20), 1));
    dc.SetBrush(wxBrush(wxColour(220, 20, 20)));
    dc.DrawCircle(cx, cy, 3);
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    dc.SetTextForeground(wxColour(200, 20, 20));
    dc.DrawText("-1", cx - 4, cy + 4);
    dc.SetTextForeground(wxColour(110, 110, 118));
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

    // Auto-zoom to the full curve: phase on x, magnitude on y, with margin.
    double nx_lo = 1e300, nx_hi = -1e300, ny_lo = 1e300, ny_hi = -1e300;
    for (size_t i = 0; i < c.f.size(); ++i) {
        if (!std::isfinite(c.mag[i]) || !std::isfinite(c.ph[i])) continue;
        nx_lo = std::min(nx_lo, c.ph[i]);
        nx_hi = std::max(nx_hi, c.ph[i]);
        ny_lo = std::min(ny_lo, c.mag[i]);
        ny_hi = std::max(ny_hi, c.mag[i]);
    }
    if (nx_lo > nx_hi || ny_lo > ny_hi) return;
    double phspan = std::max(nx_hi - nx_lo, 20.0);
    double mspan = std::max(ny_hi - ny_lo, 20.0);
    nx_lo -= phspan * 0.06; nx_hi += phspan * 0.06;
    ny_lo -= mspan * 0.08; ny_hi += mspan * 0.08;

    const int mL = 56, mR = 16, mT = 16, mB = 34;
    int W = sz.x - mL - mR, H = sz.y - mT - mB;
    if (W < 50 || H < 50) return;
    auto X = [&](double deg) { return mL + (deg - nx_lo) / (nx_hi - nx_lo) * W; };
    auto Y = [&](double db) { return mT + H - (db - ny_lo) / (ny_hi - ny_lo) * H; };

    if (!title_.empty()) {
        dc.SetTextForeground(wxColour(40, 40, 45));
        wxFont bold = dc.GetFont(); bold.SetWeight(wxFONTWEIGHT_BOLD);
        dc.SetFont(bold);
        dc.DrawText(wxString::FromUTF8(title_), mL, mT - 2);
        dc.SetFont(wxNullFont);
    }

    dc.SetPen(wxPen(wxColour(225, 225, 230)));
    dc.DrawRectangle(mL, mT, W, H);

    // The Nichols chart grid: constant closed-loop magnitude (M) contours and
    // constant closed-loop phase (N) contours, not a plain Cartesian grid.
    // The chart lines are drawn faint; the data curve is drawn on top.
    auto chart_pen = [&](bool emphasized) {
        return wxPen(emphasized ? wxColour(160, 168, 188)
                                : wxColour(210, 214, 226));
    };
    auto draw_contour = [&](std::vector<std::pair<double, double>>& pts) {
        bool started = false;
        wxPoint prev(0, 0);
        for (auto& q : pts) {
            double db = 20.0 * std::log10(std::max(q.second, 1e-9));
            if (db < ny_lo || db > ny_hi) { started = false; continue; }
            wxPoint p(int(X(q.first * 180.0 / M_PI)), int(Y(db)));
            if (started) dc.DrawLine(prev, p);
            prev = p;
            started = true;
        }
    };

    // M contours: constant closed-loop magnitude m (linear).
    //   g = [m^2 cos(phi) +/- m sqrt(1 - m^2 sin^2(phi))] / (1 - m^2)
    for (double m_db : {-12.0, -6.0, -3.0, 0.0, 3.0, 6.0, 12.0}) {
        double m = std::pow(10.0, m_db / 20.0);
        std::vector<std::pair<double, double>> pts;
        if (std::fabs(m - 1.0) < 1e-9) {
            // 0 dB closed-loop: |L| = -1/(2 cos(phi)), cos(phi) < 0.
            for (int k = -360; k <= 0; ++k) {
                double phi = k * M_PI / 180.0;
                double cp = std::cos(phi);
                if (cp >= -1e-9) continue;
                double g = -1.0 / (2.0 * cp);
                pts.push_back({phi, g});
            }
        } else {
            for (int k = -720; k <= 0; ++k) {
                double phi = k * M_PI / 180.0;
                double cp = std::cos(phi), sp = std::sin(phi);
                double disc = 1.0 - m * m * sp * sp;
                if (disc < 0.0) continue;
                double root = m * std::sqrt(disc);
                double g = (m * m * cp + root) / (1.0 - m * m);
                if (g > 0.0) pts.push_back({phi, g});
            }
        }
        dc.SetPen(chart_pen(m_db == 0.0));
        draw_contour(pts);
        // the second branch (for the +3,+6,+12 dB contours)
        if (m > 1.0) {
            pts.clear();
            for (int k = -720; k <= 0; ++k) {
                double phi = k * M_PI / 180.0;
                double cp = std::cos(phi), sp = std::sin(phi);
                double disc = 1.0 - m * m * sp * sp;
                if (disc < 0.0) continue;
                double root = m * std::sqrt(disc);
                double g = (m * m * cp - root) / (1.0 - m * m);
                if (g > 0.0) pts.push_back({phi, g});
            }
            dc.SetPen(chart_pen(false));
            draw_contour(pts);
        }
    }
    // N contours: constant closed-loop phase alpha (deg).
    for (double alpha : {-150.0, -120.0, -90.0, -60.0, -30.0, 0.0,
                         30.0, 60.0, 90.0, 120.0, 150.0}) {
        double ta = std::tan(alpha * M_PI / 180.0);
        std::vector<std::pair<double, double>> pts;
        for (int k = -720; k <= 0; ++k) {
            double phi = k * M_PI / 180.0;
            double den = std::sin(phi) - ta * std::cos(phi);
            if (std::fabs(den) < 1e-9) continue;
            double g = ta / den;
            if (g > 0.0) pts.push_back({phi, g});
        }
        dc.SetPen(chart_pen(false));
        draw_contour(pts);
    }

    // axis tick labels (light Cartesian axis for orientation)
    int ph_step = 45;
    int deg = int(std::ceil(nx_lo / ph_step)) * ph_step;
    for (; deg <= nx_hi; deg += ph_step) {
        int x = int(X(deg));
        dc.SetPen(wxPen(wxColour(232, 232, 238)));
        dc.DrawLine(x, mT, x, mT + H);
        dc.SetTextForeground(wxColour(110, 110, 118));
        dc.DrawText(wxString::Format("%d", deg) + wxString(wxUniChar(0x00B0)),
                    x - 8, mT + H + 4);
    }
    int db_step = 20;
    if (ny_hi - ny_lo > 200) db_step = 50;
    else if (ny_hi - ny_lo < 40) db_step = 5;
    int db = int(std::ceil(ny_lo / db_step)) * db_step;
    for (; db <= ny_hi; db += db_step) {
        int y = int(Y(db));
        dc.SetPen(wxPen(wxColour(232, 232, 238)));
        dc.DrawLine(mL, y, mL + W, y);
        dc.SetTextForeground(wxColour(110, 110, 118));
        dc.DrawText(wxString::Format("%+d dB", db), 4, y - 6);
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
    dc.SetTextForeground(wxColour(110, 110, 118));
    dc.DrawText("phase [deg]", mL + W - 70, mT + H + 4);
    dc.DrawText("|H| [dB]", 4, mT + 4);
}

// ---------------------------------------------------------------------------
BodePanel::BodePanel(wxWindow* parent) : wxPanel(parent) {
    auto* root = new wxBoxSizer(wxVERTICAL);

    // Toolbar: plot mode + axis spin controls + save buttons.
    auto* bar = new wxBoxSizer(wxHORIZONTAL);
    auto* mode = new wxChoice(this, wxID_ANY);
    mode->Append("Bode");
    mode->Append("Nyquist");
    mode->Append("Nichols");
    mode->SetSelection(0);
    auto* svg = new wxButton(this, wxID_ANY, "Save SVG...");
    auto* png = new wxButton(this, wxID_ANY, "Save PNG...");
    auto* csv = new wxButton(this, wxID_ANY, "Save CSV...");
    auto_box_ = new wxCheckBox(this, wxID_ANY, "Auto scale");
    auto_box_->SetValue(true);
    major_grid_ = new wxCheckBox(this, wxID_ANY, "Major grid");
    major_grid_->SetValue(true);
    minor_grid_ = new wxCheckBox(this, wxID_ANY, "Minor grid");
    minor_grid_->SetValue(true);
    // Frequency / magnitude range fields. Text controls (not spin controls)
    // so the user can type arbitrary values like "1e6" or "-40"; the spin
    // controls' integer-only range made entering a frequency impossible.
    xmin_ = new wxTextCtrl(this, wxID_ANY, "1", wxDefaultPosition, wxSize(70, -1));
    xmax_ = new wxTextCtrl(this, wxID_ANY, "1e8", wxDefaultPosition, wxSize(80, -1));
    ymin_ = new wxTextCtrl(this, wxID_ANY, "-40", wxDefaultPosition, wxSize(60, -1));
    ymax_ = new wxTextCtrl(this, wxID_ANY, "40", wxDefaultPosition, wxSize(60, -1));

    bar->Add(new wxStaticText(this, wxID_ANY, "Plot:"), 0,
             wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    bar->Add(mode, 0, wxRIGHT, 8);
    bar->Add(auto_box_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    bar->Add(major_grid_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    bar->Add(minor_grid_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    bar->Add(new wxStaticText(this, wxID_ANY, "f:"), 0,
             wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 2);
    bar->Add(xmin_, 0, wxRIGHT, 2);
    bar->Add(new wxStaticText(this, wxID_ANY, ".."), 0,
             wxALIGN_CENTER_VERTICAL | wxRIGHT, 2);
    bar->Add(xmax_, 0, wxRIGHT, 8);
    bar->Add(new wxStaticText(this, wxID_ANY, "|H|:"), 0,
             wxALIGN_CENTER_VERTICAL | wxRIGHT, 2);
    bar->Add(ymin_, 0, wxRIGHT, 2);
    bar->Add(new wxStaticText(this, wxID_ANY, ".."), 0,
             wxALIGN_CENTER_VERTICAL | wxRIGHT, 2);
    bar->Add(ymax_, 0, wxRIGHT, 8);
    bar->Add(svg, 0, wxRIGHT, 6);
    bar->Add(png, 0, wxRIGHT, 6);
    bar->Add(csv, 0, 0);
    root->Add(bar, 0, wxEXPAND | wxALL, 4);

    plot_ = new BodeCanvas(this);
    root->Add(plot_, 1, wxEXPAND);

    SetSizer(root);

    mode->Bind(wxEVT_CHOICE, [this, mode](wxCommandEvent&) {
        switch (mode->GetSelection()) {
            case 1: plot_->set_mode(PlotMode::Nyquist); break;
            case 2: plot_->set_mode(PlotMode::Nichols); break;
            default: plot_->set_mode(PlotMode::Bode); break;
        }
    });

    auto_box_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { apply_axis(); });
    major_grid_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
        plot_->set_grid(major_grid_->GetValue(), minor_grid_->GetValue());
    });
    minor_grid_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
        plot_->set_grid(major_grid_->GetValue(), minor_grid_->GetValue());
    });
    // Editing any range field takes the plot out of auto-scale so the typed
    // value is actually used (otherwise "Auto scale" would ignore it).
    auto on_edit = [this](wxCommandEvent&) {
        if (auto_box_->GetValue()) auto_box_->SetValue(false);
        apply_axis();
    };
    xmin_->Bind(wxEVT_TEXT, on_edit);
    xmax_->Bind(wxEVT_TEXT, on_edit);
    ymin_->Bind(wxEVT_TEXT, on_edit);
    ymax_->Bind(wxEVT_TEXT, on_edit);
    xmin_->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent&) { apply_axis(); });
    xmax_->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent&) { apply_axis(); });
    ymin_->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent&) { apply_axis(); });
    ymax_->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent&) { apply_axis(); });

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

void BodePanel::apply_axis() {
    if (auto_box_->GetValue()) {
        plot_->set_auto_range(true);
        return;
    }
    // Parse the four text fields; ignore (keep auto) on any parse failure so a
    // half-typed value doesn't blank the plot.
    double x0, x1, y0, y1;
    bool ok = xmin_->GetValue().ToDouble(&x0) &&
              xmax_->GetValue().ToDouble(&x1) &&
              ymin_->GetValue().ToDouble(&y0) &&
              ymax_->GetValue().ToDouble(&y1);
    if (!ok) return;
    plot_->set_x_range(x0, x1);
    plot_->set_y_range(y0, y1);
}

void BodePanel::set_result(const syms::AnalysisResult* r) {
    plot_->set_result(r);
    Refresh();
}

void BodePanel::set_title(const std::string& t) {
    plot_->set_title(t);
}

void BodePanel::sync_axis_controls() {
    // Reflect the plot's current range back into the text fields (so an
    // auto-scaled plot shows the bounds the user can then edit).
    if (!plot_) return;
    auto fmt = [](double v) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%g", v);
        return wxString(buf);
    };
    if (xmin_) xmin_->ChangeValue(fmt(plot_->x_lo()));
    if (xmax_) xmax_->ChangeValue(fmt(plot_->x_hi()));
    if (ymin_) ymin_->ChangeValue(fmt(plot_->y_lo()));
    if (ymax_) ymax_->ChangeValue(fmt(plot_->y_hi()));
}

} // namespace symcirc