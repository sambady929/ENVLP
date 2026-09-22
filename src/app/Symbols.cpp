#include "Symbols.h"

#include <algorithm>
#include <cmath>
#include <wx/dcmemory.h>

namespace symcirc {

using syms::Kind;

// ---------------------------------------------------------------------------
// pin geometry
// ---------------------------------------------------------------------------
std::vector<Pt> pin_offsets(Kind k) {
    switch (k) {
    case Kind::R:
    case Kind::C:
    case Kind::L:
        return {{-30, 0}, {30, 0}};
    case Kind::V:
        return {{-30, 0}, {30, 0}}; // + , -
    case Kind::I:
        return {{-30, 0}, {30, 0}}; // head, tail
    case Kind::D:
        return {{-30, 0}, {30, 0}}; // A , K
    case Kind::IS:
    case Kind::SBLK:
    case Kind::AMP:
        return {{-30, 0}, {30, 0}}; // in , out
    case Kind::E:
    case Kind::G:
        // out+ , out- , ctrl+ , ctrl-
        return {{40, -10}, {40, 10}, {-40, -10}, {-40, 10}};
    case Kind::NMOS:
    case Kind::PMOS:
        // D , G , S , B
        return {{0, -40}, {-40, 0}, {0, 40}, {40, 0}};
    case Kind::NPN:
    case Kind::PNP:
        // C , B , E
        return {{0, -40}, {-40, 0}, {0, 40}};
    case Kind::GND:
    case Kind::VDD:
        return {{0, 0}};
    case Kind::T:
        // p+ , p- , s+ , s-
        return {{-40, -20}, {-40, 20}, {40, -20}, {40, 20}};
    case Kind::NULLOR:
    case Kind::OPAMP:
        // in+ , in- , out
        return {{-40, -12}, {-40, 12}, {40, 0}};
    case Kind::FDOPAMP:
        // in+ , in- , out+ , out-
        return {{-40, -12}, {-40, 12}, {40, -12}, {40, 12}};
    case Kind::K:
        return {};
    }
    return {};
}

Pt rotate_pt(Pt p, int rot) {
    switch (((rot % 360) + 360) % 360) {
    case 90:  return {p.second, -p.first};
    case 180: return {-p.first, -p.second};
    case 270: return {-p.second, p.first};
    default:  return p;
    }
}

Pt transform_pt(Pt p, const Placement& pl) {
    if (pl.flip_h) p.first = -p.first;
    if (pl.flip_v) p.second = -p.second;
    return rotate_pt(p, pl.rot);
}

Pt pin_world(const syms::Component& c, const Placement& pl, int pin_index) {
    auto offs = pin_offsets(c.kind);
    if (pin_index < 0 || pin_index >= int(offs.size())) return {pl.x, pl.y};
    Pt r = transform_pt(offs[pin_index], pl);
    return {pl.x + r.first, pl.y + r.second};
}

// ---------------------------------------------------------------------------
// drawing helpers
// ---------------------------------------------------------------------------
namespace {

const wxColour kInk(0, 0, 0);        // components: black
const wxColour kSel(0, 92, 200);     // selection highlight
const wxColour kRefInk(0, 70, 150);  // reference designator
const wxColour kValInk(80, 80, 80);  // value text

// Stroke weights, in px: thin = leads, thick = device bars / plates / bodies.
constexpr double kWire = 2.0;
constexpr double kBody = 3.0;

struct Ctx {
    wxDC& dc;
    double ox, oy;
    int rot;
    bool flip_h = false, flip_v = false;
    wxColour col;

    // set the pen width for the next strokes
    void w(double width) const {
        dc.SetPen(wxPen(col, std::max(1, int(std::lround(width)))));
    }

    wxPoint P(double x, double y) const {
        Pt r = rotate_pt({flip_h ? -x : x, flip_v ? -y : y}, rot);
        return wxPoint(int(std::lround(ox + r.first)),
                       int(std::lround(oy + r.second)));
    }
    void line(double x0, double y0, double x1, double y1) const {
        dc.DrawLine(P(x0, y0), P(x1, y1));
    }
    void polyline(const std::vector<Pt>& pts, bool close = false) const {
        for (size_t i = 1; i < pts.size(); ++i)
            line(pts[i - 1].first, pts[i - 1].second, pts[i].first,
                 pts[i].second);
        if (close && pts.size() > 2)
            line(pts.back().first, pts.back().second, pts.front().first,
                 pts.front().second);
    }
    void fillpoly(const std::vector<Pt>& pts) const {
        std::vector<wxPoint> wp;
        wp.reserve(pts.size());
        for (const auto& p : pts) wp.push_back(P(p.first, p.second));
        dc.SetBrush(wxBrush(col));
        dc.DrawPolygon(int(wp.size()), wp.data());
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
    }
    // filled rectangle between two corners (axis-aligned in symbol space)
    void fillrect(double x0, double y0, double x1, double y1) const {
        fillpoly({{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}});
    }
    void circle(double cx, double cy, double r) const {
        dc.DrawCircle(P(cx, cy), int(std::lround(r)));
    }
    void arc(double cx, double cy, double r, double a0, double a1,
             int steps = 18) const {
        std::vector<Pt> pts;
        for (int i = 0; i <= steps; ++i) {
            double th = a0 + (a1 - a0) * i / steps;
            pts.push_back({cx + r * std::cos(th), cy + r * std::sin(th)});
        }
        polyline(pts);
    }
    // solid triangular arrowhead, tip at (tx,ty), coming from (fx,fy)
    void arrow(double fx, double fy, double tx, double ty, double size) const {
        double dx = tx - fx, dy = ty - fy;
        double len = std::hypot(dx, dy);
        if (len < 1e-9) return;
        dx /= len;
        dy /= len;
        double px = -dy, py = dx;
        fillpoly({{tx, ty},
                  {tx - dx * size + px * size * 0.5,
                   ty - dy * size + py * size * 0.5},
                  {tx - dx * size - px * size * 0.5,
                   ty - dy * size - py * size * 0.5}});
    }
    void text(const std::string& s, double x, double y, bool centered) const {
        wxPoint p = P(x, y);
        wxString t = wxString::FromUTF8(s);
        if (centered) {
            wxSize ts = dc.GetTextExtent(t);
            p.x -= ts.x / 2;
            p.y -= ts.y / 2;
        }
        dc.DrawText(t, p.x, p.y);
    }
};

// --- passives --------------------------------------------------------------
void draw_resistor(Ctx& t) {
    t.w(kWire);
    t.line(-30, 0, -13, 0);
    t.line(13, 0, 30, 0);
    t.polyline({{-13, 0},   {-10.8, -8}, {-6.5, 8}, {-2.2, -8},
                {2.2, 8},   {6.5, -8},   {10.8, 8}, {13, 0}});
}

void draw_capacitor(Ctx& t) {
    t.w(kWire);
    t.line(-30, 0, -5, 0);
    t.line(5, 0, 30, 0);
    t.w(kBody);
    t.line(-5, -13, -5, 13);
    t.line(5, -13, 5, 13);
}

void draw_inductor(Ctx& t) {
    t.w(kWire);
    t.line(-30, 0, -16, 0);
    t.line(16, 0, 30, 0);
    // four upward humps
    for (int i = 0; i < 4; ++i) t.arc(-12 + i * 8, 0, 4, M_PI, 2 * M_PI);
}

void draw_diode(Ctx& t) {
    t.w(kWire);
    t.line(-30, 0, -10, 0);
    t.line(9, 0, 30, 0);
    // anode triangle (outline) and cathode bar
    t.polyline({{-10, -10}, {-10, 10}, {9, 0}}, true);
    t.w(kBody);
    t.line(9, -10, 9, 10);
}

// --- sources ---------------------------------------------------------------
void draw_vsource(Ctx& t) {
    t.w(kWire);
    t.line(-30, 0, -15, 0);
    t.line(15, 0, 30, 0);
    t.w(kWire);
    t.circle(0, 0, 15);
    // polarity marks to the left of the circle (+ upper, - lower)
    t.line(-25, -8, -17, -8);
    t.line(-21, -12, -21, -4);
    t.line(-25, 8, -17, 8);
}

void draw_isource(Ctx& t) {
    t.w(kWire);
    t.line(-30, 0, -15, 0);
    t.line(15, 0, 30, 0);
    t.circle(0, 0, 15);
    // arrow toward the head pin (left)
    t.line(7, 0, 1, 0);
    t.arrow(1, 0, -8, 0, 7);
}

void draw_vdd(Ctx& t) {
    t.w(kWire);
    t.line(0, 0, 0, -11);
    t.fillrect(-13, -15, 13, -11);
}

void draw_gnd(Ctx& t) {
    t.w(kWire);
    t.line(0, 0, 0, 8);
    t.w(kBody);
    t.line(-12, 8, 12, 8);
    t.line(-7.5, 15, 7.5, 15);
    t.line(-4, 21, 4, 21);
}

// --- controlled sources ----------------------------------------------------
void draw_vcvs(Ctx& t, Kind k) {
    t.w(kWire);
    t.line(40, -10, 26, -10);
    t.line(40, 10, 26, 10);
    t.line(-40, -10, -26, -10);
    t.line(-40, 10, -26, 10);
    t.w(kBody);
    t.polyline({{-26, -20}, {26, -20}, {26, 20}, {-26, 20}}, true);
    // polarity marks
    t.w(kWire);
    t.line(-23, -10, -17, -10);
    t.line(-20, -13, -20, -7);
    t.line(-23, 10, -17, 10);
    t.line(17, -10, 23, -10);
    t.line(20, -13, 20, -7);
    t.line(17, 10, 23, 10);
    t.text(k == Kind::E ? "E" : "G", 0, 0, true);
}

// --- transistors -----------------------------------------------------------
void draw_mosfet(Ctx& t, Kind k) {
    const bool n = (k == Kind::NMOS);
    // gate lead and gate bar (thick, filled)
    t.w(kWire);
    t.line(-40, 0, -16, 0);
    t.fillrect(-16, -18, -13, 18);
    // channel bar (thick, filled)
    t.fillrect(-8, -22, -5, 22);
    // drain / source taps
    t.w(kWire);
    t.polyline({{-5, -18}, {0, -18}, {0, -40}});
    t.polyline({{-5, 18}, {0, 18}, {0, 40}});
    // bulk tap from mid-channel out to the B pin, with the polarity arrow
    t.line(-5, 0, 12, 0);
    t.line(12, 0, 40, 0);
    if (n)
        t.arrow(9, 0, -2, 0, 9); // into the channel
    else
        t.arrow(-2, 0, 9, 0, 9); // out of the channel
}

void draw_bjt(Ctx& t, Kind k) {
    const bool npn = (k == Kind::NPN);
    t.w(kWire);
    t.line(-40, 0, -22, 0);
    t.w(kBody);
    t.line(-22, -16, -22, 16); // base bar
    t.w(kWire);
    t.polyline({{-22, -7.5}, {0, -19}, {0, -40}}); // collector
    t.polyline({{-22, 7.5}, {0, 19}, {0, 40}});    // emitter
    // emitter arrow sits on the emitter segment
    if (npn)
        t.arrow(-14, 13, -4, 17.75, 9); // outward (away from base)
    else
        t.arrow(-8, 15, -18, 10.4, 9);  // inward (toward base)
}

// --- amplifiers / blocks ---------------------------------------------------
void draw_opamp(Ctx& t, bool fully_diff, bool nullor) {
    t.w(kWire);
    t.line(-40, -12, -30, -12);
    t.line(-40, 12, -30, 12);
    if (fully_diff) {
        t.line(24, 0, 40, -12);
        t.line(24, 0, 40, 12);
    } else {
        t.line(24, 0, 40, 0);
    }
    t.w(kBody);
    t.polyline({{-30, -30}, {-30, 30}, {24, 0}}, true);
    t.w(kWire);
    if (fully_diff) {
        // input +/- inside
        t.line(-27, -12, -21, -12);
        t.line(-24, -15, -24, -9);
        t.line(-27, 12, -21, 12);
        // output polarity
        t.line(28, -14, 34, -14);
        t.line(31, -17, 31, -11);
        t.line(28, 14, 34, 14);
    } else {
        t.line(-27, -12, -21, -12);
        t.line(-24, -15, -24, -9);
        t.line(-27, 12, -21, 12);
    }
    if (nullor) t.text("\u221e", 0, 0, true);
}

void draw_amp(Ctx& t) {
    t.w(kWire);
    t.line(-30, 0, -26, 0);
    t.line(21, 0, 30, 0);
    t.w(kBody);
    t.polyline({{-26, -26}, {-26, 26}, {21, 0}}, true);
}

void draw_ratio(Ctx& t, const std::string& mark) {
    t.w(kWire);
    t.line(-30, 0, -22, 0);
    t.line(22, 0, 30, 0);
    t.w(kBody);
    t.polyline({{-22, -22}, {22, -22}, {22, 22}, {-22, 22}}, true);
    t.text(mark, 0, 0, true);
}

void draw_transformer(Ctx& t) {
    t.w(kWire);
    // primary (left) and secondary (right) windings
    t.line(-40, -20, -40, -16);
    t.line(-40, 16, -40, 20);
    for (int i = 0; i < 4; ++i)
        t.arc(-40, -12 + i * 8, 4, M_PI / 2, 3 * M_PI / 2);
    t.line(40, -20, 40, -16);
    t.line(40, 16, 40, 20);
    for (int i = 0; i < 4; ++i)
        t.arc(40, -12 + i * 8, 4, -M_PI / 2, M_PI / 2);
    // core
    t.w(kBody);
    t.line(-5, -16, -5, 16);
    t.line(5, -16, 5, 16);
}

void draw_body(Ctx& t, Kind k) {
    switch (k) {
    case Kind::R: draw_resistor(t); break;
    case Kind::C: draw_capacitor(t); break;
    case Kind::L: draw_inductor(t); break;
    case Kind::V: draw_vsource(t); break;
    case Kind::I: draw_isource(t); break;
    case Kind::GND: draw_gnd(t); break;
    case Kind::VDD: draw_vdd(t); break;
    case Kind::D: draw_diode(t); break;
    case Kind::E:
    case Kind::G: draw_vcvs(t, k); break;
    case Kind::NMOS:
    case Kind::PMOS: draw_mosfet(t, k); break;
    case Kind::NPN:
    case Kind::PNP: draw_bjt(t, k); break;
    case Kind::NULLOR: draw_opamp(t, false, true); break;
    case Kind::OPAMP: draw_opamp(t, false, false); break;
    case Kind::FDOPAMP: draw_opamp(t, true, false); break;
    case Kind::AMP: draw_amp(t); break;
    case Kind::IS: draw_ratio(t, "1/s"); break;
    case Kind::SBLK: draw_ratio(t, "s"); break;
    case Kind::T: draw_transformer(t); break;
    case Kind::K: break;
    }
}

void label_anchor(Kind k, double& lx, double& ly) {
    lx = 22;
    ly = -22;
    switch (k) {
    case Kind::GND: lx = 16; ly = 4; break;
    case Kind::VDD: lx = 18; ly = -26; break;
    case Kind::E:
    case Kind::G: lx = 28; ly = -24; break;
    case Kind::NMOS:
    case Kind::PMOS: lx = 12; ly = -34; break;
    case Kind::NPN:
    case Kind::PNP: lx = 8; ly = -36; break;
    case Kind::OPAMP:
    case Kind::FDOPAMP:
    case Kind::AMP:
    case Kind::NULLOR: lx = 10; ly = -34; break;
    case Kind::T: lx = -34; ly = -34; break;
    case Kind::IS:
    case Kind::SBLK: lx = 24; ly = -24; break;
    default: break;
    }
}

} // namespace

// ---------------------------------------------------------------------------
void draw_symbol(wxDC& dc, const syms::Component& c, const Placement& pl,
                 bool selected) {
    wxColour ink = selected ? kSel : kInk;
    dc.SetPen(wxPen(ink, int(kWire)));
    dc.SetBrush(*wxTRANSPARENT_BRUSH);

    Ctx t{dc, pl.x, pl.y, pl.rot, pl.flip_h, pl.flip_v, ink};
    draw_body(t, c.kind);

    if (c.kind == Kind::K) return; // coupling marker carries no label

    wxFont base = dc.GetFont();
    double lx, ly;
    label_anchor(c.kind, lx, ly);

    wxFont ref_font = base;
    ref_font.SetStyle(wxFONTSTYLE_ITALIC);
    ref_font.SetWeight(wxFONTWEIGHT_BOLD);
    dc.SetFont(ref_font);
    dc.SetTextForeground(kRefInk);
    t.text(c.ref, lx, ly, false);

    if (!c.value_text.empty() && c.kind != Kind::GND && c.kind != Kind::VDD) {
        wxFont val_font = base;
        val_font.SetStyle(wxFONTSTYLE_ITALIC);
        dc.SetFont(val_font);
        dc.SetTextForeground(kValInk);
        t.text(c.value_text, lx, ly + 14, false);
    }
    dc.SetFont(base);

    // device non-ideality marker: only when at least one parasitic is on
    if (syms::is_device(c.kind)) {
        bool any = false;
        for (const auto& kv : c.param_on)
            if (kv.second) any = true;
        if (any) {
            dc.SetTextForeground(wxColour(170, 60, 20));
            t.text("\u00b6", lx, ly + 26, false);
        }
    }
}

void symbol_bbox(const syms::Component& c, const Placement& pl, double& x0,
                 double& y0, double& x1, double& y1) {
    auto offs = pin_offsets(c.kind);
    x0 = y0 = 1e18;
    x1 = y1 = -1e18;
    auto acc = [&](Pt w) {
        x0 = std::min(x0, w.first);
        y0 = std::min(y0, w.second);
        x1 = std::max(x1, w.first);
        y1 = std::max(y1, w.second);
    };
    acc({pl.x, pl.y});
    for (size_t i = 0; i < offs.size(); ++i) {
        Pt r = transform_pt(offs[i], pl);
        acc({pl.x + r.first, pl.y + r.second});
    }
    double m = 18;
    x0 -= m;
    y0 -= m;
    x1 += m;
    y1 += m;
}

wxBitmap symbol_swatch(syms::Kind k, int w, int h) {
    wxBitmap bmp(w, h);
    wxMemoryDC dc(bmp);
    dc.SetBackground(*wxWHITE_BRUSH);
    dc.Clear();
    dc.SetPen(wxPen(kInk, int(kWire)));
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    dc.SetUserScale(0.36, 0.36);
    Ctx t{dc, w / (2 * 0.36), h / (2 * 0.36), 0, false, false, kInk};
    draw_body(t, k);
    dc.SelectObject(wxNullBitmap);
    return bmp;
}

} // namespace symcirc
