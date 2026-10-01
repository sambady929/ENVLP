#pragma once
#include "SymbolGeom.h"

#include <wx/dc.h>

namespace envlp {

// Draw the component symbol, leads, and ref/value labels.
// `selected` gets a highlight color on the symbol body.
void draw_symbol(wxDC& dc, const syms::Component& c, const Placement& pl,
                 bool selected);

// Palette swatch: draws a small symbol preview into a bitmap.
wxBitmap symbol_swatch(syms::Kind k, int w, int h);

} // namespace envlp
