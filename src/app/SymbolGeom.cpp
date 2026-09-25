// Pure-geometry helpers for component symbols (no wxWidgets dependency).
// Used by Document (net resolution), the canvas (hit-testing), and the symbol
// renderer.
#include "SymbolGeom.h"

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
    // V and I are drawn vertical (rotated 90 deg CCW): +/head up, -/tail down
    case Kind::V:
        return {{0, -30}, {0, 30}}; // + , -
    case Kind::I:
        return {{0, -30}, {0, 30}}; // head , tail
    case Kind::D:
        return {{-30, 0}, {30, 0}}; // A , K
    case Kind::IS:
    case Kind::SBLK:
    case Kind::AMP:
        return {{-30, 0}, {30, 0}}; // in , out
    case Kind::E:
    case Kind::G:
    case Kind::CCCS:
    case Kind::CCVS:
        // out+ , out- , ctrl+ , ctrl-
        return {{40, -10}, {40, 10}, {-40, -10}, {-40, 10}};
    case Kind::NMOS:
        // D , G , S  (D top, S bottom)
        return {{0, -40}, {-40, 0}, {0, 40}};
    case Kind::PMOS:
        // flipped vertically: S (with the arrow) on top, D on the bottom
        return {{0, 40}, {-40, 0}, {0, -40}};
    case Kind::NPN:
        // C , B , E  (C top, E bottom)
        return {{0, -40}, {-40, 0}, {0, 40}};
    case Kind::PNP:
        // flipped vertically: E (with the arrow) on top, C on the bottom
        return {{0, 40}, {-40, 0}, {0, -40}};
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
        return {{-40, -14}, {-40, 14}, {34, -14}, {34, 14}};
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

Pt pin_outward(const syms::Component& c, const Placement& pl, int pin_index) {
    // Local-frame outward direction per kind/pin. This mirrors analog-canvas's
    // SymbolPinSchema.direction: a wire must leave a pin along this axis.
    // The vector is then run through transform_pt so rotation/flip follow.
    Pt local{0, 0};
    switch (c.kind) {
    case Kind::R:
    case Kind::C:
    case Kind::L:
    case Kind::D:
    case Kind::IS:
    case Kind::SBLK:
    case Kind::AMP:
        // two-terminal horizontal: pin0 (left) escapes west, pin1 (right) east
        local = (pin_index == 0) ? Pt{-1, 0} : Pt{1, 0};
        break;
    case Kind::V:
    case Kind::I:
        // drawn vertical: +/head (pin0) escapes north, -/tail (pin1) south
        local = (pin_index == 0) ? Pt{0, -1} : Pt{0, 1};
        break;
    case Kind::NMOS:
    case Kind::NPN:
        // D/C top -> north, G/B left -> west, S/E bottom -> south
        local = (pin_index == 0) ? Pt{0, -1}
                : (pin_index == 1) ? Pt{-1, 0}
                                   : Pt{0, 1};
        break;
    case Kind::PMOS:
    case Kind::PNP:
        // S/E top, G/B left, D/C bottom
        local = (pin_index == 0) ? Pt{0, -1}
                : (pin_index == 1) ? Pt{-1, 0}
                                   : Pt{0, 1};
        break;
    case Kind::E:
    case Kind::G:
    case Kind::CCCS:
    case Kind::CCVS:
        // out+ , out- , ctrl+ , ctrl-
        local = (pin_index == 0 || pin_index == 1) ? Pt{1, 0} : Pt{-1, 0};
        break;
    case Kind::OPAMP:
        // in+ , in- , out
        local = (pin_index >= 2) ? Pt{1, 0} : Pt{-1, 0};
        break;
    case Kind::FDOPAMP:
        local = (pin_index >= 2) ? Pt{1, 0} : Pt{-1, 0};
        break;
    case Kind::NULLOR:
        local = (pin_index >= 2) ? Pt{1, 0} : Pt{-1, 0};
        break;
    case Kind::VDD:
    case Kind::GND:
        local = {0, -1}; // rail above the pin
        break;
    default:
        return {0, 0}; // no single outward axis; caller routes freely
    }
    Pt r = transform_pt(local, pl);
    double len = std::hypot(r.first, r.second);
    if (len < 1e-9) return {0, 0};
    return {r.first / len, r.second / len};
}

// Half-extents of a symbol's *drawn body* in its own local frame, ignoring
// pins. Used so ref/value labels clear the artwork, not just the pin line.
// Returns {x0,y0,x1,y1}; a kind with no local body returns a degenerate box
// at the origin.
namespace {
struct LocalBox { double x0, y0, x1, y1; };
LocalBox body_local_box(Kind k) {
    switch (k) {
    // horizontal two-terminal bodies: zigzag / plates / coils span y
    case Kind::R: return {-13, -9, 13, 9};
    case Kind::C: return {-5, -13, 5, 13};
    case Kind::L: return {-14, -9, 14, 9};
    case Kind::D: return {-13, -12, 13, 12};
    // sources: circle radius ~13, plus a small cap above
    case Kind::V: return {-13, -17, 13, 13};
    case Kind::I: return {-13, -17, 13, 13};
    // MOSFET: gate bar + channel, roughly this footprint
    case Kind::NMOS: return {-16, -22, 12, 22};
    case Kind::PMOS: return {-16, -22, 12, 22};
    case Kind::NPN: return {-14, -22, 16, 22};
    case Kind::PNP: return {-14, -22, 16, 22};
    // controlled sources: the box is ±26 wide, ±20 tall
    case Kind::E:
    case Kind::G:
    case Kind::CCCS:
    case Kind::CCVS: return {-26, -20, 26, 20};
    case Kind::OPAMP: return {-26, -22, 30, 22};
    case Kind::NULLOR: return {-20, -16, 20, 16};
    case Kind::FDOPAMP: return {-26, -22, 34, 22};
    case Kind::AMP: return {-26, -20, 30, 20};
    case Kind::IS: return {-16, -16, 16, 16};
    case Kind::SBLK: return {-16, -16, 16, 16};
    case Kind::T: return {-40, -22, 40, 22};
    case Kind::K: return {-8, -8, 8, 8};
    case Kind::GND: return {-12, 0, 12, 21};
    case Kind::VDD: return {-13, -15, 13, 0};
    }
    return {0, 0, 0, 0};
}
} // namespace

void symbol_bbox(const syms::Component& c, const Placement& pl, double& x0,
                 double& y0, double& x1, double& y1, double pad) {
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
    // Union in the drawn body so labels sit beside the artwork, not on it.
    LocalBox lb = body_local_box(c.kind);
    Pt corners[4] = {{lb.x0, lb.y0}, {lb.x1, lb.y0},
                     {lb.x1, lb.y1}, {lb.x0, lb.y1}};
    for (const auto& cpt : corners) {
        Pt r = transform_pt(cpt, pl);
        acc({pl.x + r.first, pl.y + r.second});
    }
    x0 -= pad;
    y0 -= pad;
    x1 += pad;
    y1 += pad;
}

} // namespace symcirc
