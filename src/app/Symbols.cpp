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
        return {{0, -30}, {0, 30}, {0, -30}, {0, 30}};
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

const wxColour kInk(0, 0, 0);          // components: black
const wxColour kSel(0, 92, 200);       // selection highlight
const wxColour kRefInk(0, 70, 150);    // reference designator
const wxColour kValInk(90, 90, 90);    // value text

struct Ctx {
    wxDC& dc;
    double ox, oy;
    int rot;
    bool flip_h = false, flip_v = false;

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
    void circle(double cx, double cy, double r) const {
        dc.DrawCircle(P(cx, cy), int(std::lround(r)));
    }
    void arc(double cx, double cy, double r, double a0, double a1,
             int steps = 16) const {
        std::vector<Pt> pts;
        for (int i = 0; i <= steps; ++i) {
            double th = a0 + (a1 - a0) * i / steps;
            pts.push_back({cx + r * std::cos(th), cy + r * std::sin(th)});
        }
        polyline(pts);
    }
    // solid triangular arrowhead with tip at (tx,ty) coming from (fx,fy)
    void arrow(double fx, double fy, double tx, double ty, double size) const {
        double dx = tx - fx, dy = ty - fy;
        double len = std::hypot(dx, dy);
        if (len < 1e-9) return;
        dx /= len;
        dy /= len;
        double px = -dy, py = dx;
        dc.SetBrush(wxBrush(dc.GetPen().GetColour()));
        Pt a{tx, ty};
        Pt b{tx - dx * size + px * size * 0.5,
             ty - dy * size + py * size * 0.5};
        Pt c{tx - dx * size - px * size * 0.5,
             ty - dy * size - py * size * 0.5};
        dc.DrawPolygon(3, new wxPoint[3]{P(a.first, a.second), P(b.first, b.second),
                                         P(c.first, c.second)});
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
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
void draw_resistor(const Ctx& t) {
    t.line(-30, 0, -14, 0);
    t.line(14, 0, 30, 0);
    t.polyline({{-14, 0}, {-11, -7}, {-5, 7}, {1, -7}, {7, 7}, {11, -7},
                {14, 0}});
}

void draw_capacitor(const Ctx& t) {
    t.line(-30, 0, -5, 0);
    t.line(5, 0, 30, 0);
    t.line(-5, -12, -5, 12);
    t.line(5, -12, 5, 12);
}

void draw_inductor(const Ctx& t) {
    t.line(-30, 0, -18, 0);
    t.line(18, 0, 30, 0);
    for (int i = 0; i < 3; ++i)
        t.arc(-12 + i * 12, 0, 6, 0, M_PI); // humps on top
}

void draw_diode(const Ctx& t) {
    t.line(-30, 0, -8, 0);
    t.line(8, 0, 30, 0);
    // filled triangle A -> K, cathode bar
    t.dc.SetBrush(wxBrush(kInk));
    wxPoint tri[3] = {t.P(-8, -8), t.P(-8, 8), t.P(8, 0)};
    t.dc.DrawPolygon(3, tri);
    t.dc.SetBrush(*wxTRANSPARENT_BRUSH);
    t.line(8, -8, 8, 8);
}

// --- sources ---------------------------------------------------------------
void draw_vsource(const Ctx& t) {
    t.line(-30, 0, -15, 0);
    t.line(15, 0, 30, 0);
    t.circle(0, 0, 15);
    t.text("+", -15, -7, true);
    t.text("\u2212", 15, -7, true);
}

void draw_isource(const Ctx& t) {
    t.line(-30, 0, -15, 0);
    t.line(15, 0, 30, 0);
    t.circle(0, 0, 15);
    // current flows toward the head (left pin)
    t.arrow(7, 0, -7, 0, 7);
}

void draw_vdd(const Ctx& t) {
    t.line(0, 0, 0, -10);
    t.line(-12, -10, 12, -10);
    t.line(-8, -16, 8, -16);
    t.line(-4, -22, 4, -22);
}

void draw_gnd(const Ctx& t) {
    t.line(0, 0, 0, 10);
    t.line(-14, 10, 14, 10);
    t.line(-9, 16, 9, 16);
    t.line(-4, 22, 4, 22);
}

// --- controlled sources ----------------------------------------------------
void draw_vcvs(const Ctx& t, Kind k) {
    t.polyline({{-30, -20}, {30, -20}, {30, 20}, {-30, 20}}, true);
    t.line(40, -10, 30, -10);
    t.line(40, 10, 30, 10);
    t.line(-40, -10, -30, -10);
    t.line(-40, 10, -30, 10);
    t.text("+", 36, -22, true);
    t.text("\u2212", 36, 2, true);
    t.text("+", -36, -22, true);
    t.text("\u2212", -36, 2, true);
    if (k == Kind::E) {
        // diamond-ish voltage-source mark inside
        t.polyline({{-6, 0}, {0, -8}, {6, 0}, {0, 8}}, true);
    } else {
        // current-source mark inside
        t.circle(0, 0, 7);
        t.arrow(6, 0, -6, 0, 6);
    }
}

// --- transistors -----------------------------------------------------------
void draw_mosfet(const Ctx& t, Kind k) {
    const bool n = (k == Kind::NMOS);
    // gate lead to the left, gate bar
    t.line(-40, 0, -16, 0);
    t.line(-16, -18, -16, 18);
    // channel: three segments (source, gate region, drain) with bulk tap
    t.line(-8, -20, -8, -8);
    t.line(-8, -6, -8, 6);
    t.line(-8, 8, -8, 20);
    // drain (top) and source (bottom) verticals
    t.line(-8, -20, 0, -20);
    t.line(0, -40, 0, -20);
    t.line(-8, 20, 0, 20);
    t.line(0, 40, 0, 20);
    // bulk connection to source/bulk
    t.line(-8, 0, 14, 0);
    t.line(14, 0, 14, 10);
    t.line(0, 20, 14, 10);
    if (n)
        t.arrow(2, 12, -4, 9, 7);   // arrow points into the channel
    else
        t.arrow(-4, 9, 2, 12, 7);   // arrow points out
    // bulk pin
    t.line(14, 0, 40, 0);
}

void draw_bjt(const Ctx& t, Kind k) {
    t.line(-40, 0, -12, 0);
    t.line(-12, -18, -12, 18);
    t.line(-12, -6, 6, -30);
    t.line(0, -40, 6, -30);
    t.line(-12, 6, 6, 30);
    t.line(0, 40, 6, 30);
    if (k == Kind::NPN)
        t.arrow(-8, 17, 2, 27, 8); // emitter arrow points out
    else
        t.arrow(2, 27, -8, 17, 8); // PNP points in
}

// --- amplifiers / blocks ---------------------------------------------------
void draw_opamp(const Ctx& t, bool fully_diff) {
    t.polyline({{-24, -24}, {-24, 24}, {28, 0}}, true);
    // inputs
    t.line(-40, -12, -24, -12);
    t.line(-40, 12, -24, 12);
    t.text("+", -22, -12, true);
    t.text("\u2212", -22, 12, true);
    // outputs
    if (fully_diff) {
        t.line(28, 0, 40, -12);
        t.line(28, 0, 40, 12);
    } else {
        t.line(28, 0, 40, 0);
    }
}

void draw_amp(const Ctx& t) {
    t.polyline({{-24, -20}, {-24, 20}, {28, 0}}, true);
    t.line(-30, 0, -24, 0);
    t.line(28, 0, 30, 0);
    t.text("A", 0, 0, true);
}

void draw_ratio(const Ctx& t, const std::string& mark) {
    t.polyline({{-28, -20}, {28, -20}, {28, 20}, {-28, 20}}, true);
    t.line(-30, 0, -28, 0);
    t.line(28, 0, 30, 0);
    t.text(mark, 0, 0, true);
}

void draw_nullor(const Ctx& t) {
    // nullator (input, oval) with a short diagonal, norator (output, bar)
    t.line(-30, 0, -14, 0);
    t.polyline({{-14, 0}, {0, -12}, {14, 0}, {0, 12}}, true);
    t.line(14, 0, 30, 0); // placeholder; nullor is usually drawn symbolically
    t.text("N", 0, 0, true);
}

void draw_transformer(const Ctx& t) {
    // primary winding (top) and secondary (bottom) as mirrored humps
    t.line(0, -30, 0, -18);
    for (int i = 0; i < 3; ++i) t.arc(0, -12 + i * 12, 6, -M_PI / 2, M_PI / 2);
    t.line(0, 6, 0, 30);
    // core: two short bars between windings
    t.line(6, -14, 6, 14);
    t.line(10, -14, 10, 14);
}

std::string default_label(syms::Kind k) {
    return syms::kind_token(k);
}

} // namespace

// ---------------------------------------------------------------------------
void draw_symbol(wxDC& dc, const syms::Component& c, const Placement& pl,
                 bool selected) {
    wxColour ink = selected ? kSel : kInk;
    dc.SetPen(wxPen(ink, selected ? 2 : 1));
    dc.SetBrush(*wxTRANSPARENT_BRUSH);

    Ctx t{dc, pl.x, pl.y, pl.rot, pl.flip_h, pl.flip_v};
    switch (c.kind) {
    case Kind::R: draw_resistor(t); break;
    case Kind::C: draw_capacitor(t); break;
    case Kind::L: draw_inductor(t); break;
    case Kind::V: draw_vsource(t); break;
    case Kind::I: draw_isource(t); break;
    case Kind::GND: draw_gnd(t); break;
    case Kind::VDD: draw_vdd(t); break;
    case Kind::D: draw_diode(t); break;
    case Kind::E:
    case Kind::G: draw_vcvs(t, c.kind); break;
    case Kind::NMOS:
    case Kind::PMOS: draw_mosfet(t, c.kind); break;
    case Kind::NPN:
    case Kind::PNP: draw_bjt(t, c.kind); break;
    case Kind::NULLOR: draw_nullor(t); break;
    case Kind::OPAMP: draw_opamp(t, false); break;
    case Kind::FDOPAMP: draw_opamp(t, true); break;
    case Kind::AMP: draw_amp(t); break;
    case Kind::IS: draw_ratio(t, "1/s"); break;
    case Kind::SBLK: draw_ratio(t, "s"); break;
    case Kind::T: draw_transformer(t); break;
    case Kind::K:
        // coupling marks are drawn on the two inductors; nothing standalone
        break;
    }

    if (c.kind == Kind::K) return; // no label for the coupling marker

    // labels: reference above-right, value below-right
    dc.SetTextForeground(kRefInk);
    double lx = 34, ly = -20;
    if (c.kind == Kind::GND) { lx = 20; ly = 6; }
    if (c.kind == Kind::VDD) { lx = 20; ly = -24; }
    if (c.kind == Kind::E || c.kind == Kind::G) { lx = 44; ly = -26; }
    if (c.kind == Kind::NMOS || c.kind == Kind::PMOS) { lx = 20; ly = -34; }
    if (c.kind == Kind::NPN || c.kind == Kind::PNP) { lx = 12; ly = -40; }
    if (c.kind == Kind::OPAMP || c.kind == Kind::FDOPAMP ||
        c.kind == Kind::AMP || c.kind == Kind::NULLOR) { lx = 16; ly = -30; }
    t.text(c.ref, lx, ly, false);

    if (!c.value_text.empty() && c.kind != Kind::GND && c.kind != Kind::VDD) {
        dc.SetTextForeground(kValInk);
        t.text(c.value_text, lx, ly + 12, false);
    }

    // device non-ideality marker: only when at least one parasitic is on
    if (syms::is_device(c.kind)) {
        bool any = false;
        for (const auto& kv : c.param_on)
            if (kv.second) any = true;
        if (any) {
            dc.SetTextForeground(wxColour(170, 60, 20));
            t.text("\u00b6", lx, ly + 24, false);
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
    dc.SetPen(wxPen(kInk, 1));
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    syms::Component c;
    c.kind = k;
    c.ref = "";
    Placement pl{0, 0, 0};
    // scale into the swatch
    dc.SetUserScale(0.42, 0.42);
    Ctx t{dc, w / (2 * 0.42), h / (2 * 0.42), 0, false, false};
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
    case Kind::NULLOR: draw_nullor(t); break;
    case Kind::OPAMP: draw_opamp(t, false); break;
    case Kind::FDOPAMP: draw_opamp(t, true); break;
    case Kind::AMP: draw_amp(t); break;
    case Kind::IS: draw_ratio(t, "1/s"); break;
    case Kind::SBLK: draw_ratio(t, "s"); break;
    case Kind::T: draw_transformer(t); break;
    case Kind::K: break;
    }
    dc.SelectObject(wxNullBitmap);
    return bmp;
}

} // namespace symcirc
