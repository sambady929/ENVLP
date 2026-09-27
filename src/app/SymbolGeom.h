#pragma once
#include "Document.h"
#include "core/Netlist.h"

namespace symcirc {

// Pin geometry (relative to component origin, rotation 0), in canvas px.
// Order matches pin_count()/pin_names() conventions in core/Netlist.h.
std::vector<Pt> pin_offsets(syms::Kind k);

// Rotate a point by rot degrees (0/90/180/270, clockwise on screen).
Pt rotate_pt(Pt p, int rot);

// Apply flip_h/flip_v then rotation (must match pin_world / the renderer).
Pt transform_pt(Pt p, const Placement& pl);

// World position of a pin for a placed component.
Pt pin_world(const syms::Component& c, const Placement& pl, int pin_index);

// The direction a wire should leave a pin, in *world* coordinates, as a unit
// vector. Ported from analog-canvas's per-pin `direction`
// (north/east/south/west): a wire from a pin must escape along the pin's
// outward axis so it never runs back across the symbol body. For kinds whose
// pins don't have a single obvious outward axis (e.g. the four-terminal
// transformer) returns {0,0} and the caller falls back to free routing.
Pt pin_outward(const syms::Component& c, const Placement& pl, int pin_index);

// Bounding box (axis-aligned), used by hit-testing, selection, and the
// ref/value label anchor. `pad` grows the box on all sides; the default is
// generous for hit tolerance, but selection and label anchoring pass a small
// pad so a symbol does not swallow the wires attached to its pins.
void symbol_bbox(const syms::Component& c, const Placement& pl, double& x0,
                 double& y0, double& x1, double& y1, double pad = 18.0);

// The symbol's *drawn body* box only (no pins), in world coordinates. Used for
// hit-testing: a click selects a part by its artwork, not by the long span of
// its pin lines -- otherwise a MOSFET's hit box is many times a wire's, which
// makes selecting between a device and its wires feel arbitrary. `pad` grows
// the box a little so the outline itself is easy to grab.
void symbol_body_bbox(const syms::Component& c, const Placement& pl,
                      double& x0, double& y0, double& x1, double& y1,
                      double pad = 0.0);

} // namespace symcirc
