#include "BodePanel.h"
#include "core/Eng.h"

#include <algorithm>
#include <cmath>
#include <vector>
#include <wx/dcbuffer.h>

namespace symcirc {

wxBEGIN_EVENT_TABLE(BodePanel, wxPanel)
    EVT_PAINT(BodePanel::on_paint)
wxEND_EVENT_TABLE()

BodePanel::BodePanel(wxWindow* parent) : wxPanel(parent) {
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetBackgroundColour(*wxWHITE);
}

void BodePanel::set_result(const syms::AnalysisResult* r) {
    res_ = r;
    Refresh();
}

void BodePanel::on_paint(wxPaintEvent&) {
    wxAutoBufferedPaintDC dc(this);
    wxSize sz = GetClientSize();
    dc.SetBackground(*wxWHITE);
    dc.Clear();

    if (!res_) {
        dc.SetTextForeground(wxColour(150, 150, 155));
        dc.DrawText("Run an analysis (F5) to see the Bode plot.",
                     12, 12);
        return;
    }

    const int mL = 56, mR = 12, mT = 12, mB = 42;
    const int W = sz.x - mL - mR;
    const int Hh = (sz.y - mT - mB - 14) / 2; // mag half
    const int Hp = sz.y - mT - mB - 14;       // phase half (shared x)
    if (W < 50 || Hh < 30) return;

    const double f_start = 1.0, f_end = 1e8;
    const int N = 400;
    auto freqs = syms::sweep_hz(f_start, f_end, N);

    std::vector<double> mag(freqs.size()), ph(freqs.size());
    double mag_min = 1e300, mag_max = -1e300, ph_min = 1e300, ph_max = -1e300;
    for (size_t i = 0; i < freqs.size(); ++i) {
        double w = 2 * M_PI * freqs[i];
        mag[i] = syms::mag_db_at(*res_, w);
        ph[i] = syms::phase_deg_at(*res_, w);
        if (std::isfinite(mag[i])) {
            mag_min = std::min(mag_min, mag[i]);
            mag_max = std::max(mag_max, mag[i]);
        }
        if (std::isfinite(ph[i])) {
            ph_min = std::min(ph_min, ph[i]);
            ph_max = std::max(ph_max, ph[i]);
        }
    }
    if (mag_min > mag_max) return; // nothing plottable

    // pad ranges
    auto pad = [](double& lo, double& hi) {
        double span = std::max(hi - lo, 1.0);
        lo -= span * 0.08;
        hi += span * 0.08;
    };
    pad(mag_min, mag_max);
    ph_min = -200;
    ph_max = 200;

    auto x_of = [&](double f) {
        double t = std::log10(f / f_start) / std::log10(f_end / f_start);
        return mL + t * W;
    };

    // --- grids: decades on x ---
    dc.SetPen(wxPen(wxColour(232, 232, 236)));
    dc.SetTextForeground(wxColour(140, 140, 145));
    for (int e = 0; e <= 8; ++e) {
        double f = std::pow(10.0, e);
        int x = int(x_of(f));
        dc.DrawLine(x, mT, x, mT + Hh);
        dc.DrawLine(x, mT + Hh + 14, x, mT + Hh + 14 + Hp);
        wxString lbl;
        if (e == 0) lbl = "1";
        else if (e <= 3)
            lbl = wxString::Format("1e%d", e);
        else
            lbl = wxString::Format("%gk", std::pow(10.0, e - 3));
        if (e >= 4 && (e - 4) % 3 == 0) {} // label below in Hz at decades
        dc.DrawText(lbl, x + 2, mT + Hh + 14 + Hp + 4);
    }

    // --- magnitude frame ---
    auto y_mag = [&](double db) {
        return mT + Hh - (db - mag_min) / (mag_max - mag_min) * Hh;
    };
    dc.SetPen(wxPen(wxColour(210, 210, 215)));
    dc.DrawRectangle(mL, mT, W, Hh);
    // y labels (dB)
    int db_step = int(std::max(10.0, std::ceil((mag_max - mag_min) / 5)));
    for (int db = int(std::floor(mag_min / db_step) * db_step);
         db <= mag_max; db += db_step) {
        int y = int(y_mag(db));
        if (y < mT || y > mT + Hh) continue;
        dc.SetPen(wxPen(wxColour(240, 240, 244)));
        dc.DrawLine(mL, y, mL + W, y);
        dc.SetTextForeground(wxColour(140, 140, 145));
        dc.DrawText(wxString::Format("%d dB", db), 4, y - 6);
    }

    // --- phase frame ---
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

    // --- curves ---
    dc.SetPen(wxPen(wxColour(200, 40, 40), 2));
    bool started = false;
    wxPoint prev(0, 0);
    for (size_t i = 0; i < freqs.size(); ++i) {
        if (!std::isfinite(mag[i])) {
            started = false;
            continue;
        }
        wxPoint p(int(x_of(freqs[i])), int(y_mag(mag[i])));
        if (started) dc.DrawLine(prev, p);
        prev = p;
        started = true;
    }

    dc.SetPen(wxPen(wxColour(40, 90, 200), 2));
    started = false;
    for (size_t i = 0; i < freqs.size(); ++i) {
        if (!std::isfinite(ph[i])) {
            started = false;
            continue;
        }
        wxPoint p(int(x_of(freqs[i])), int(y_ph(ph[i])));
        if (started) dc.DrawLine(prev, p);
        prev = p;
        started = true;
    }

    // titles
    dc.SetTextForeground(wxColour(200, 40, 40));
    dc.DrawText("Magnitude", mL + 6, mT + 4);
    dc.SetTextForeground(wxColour(40, 90, 200));
    dc.DrawText("Phase", mL + 6, mT + Hh + 18);
    dc.SetTextForeground(wxColour(90, 90, 95));
    dc.DrawText("frequency [Hz]", mL + W - 78, mT + Hh + 14 + Hp + 4);
}

} // namespace symcirc
