#include "Symbols.h"

#include <algorithm>
#include <cmath>

namespace symcirc {

using syms::Kind;

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
        return {{0, 0}};
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

Pt pin_world(const syms::Component& c, const Placement& pl, int pin_index) {
    auto offs = pin_offsets(c.kind);
    if (pin_index < 0 || pin_index >= int(offs.size())) return {pl.x, pl.y};
    Pt r = rotate_pt(offs[pin_index], pl.rot);
    return {pl.x + r.first, pl.y + r.second};
}

// ---------------------------------------------------------------------------
// drawing helpers
// ---------------------------------------------------------------------------
namespace {

struct Ctx {
    wxDC& dc;
    double ox, oy;
    int rot;

    wxPoint P(double x, double y) const {
        Pt r = rotate_pt({x, y}, rot);
        return wxPoint(int(std::lround(ox + r.first)),
                       int(std::lround(oy + r.second)));
    }
    void line(double x0, double y0, double x1, double y1) const {
        dc.DrawLine(P(x0, y0), P(x1, y1));
    }
    void polyline(const std::vector<Pt>& pts) const {
        for (size_t i = 1; i < pts.size(); ++i)
            line(pts[i - 1].first, pts[i - 1].second, pts[i].first,
                 pts[i].second);
    }
    // Arrowhead at (tx,ty) coming from (fx,fy).
    void arrow(double fx, double fy, double tx, double ty, double size) const {
        double dx = tx - fx, dy = ty - fy;
        double len = std::hypot(dx, dy);
        if (len < 1e-9) return;
        dx /= len;
        dy /= len;
        double bx = tx - dx * size, by = ty - dy * size;
        double px = -dy, py = dx;
        polyline({{tx, ty}, {bx + px * size * 0.5, by + py * size * 0.5}});
        polyline({{tx, ty}, {bx - px * size * 0.5, by - py * size * 0.5}});
    }
    void label(const std::string& text, double x, double y) const {
        wxPoint p = P(x, y);
        wxSize ts = dc.GetTextExtent(wxString::FromUTF8(text));
        dc.DrawText(wxString::FromUTF8(text), p.x - ts.x / 2, p.y - ts.y / 2);
    }
};

void draw_passive(const Ctx& t, syms::Kind k) {
    t.line(-30, 0, -14, 0);
    t.line(14, 0, 30, 0);
    if (k == Kind::R) {
        // zig-zag
        t.polyline({{-14, 0}, {-11, -7}, {-5, 7}, {1, -7}, {7, 7}, {11, -7},
                    {14, 0}});
    } else if (k == Kind::C) {
        t.line(-6, -12, -6, 12);
        t.line(6, -12, 6, 12);
    } else { // L: three arcs approximated by polylines
        for (int i = 0; i < 3; ++i) {
            double x0 = -14 + i * 9.33;
            std::vector<Pt> arc;
            for (int a = 0; a <= 8; ++a) {
                double th = M_PI * a / 8.0;
                arc.push_back({x0 + 9.33 * a / 8.0, -std::sin(th) * 7.0});
            }
            t.polyline(arc);
        }
    }
}

void draw_source(const Ctx& t, syms::Kind k) {
    t.line(-30, 0, -15, 0);
    t.line(15, 0, 30, 0);
    // circle of radius15 through polyline
    std::vector<Pt> circ;
    for (int a = 0; a <= 32; ++a) {
        double th = 2 * M_PI * a / 32.0;
        circ.push_back({std::cos(th) * 15.0, std::sin(th) * 15.0});
    }
    t.polyline(circ);
    if (k == Kind::V) {
        t.dc.SetTextBackground(t.dc.GetBackground().GetColour());
        t.label("+", -22, -10);
        t.label("-", 22, -10);
    } else {
        // current exits the head pin (left): arrow pointing left
        t.arrow(9, 0, -9, 0, 6);
    }
}

void draw_vcvs(const Ctx& t, syms::Kind k) {
    // box
    t.polyline({{-30, -20}, {30, -20}, {30, 20}, {-30, 20}, {-30, -20}});
    // leads
    t.line(40, -10, 30, -10);
    t.line(40, 10, 30, 10);
    t.line(-40, -10, -30, -10);
    t.line(-40, 10, -30, 10);
    t.label("+", 36, -20);
    t.label("-", 36, 2);
    t.label("+", -36, -20);
    t.label(k == Kind::E ? "E" : "G", 0, 0);
}

void draw_mosfet(const Ctx& t, Kind k) {
    // gate lead + bar
    t.line(-40, 0, -12, 0);
    t.line(-12, -20, -12, 20);
    // channel bar
    t.line(0, -25, 0, 25);
    // drain / source leads
    t.line(0, -40, 0, -25);
    t.line(0, 40, 0, 25);
    // bulk lead
    t.line(40, 0, 0, 0);
    // gap between gate bar and channel implied by geometry
    if (k == Kind::NMOS)
        t.arrow(24, 0, 12, 0, 7); // inward
    else
        t.arrow(12, 0, 24, 0, 7); // outward
}

void draw_bjt(const Ctx& t, Kind k) {
    // base lead + bar
    t.line(-40, 0, -10, 0);
    t.line(-10, -20, -10, 20);
    // collector / emitter diagonals to top/bottom pins
    t.line(-10, -8, 0, -40);
    t.line(-10, 8, 0, 40);
    // emitter arrow: NPN points away from base (down), PNP toward (up)
    if (k == Kind::NPN)
        t.arrow(-7, 24, -1, 38, 7);
    else
        t.arrow(-1, 38, -7, 24, 7);
}

void draw_gnd(const Ctx& t) {
    t.line(0, 0, 0, 15);
    t.line(-15, 15, 15, 15);
    t.line(-9, 21, 9, 21);
    t.line(-3, 27, 3, 27);
}

} // namespace

void draw_symbol(wxDC& dc, const syms::Component& c, const Placement& pl,
                 bool selected) {
    wxColour body = selected ? wxColour(0, 120, 215)
                             : wxColour(35, 35, 40);
    if (c.kind == Kind::GND) body = selected ? wxColour(0, 120, 215)
                                             : wxColour(60, 60, 65);
    dc.SetPen(wxPen(body, selected ? 2 : 1));
    if (selected) dc.SetBrush(wxBrush(body, wxBRUSHSTYLE_TRANSPARENT));

    Ctx t{dc, pl.x, pl.y, pl.rot};

    switch (c.kind) {
    case Kind::R:
    case Kind::C:
    case Kind::L:      draw_passive(t, c.kind); break;
    case Kind::V:
    case Kind::I:      draw_source(t, c.kind); break;
    case Kind::E:
    case Kind::G:      draw_vcvs(t, c.kind); break;
    case Kind::NMOS:
    case Kind::PMOS:   draw_mosfet(t, c.kind); break;
    case Kind::NPN:
    case Kind::PNP:    draw_bjt(t, c.kind); break;
    case Kind::GND:    draw_gnd(t); break;
    }

    // labels (always horizontal text, positioned relative to the symbol)
    dc.SetTextForeground(wxColour(20, 90, 160));
    wxPoint rp = t.P(c.kind == Kind::GND ? 26 : 48, -6);
    dc.DrawText(wxString::FromUTF8(c.ref), rp.x, rp.y);
    if (!c.value_text.empty() && c.kind != Kind::GND) {
        dc.SetTextForeground(wxColour(100, 100, 110));
        wxPoint vp = t.P(48, 8);
        dc.DrawText(wxString::FromUTF8(c.value_text), vp.x, vp.y);
    }

    // parasitic tick mark when any device param is enabled
    if (syms::is_independent_source(c.kind) == false &&
        (c.kind == Kind::NMOS || c.kind == Kind::PMOS ||
         c.kind == Kind::NPN || c.kind == Kind::PNP)) {
        bool any = false;
        for (const auto& kv : c.param_on)
            if (kv.second) any = true;
        if (any) {
            dc.SetTextForeground(wxColour(170, 60, 20));
            wxPoint pp = t.P(48, 22);
            dc.DrawText(wxString::FromUTF8("\u00b6"), pp.x, pp.y);
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
        Pt r = rotate_pt(offs[i], pl.rot);
        acc({pl.x + r.first, pl.y + r.second});
    }
    // include body extents
    double m = syms::is_independent_source(c.kind) || c.kind == Kind::GND ? 18 : 14;
    x0 -= m;
    y0 -= m;
    x1 += m;
    y1 += m;
}

} // namespace symcirc
