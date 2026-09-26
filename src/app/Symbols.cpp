#include "Symbols.h"
#include "SymbolGeom.h"

#include <algorithm>
#include <cmath>
#include <wx/dcmemory.h>

namespace symcirc {

using syms::Kind;

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
    // pen cache: drawing a symbol sets the same pen many times; skip the
    // redundant wxDC::SetPen calls (they are surprisingly costly on GDI).
    mutable int last_width = -1;

    // set the pen width for the next strokes
    void w(double width) const {
        int px = std::max(1, int(std::lround(width)));
        if (px == last_width) return;
        last_width = px;
        dc.SetPen(wxPen(col, px));
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
    // ellipse in symbol space; `rot`-flip-aware at the endpoints via P()
    void ellipse(double cx, double cy, double rx, double ry,
                 int steps = 32) const {
        std::vector<Pt> pts;
        for (int i = 0; i <= steps; ++i) {
            double th = 2.0 * M_PI * i / steps;
            pts.push_back({cx + rx * std::cos(th), cy + ry * std::sin(th)});
        }
        polyline(pts);
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
    t.line(0, -30, 0, -15);
    t.line(0, 15, 0, 30);
    t.circle(0, 0, 15);
    // polarity marks inside the circle (+ upper, - lower)
    t.line(-7, -7, 7, -7);
    t.line(0, -12, 0, -2);
    t.line(-7, 7, 7, 7);
}

void draw_isource(Ctx& t) {
    t.w(kWire);
    t.line(0, -30, 0, -15);
    t.line(0, 15, 0, 30);
    t.circle(0, 0, 15);
    // arrow points down, away from the head pin (current source convention)
    t.line(0, -8, 0, -1);
    t.arrow(0, -1, 0, 8, 7);
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
    // polarity marks just outside the box, at the pin rows
    t.w(kWire);
    t.line(-23, -10, -19, -10);
    t.line(-21, -12, -21, -8);
    t.line(-23, 10, -19, 10);
    t.line(19, -10, 23, -10);
    t.line(21, -12, 21, -8);
    t.line(19, 10, 23, 10);
    t.text(k == Kind::E ? "E" : "G", 0, 0, true);
}

void draw_body_ccvs_cccs(Ctx& t, Kind k) {
    t.w(kWire);
    t.line(40, -10, 26, -10);
    t.line(40, 10, 26, 10);
    t.line(-40, -10, -26, -10);
    t.line(-40, 10, -26, 10);
    t.w(kBody);
    t.polyline({{-26, -20}, {26, -20}, {26, 20}, {-26, 20}}, true);
    // input port polarity marks just outside the box
    t.w(kWire);
    t.line(-23, -10, -19, -10);
    t.line(-21, -12, -21, -8);
    t.line(-23, 10, -19, 10);
    // output port polarity marks
    t.line(19, -10, 23, -10);
    t.line(21, -12, 21, -8);
    t.line(19, 10, 23, 10);
    t.text(k == Kind::CCCS ? "F" : "H", 0, 0, true);
}

// --- transistors -----------------------------------------------------------
// Razavi / sym1 geometry: thin gate lead into a thick gate bar, a thick
// channel bar, drain and source taps, and a source-polarity arrow.
void draw_mosfet(Ctx& t, Kind k) {
    const bool n = (k == Kind::NMOS);
    t.w(kWire);
    t.line(-40, 0, -31.33, 0);
    // drain and source taps off the ends of the channel bar, to the pins
    t.polyline({{-20.67, -16}, {0, -16}, {0, -40}});
    t.polyline({{-20.67, 16}, {0, 16}, {0, 40}});
    // thick bars: gate bar and channel bar
    t.w(kBody);
    t.line(-31.33, -14, -31.33, 14);
    t.line(-20.67, -16, -20.67, 16);
    // Source arrow on the source tap. The NMOS arrow points to the right
    // (toward the drain/source pins); the PMOS is drawn mirrored so its arrow
    // points to the left.
    if (n)
        t.arrow(-19, 16, -4, 16, 8);    // NMOS: tip to the right
    else
        t.arrow(-4, -16, -19, -16, 8);  // PMOS: tip to the left
}

void draw_bjt(Ctx& t, Kind k) {
    const bool npn = (k == Kind::NPN);
    t.w(kWire);
    t.line(-40, 0, -16.87, 0);
    t.w(kBody);
    t.line(-16.87, -13.35, -16.87, 13.34); // base bar
    t.w(kWire);
    if (npn) {
        // sym1 geometry: collector diagonal to the top, emitter diagonal to
        // the bottom, filled emitter arrow pointing down-right
        t.polyline({{-16.87, -6.4}, {0, -13.38}, {0, -40}});
        t.polyline({{-16.87, 6.4}, {0, 13.38}, {0, 40}});
        t.fillpoly({{-6.64, 6.79}, {-9.95, 13.38}, {0, 13.38}});
    } else {
        // PNP (upside down): emitter diagonal up-right with the arrow at the
        // base end pointing back toward the base
        t.polyline({{-8.58, -9.69}, {0, -13.38}, {0, -40}});
        t.polyline({{-16.87, 6.4}, {0, 13.38}, {0, 40}});
        t.fillpoly({{-10.23, -12.99}, {-6.92, -6.4}, {-16.87, -6.4}});
    }
}

// --- amplifiers / blocks ---------------------------------------------------
void draw_opamp(Ctx& t, bool fully_diff) {
    t.w(kWire);
    t.line(-40, -10, -30, -10);
    t.line(-40, 10, -30, 10);
    if (fully_diff) {
        // short horizontal output leads that start where the slanted edges
        // meet the output pin heights
        t.line(-2.3, -10, 22, -10);
        t.line(-2.3, 10, 22, 10);
    } else {
        t.line(21.96, 0, 40, 0);
    }
    t.w(kBody);
    t.polyline({{-30, -30}, {-30, 30}, {21.96, 0}}, true);
    t.w(kWire);
    if (fully_diff) {
        // input + / - (inverting input is the lower one)
        t.line(-24, -10, -18, -10);
        t.line(-21, -13, -21, -7);
        t.line(-24, 10, -18, 10);
        // output marks moved further left: - on the top output, + below
        t.line(-14, -10, -9, -10);
        t.line(-12, -13, -12, -7); // plus on the lower output
        t.line(-14, 10, -9, 10);   // minus on the upper output
    } else {
        t.line(-27, -10, -21, -10);
        t.line(-24, -13, -24, -7);
        t.line(-27, 10, -21, 10);
    }
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

// standalone mutual-coupling marker: a literal letter K
void draw_coupling(Ctx& t) {
    wxFont f = t.dc.GetFont();
    f.SetPointSize(std::max(10, f.GetPointSize() + 6));
    f.SetWeight(wxFONTWEIGHT_BOLD);
    f.SetStyle(wxFONTSTYLE_NORMAL);
    t.dc.SetFont(f);
    t.dc.SetTextForeground(t.col);
    t.text("K", 0, 0, true);
}

void draw_transformer(Ctx& t) {
    t.w(kWire);
    // leads from the pins to the windings
    t.polyline({{-40, -20}, {-22, -20}, {-22, -16}});
    t.polyline({{-40, 20}, {-22, 20}, {-22, 16}});
    for (int i = 0; i < 4; ++i)
        t.arc(-22, -12 + i * 8, 4, M_PI / 2, 3 * M_PI / 2);
    t.polyline({{40, -20}, {22, -20}, {22, -16}});
    t.polyline({{40, 20}, {22, 20}, {22, 16}});
    for (int i = 0; i < 4; ++i)
        t.arc(22, -12 + i * 8, 4, -M_PI / 2, M_PI / 2);
    // core, close to the windings
    t.w(kBody);
    t.line(-3, -16, -3, 16);
    t.line(3, -16, 3, 16);
}

void draw_nullor(Ctx& t) {
    // Nullor as a two-port: the nullator on the left is a narrow, slightly
    // tall ellipse, and the norator on the right is two overlapping vertical
    // circles.
    t.w(kWire);
    t.line(-40, -12, -24, -12);
    t.line(-40, 12, -24, 12);
    t.line(24, 0, 40, 0);
    t.w(kBody);
    // nullator: tall ellipse
    t.ellipse(-16, 0, 6, 15, 36);
    // norator: two overlapping vertical circles
    t.circle(10, -7, 8);
    t.circle(10, 7, 8);
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
    case Kind::CCCS:
    case Kind::CCVS: draw_body_ccvs_cccs(t, k); break;
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
    case Kind::K: draw_coupling(t); break;
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

    // Ground is anonymous: show neither a reference nor a value, just the
    // glyph. VDD is special -- it's a supply rail, so it always reads "VDD"
    // with the DC voltage underneath.
    if (c.kind == Kind::GND) return;

    double bx0, by0, bx1, by1;
    symbol_bbox(c, pl, bx0, by0, bx1, by1, 2.0);

    if (c.kind == Kind::VDD) {
        // "VDD" above the glyph (supply rails read as a label on top), the
        // DC value below it. Both are centred horizontally on the symbol.
        double fs = 9.0;
        wxFont f = base;
        f.SetPointSize(int(fs));
        f.SetWeight(wxFONTWEIGHT_BOLD);
        dc.SetFont(f);
        wxString ref_text = "VDD";
        wxSize rs = dc.GetTextExtent(ref_text);
        double cx = (bx0 + bx1) / 2.0;
        dc.SetTextForeground(kRefInk);
        dc.DrawText(ref_text, wxPoint(int(cx - rs.x / 2.0), int(by0 - rs.y)));
        if (!c.value_text.empty()) {
            f.SetWeight(wxFONTWEIGHT_NORMAL);
            dc.SetFont(f);
            wxString vt = wxString::FromUTF8(c.value_text) + " V";
            wxSize vs = dc.GetTextExtent(vt);
            dc.SetTextForeground(kValInk);
            dc.DrawText(vt, wxPoint(int(cx - vs.x / 2.0), int(by1 + 2)));
        }
        dc.SetTextForeground(kRefInk);
        dc.SetFont(base);
        return;
    }

    // Text placement. A *horizontal* component (its body runs left-to-right)
    // puts the reference above the body and the value below it, both centred;
    // a *vertical* component stacks the two to the right of the body. The
    // anchor distances come from symbol_bbox, which unions the drawn body, so
    // each kind uses its own size instead of one fixed offset.
    double fs = 9.0;

    wxFont ref_font = base;
    ref_font.SetPointSize(int(fs));
    ref_font.SetStyle(wxFONTSTYLE_ITALIC);
    ref_font.SetWeight(wxFONTWEIGHT_BOLD);
    dc.SetFont(ref_font);
    dc.SetTextForeground(kRefInk);
    wxString ref_text = wxString::FromUTF8(c.ref);
    wxSize ref_ts = dc.GetTextExtent(ref_text);
    double lh = ref_ts.y;

    wxString val_text = !c.value_text.empty()
                            ? wxString::FromUTF8(c.value_text)
                            : wxString();

    bool horizontal = (bx1 - bx0) >= (by1 - by0);
    double cx = (bx0 + bx1) / 2.0;
    // A tight margin. Horizontal passive/amplifier labels hug the body (the
    // bbox already pads), and the resistor/inductor/op-amp bodies do not need
    // the full bounding box height, so pull them in a little more.
    double gap = 1.5;
    if (horizontal) {
        switch (c.kind) {
            case Kind::R:
            case Kind::L:
            case Kind::AMP:
            case Kind::OPAMP:
            case Kind::FDOPAMP:
                gap = 0.0; // body box is taller than the ink: hug it
                break;
            default:
                gap = 1.5;
                break;
        }
    }
    double marker_x = bx1 + gap, marker_y = by1 + gap;

    if (horizontal) {
        dc.DrawText(ref_text,
                    wxPoint(int(cx - ref_ts.x / 2.0), int(by0 - gap - lh)));
        if (!val_text.IsEmpty()) {
            wxFont val_font = base;
            val_font.SetPointSize(int(fs));
            val_font.SetStyle(wxFONTSTYLE_ITALIC);
            dc.SetFont(val_font);
            dc.SetTextForeground(kValInk);
            wxSize vs = dc.GetTextExtent(val_text);
            double vy = by1 + gap;
            dc.DrawText(val_text, wxPoint(int(cx - vs.x / 2.0), int(vy)));
            marker_x = cx + vs.x / 2.0 + 2.0;
            marker_y = vy;
            dc.SetTextForeground(kRefInk);
        } else {
            marker_x = cx + ref_ts.x / 2.0 + 2.0;
            marker_y = by0 - gap - lh;
        }
    } else {
        double total_h = val_text.IsEmpty() ? lh : 2 * lh;
        double stack_y = (by0 + by1) / 2.0 - total_h / 2.0;
        double tx = bx1 + gap;
        dc.DrawText(ref_text, wxPoint(int(tx), int(stack_y)));
        if (!val_text.IsEmpty()) {
            wxFont val_font = base;
            val_font.SetPointSize(int(fs));
            val_font.SetStyle(wxFONTSTYLE_ITALIC);
            dc.SetFont(val_font);
            dc.SetTextForeground(kValInk);
            dc.DrawText(val_text, wxPoint(int(tx), int(stack_y + lh)));
            dc.SetTextForeground(kRefInk);
        }
        marker_x = tx;
        marker_y = stack_y + (val_text.IsEmpty() ? 0 : lh) + lh;
    }
    dc.SetFont(base);

    // device non-ideality marker (¶): sits just past the label text when at
    // least one parasitic is enabled.
    if (syms::is_device(c.kind)) {
        bool any = false;
        for (const auto& kv : c.param_on)
            if (kv.second) any = true;
        if (any) {
            dc.SetTextForeground(wxColour(170, 60, 20));
            // Use an explicitly constructed Unicode char: a raw "\u00b6"
            // narrow literal is UTF-8 (0xC2 0xB6) and wxString's implicit
            // conversion misreads it as two Latin-1 bytes ("Â¶").
            dc.DrawText(wxString(wxUniChar(0x00B6)),
                        wxPoint(int(marker_x), int(marker_y)));
        }
    }
}

void symbol_bbox(const syms::Component& c, const Placement& pl, double& x0,
                 double& y0, double& x1, double& y1); // in SymbolGeom.cpp

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
