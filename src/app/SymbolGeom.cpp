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
    x0 -= pad;
    y0 -= pad;
    x1 += pad;
    y1 += pad;
}

} // namespace symcirc
