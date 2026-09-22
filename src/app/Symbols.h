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

// Apply flip_h/flip_v then rotation (must match pin_world / the renderer).
Pt transform_pt(Pt p, const Placement& pl);

// World position of a pin for a placed component.
Pt pin_world(const syms::Component& c, const Placement& pl, int pin_index);

// Draw the component symbol, leads, and ref/value labels.
// `selected` gets a highlight color on the symbol body.
void draw_symbol(wxDC& dc, const syms::Component& c, const Placement& pl,
                 bool selected);

// Bounding box (axis-aligned) used for hit testing.
void symbol_bbox(const syms::Component& c, const Placement& pl, double& x0,
                 double& y0, double& x1, double& y1);

// Mutual-coupling marker: a dashed arc between the two inductor centers with
// a filled dot on each winding.
void draw_coupling(wxDC& dc, Pt a, Pt b, bool selected);
void coupling_bbox(Pt a, Pt b, double& x0, double& y0, double& x1, double& y1);

// Palette swatch: draws a small symbol preview into a bitmap.
wxBitmap symbol_swatch(syms::Kind k, int w, int h);

} // namespace symcirc
