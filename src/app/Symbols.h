#pragma once
#include "Document.h"
#include "core/Netlist.h"

#include <wx/dc.h>

#include <vector>

namespace symcirc {

// Pin geometry (relative to component origin, rotation 0), in canvas px.
// Order matches pin_count()/pin_names() conventions in core/Netlist.h.
std::vector<Pt> pin_offsets(syms::Kind k);

// Rotate a point by rot degrees (0/90/180/270, clockwise on screen).
Pt rotate_pt(Pt p, int rot);

// World position of a pin for a placed component.
Pt pin_world(const syms::Component& c, const Placement& pl, int pin_index);

// Draw the component symbol, leads, and ref/value labels.
// `selected` gets a highlight color on the symbol body.
void draw_symbol(wxDC& dc, const syms::Component& c, const Placement& pl,
                 bool selected);

// Bounding box (axis-aligned) used for hit testing.
void symbol_bbox(const syms::Component& c, const Placement& pl, double& x0,
                 double& y0, double& x1, double& y1);

} // namespace symcirc
