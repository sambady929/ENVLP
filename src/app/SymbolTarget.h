#pragma once
#include "SymbolGeom.h"

#include <string>
#include <vector>

namespace envlp {

// Where symbol artwork is sent. Symbols.cpp draws through this, so there is one
// implementation of the art for the desktop (wxDC) and for the browser (path
// data rendered on a canvas).
//
// Coordinates are in the symbol's local frame with the placement transform
// already applied, so a web target is fed a Ctx built with an identity
// placement and gets pristine local geometry.
struct Rgb {
    unsigned char r = 0, g = 0, b = 0;
};

struct SymbolTarget {
    virtual ~SymbolTarget() = default;
    virtual void set_width(double w) = 0;
    virtual void set_colour(Rgb c) = 0;
    // Text drawn by a symbol (marks such as "+"/"-"/"K"), not the reference and
    // value labels -- those are placed by the front-end.
    virtual void set_emphasis(bool on) = 0;
    virtual void line(double x0, double y0, double x1, double y1) = 0;
    virtual void fill_poly(const std::vector<Pt>& pts) = 0;
    virtual void circle(double x, double y, double r) = 0;
    virtual void text(const std::string& s, double x, double y, bool centered) = 0;
};

// Draw a component kind's artwork (no pins, no labels) through `t`, with an
// identity placement -- the coordinates are the symbol's own frame.
void draw_body_kind(SymbolTarget& t, syms::Kind k);

// Same, with a placement applied (the desktop draws straight into a DC).
void draw_body_placed(SymbolTarget& t, syms::Kind k, double ox, double oy,
                      int rot, bool flip_h, bool flip_v, Rgb col);

} // namespace envlp
