#pragma once
#include <wx/colour.h>

// Visual theme, ported from the analog-canvas reference (razavi-textbook-v1
// style profile + the editor's :root tokens). Keeping the values in one place
// makes the canvas, symbols and panels agree, and makes it trivial to retune.
//
// Reference values:
//   canvas panel bg      #fafaf9
//   svg canvas bg        #fff
//   grid dot             #c4c7c9, 10x10 unit lattice, r = 0.7 (no major/minor)
//   foreground / ink     #000
//   accent (selection)   #2383e2
//   wire / symbol stroke 1.6 ; emphasis 2.4 ; ground 2.907 ; supply 1.8
//   junction dot         r = 3.77907, fill #000
//   selection halo       accent @ 42%, width 7
//   hover                accent @ 22%, dashed
//   marquee window       accent @ 10% fill, accent stroke
//   marquee crossing     #3fa34d @ 10% fill, #3fa34d stroke
namespace envlp {
namespace theme {

// --- canvas ---------------------------------------------------------------
inline const wxColour canvas_bg(0xfa, 0xfa, 0xf9);
inline const wxColour svg_bg(0xff, 0xff, 0xff);
inline const wxColour grid_dot(0xc4, 0xc7, 0xc9);

// --- ink / accent ---------------------------------------------------------
inline const wxColour ink(0x00, 0x00, 0x00);
inline const wxColour accent(0x23, 0x83, 0xe2);
inline const wxColour accent_soft(0x23, 0x83, 0xe2, 0x1a);   // ~10%
inline const wxColour accent_sel(0x23, 0x83, 0xe2, 0x24);    // ~14%
inline const wxColour accent_halo(0x23, 0x83, 0xe2, 0x6b);   // ~42%
inline const wxColour accent_hover(0x23, 0x83, 0xe2, 0x38);  // ~22%
inline const wxColour marquee_crossing(0x3f, 0xa3, 0x4d);
inline const wxColour marquee_crossing_soft(0x3f, 0xa3, 0x4d, 0x1a);
inline const wxColour error_red(0xeb, 0x57, 0x57);
inline const wxColour wire_warn(0xd3, 0x21, 0x2c);

// --- chrome ---------------------------------------------------------------
// A cool, modern light palette: slightly blue-grey surfaces so white cards and
// the canvas read as "paper on a desk" instead of a flat beige sheet.
inline const wxColour chrome_bg(0xf4, 0xf6, 0xfa);
inline const wxColour surface(0xff, 0xff, 0xff);
inline const wxColour surface_muted(0xed, 0xf0, 0xf6);
inline const wxColour surface_hover(0xe4, 0xe9, 0xf2);
inline const wxColour border(0xe2, 0xe6, 0xee);
inline const wxColour border_strong(0xd2, 0xd9, 0xe6);
inline const wxColour text(0x1f, 0x24, 0x30);
inline const wxColour text_muted(0x6b, 0x72, 0x84);
inline const wxColour text_disabled(0xb0, 0xb6, 0xc2);
inline const wxColour accent_tint(0xe8, 0xf0, 0xfe);
// Card chrome for the analysis cards (border + header strip).
inline const wxColour card_bg(0xff, 0xff, 0xff);
inline const wxColour card_head(0xf8, 0xfa, 0xfd);


// --- ink roles for symbols ------------------------------------------------
inline const wxColour ref_ink(0x00, 0x00, 0x00);   // reference designator
inline const wxColour val_ink(0x78, 0x77, 0x74);   // value text (muted)

// --- stroke weights (document units) --------------------------------------
constexpr double kStrokeWire = 1.6;
constexpr double kStrokeSymbol = 1.6;
constexpr double kStrokeEmphasis = 2.4;
constexpr double kStrokeGround = 2.907;
constexpr double kStrokeSupply = 1.8;

// Junction / solder dot radius (document units).
constexpr double kJunctionRadius = 3.78;

// --- grids ----------------------------------------------------------------
constexpr double kGrid = 10.0;      // document grid
constexpr double kPinGrid = 2.0;    // symbol pin connection grid
constexpr double kSnapRadiusPx = 6.0;  // electrical capture, screen px

// --- typography -----------------------------------------------------------
constexpr int kRefFontPt = 9;
constexpr int kValFontPt = 9;

} // namespace theme
} // namespace envlp
