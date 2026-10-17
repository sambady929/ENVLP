#include "Symbols.h"
#include "SymbolGeom.h"
#include "SymbolTarget.h"
#include "Theme.h"

#include <algorithm>
#include <cmath>
#include <wx/dcmemory.h>

namespace envlp {

using syms::Kind;

// ---------------------------------------------------------------------------
// drawing helpers
// ---------------------------------------------------------------------------
namespace {

const wxColour kInk = theme::ink;         // components: black
const wxColour kSel = theme::accent;      // selection highlight
const wxColour kRefInk = theme::ref_ink;  // reference designator
const wxColour kValInk = theme::val_ink;  // value text

// Stroke weights, in px: thin = leads, thick = device bars / plates / bodies.
constexpr double kWire = theme::kStrokeWire;
constexpr double kBody = theme::kStrokeEmphasis;

// The wxWidgets target: draws the same calls onto a DC.
struct WxTarget : SymbolTarget {
    wxDC& dc;
    wxColour col;
    mutable int last_width = -1;
    explicit WxTarget(wxDC& d, wxColour c) : dc(d), col(c) {}
    void set_width(double w) override {
        int px = std::max(1, int(std::lround(w)));
        if (px == last_width) return;
        last_width = px;
        dc.SetPen(wxPen(col, px));
    }
    void set_colour(Rgb c) override {
        col = wxColour(c.r, c.g, c.b);
        last_width = -1;
    }
    void set_emphasis(bool) override {} // draw_coupling sets the DC font itself
    void line(double x0, double y0, double x1, double y1) override {
        dc.DrawLine(int(std::lround(x0)), int(std::lround(y0)),
                    int(std::lround(x1)), int(std::lround(y1)));
    }
    void fill_poly(const std::vector<Pt>& pts) override {
        std::vector<wxPoint> wp;
        wp.reserve(pts.size());
        for (const auto& p : pts)
            wp.push_back(wxPoint(int(std::lround(p.first)),
                                 int(std::lround(p.second))));
        dc.SetBrush(wxBrush(col));
        dc.DrawPolygon(int(wp.size()), wp.data());
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
    }
    void circle(double x, double y, double r) override {
        dc.DrawCircle(wxPoint(int(std::lround(x)), int(std::lround(y))),
                      int(std::lround(r)));
    }
    void text(const std::string& s, double x, double y, bool centered) override {
        wxString t = wxString::FromUTF8(s);
        int px = int(std::lround(x)), py = int(std::lround(y));
        if (centered) {
            wxSize ts = dc.GetTextExtent(t);
            px -= ts.x / 2;
            py -= ts.y / 2;
        }
        dc.DrawText(t, px, py);
    }
};

} // namespace

// ---------------------------------------------------------------------------
void draw_symbol(wxDC& dc, const syms::Component& c, const Placement& pl,
                 bool selected) {
    wxColour ink = selected ? kSel : kInk;
    dc.SetPen(wxPen(ink, int(kWire)));
    dc.SetBrush(*wxTRANSPARENT_BRUSH);

    WxTarget target(dc, ink);
    Rgb ink_rgb{(unsigned char)ink.Red(), (unsigned char)ink.Green(),
                (unsigned char)ink.Blue()};
    draw_body_placed(target, c.kind, pl.x, pl.y, pl.rot, pl.flip_h, pl.flip_v,
                     ink_rgb);

    if (c.kind == Kind::K) return; // coupling marker carries no label

    wxFont base = dc.GetFont();

    // Ground is anonymous: show neither a reference nor a value, just the
    // glyph. VDD is special -- it's a supply rail, so it always reads "VDD"
    // with the DC voltage underneath.
    if (c.kind == Kind::GND) return;

    double bx0, by0, bx1, by1;
    symbol_bbox(c, pl, bx0, by0, bx1, by1, 2.0);

    if (c.kind == Kind::VDD) {
        // A supply rail always reads just "VDD" above the glyph. Its voltage
        // is a DC-analysis setting, not a per-symbol value, so nothing is
        // drawn underneath.
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
        // A three-pin transistor has its gate on one side (left normally, right
        // when mirrored), drain top, source bottom. The label sits on the side
        // OPPOSITE the gate and toward the top, so neither the gate wire nor
        // the drain lead rides over it.
        double tx = bx1 + gap;
        double stack_y = by0 + gap;
        bool three_pin = c.kind == Kind::NMOS || c.kind == Kind::PMOS ||
                         c.kind == Kind::NPN || c.kind == Kind::PNP;
        if (three_pin) {
            auto offs = pin_offsets(c.kind);
            if (offs.size() > 1) {
                Pt g = transform_pt(offs[1], pl); // gate offset in world frame
                // Gate on the right -> label on the left (and vice versa).
                if (g.first > 0.0) tx = bx0 - gap - ref_ts.x;
            }
        }
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
}

void symbol_bbox(const syms::Component& c, const Placement& pl, double& x0,
                 double& y0, double& x1, double& y1); // in SymbolGeom.cpp

wxBitmap symbol_swatch(syms::Kind k, int w, int h) {
    wxBitmap bmp(w, h);
    wxMemoryDC dc(bmp);
    dc.SetBackground(*wxWHITE_BRUSH);
    dc.Clear();
    // Scale the artwork with the bitmap so a high-DPI (larger) swatch draws a
    // proportionally larger symbol rather than the same glyph in more padding.
    double sc = 0.36 * double(w) / 40.0;
    int pen = std::max(1, int(std::lround(kWire * double(w) / 40.0)));
    dc.SetPen(wxPen(kInk, pen));
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    dc.SetUserScale(sc, sc);
    WxTarget swatch_target(dc, kInk);
    Rgb swatch_ink{(unsigned char)kInk.Red(), (unsigned char)kInk.Green(),
                    (unsigned char)kInk.Blue()};
    draw_body_placed(swatch_target, k, w / (2 * sc), h / (2 * sc), 0, false,
                     false, swatch_ink);
    dc.SelectObject(wxNullBitmap);
    return bmp;
}

} // namespace envlp
