#include "SchematicCanvas.h"
#include "Symbols.h"

#include <algorithm>
#include <cmath>
#include <wx/dcbuffer.h>

namespace symcirc {

using syms::Component;
using syms::Kind;

wxBEGIN_EVENT_TABLE(SchematicCanvas, wxWindow)
    EVT_PAINT(SchematicCanvas::on_paint)
    EVT_LEFT_DOWN(SchematicCanvas::on_left_down)
    EVT_LEFT_UP(SchematicCanvas::on_left_up)
    EVT_LEFT_DCLICK(SchematicCanvas::on_left_dclick)
    EVT_MOTION(SchematicCanvas::on_motion)
    EVT_RIGHT_DOWN(SchematicCanvas::on_right_down)
    EVT_RIGHT_UP(SchematicCanvas::on_right_up)
    EVT_MOUSEWHEEL(SchematicCanvas::on_mousewheel)
    EVT_LEAVE_WINDOW(SchematicCanvas::on_leave)
    EVT_MOUSE_CAPTURE_CHANGED(SchematicCanvas::on_capture_changed)
    EVT_SIZE(SchematicCanvas::on_size)
wxEND_EVENT_TABLE()

namespace {
constexpr double kGrid = 10.0;
constexpr double kSnapR = 5.0;  // 1/2 grid in *document* units; pins and wires
                                // both share this tolerance for hover/select
constexpr double kMinZoom = 0.05, kMaxZoom = 8.0;

double dist(Pt a, Pt b) { return std::hypot(a.first - b.first, a.second - b.second); }

// Distance from p to segment ab.
double seg_dist(Pt p, Pt a, Pt b) {
    double vx = b.first - a.first, vy = b.second - a.second;
    double wx = p.first - a.first, wy = p.second - a.second;
    double L2 = vx * vx + vy * vy;
    if (L2 < 1e-12) return std::hypot(wx, wy);
    double t = (wx * vx + wy * vy) / L2;
    t = std::max(0.0, std::min(1.0, t));
    return std::hypot(wx - t * vx, wy - t * vy);
}

// Manhattan route between two points. `h_first` puts the horizontal leg first
// (corner at {b.x, a.y}) or the vertical leg first (corner at {a.x, b.y}).
std::vector<Pt> ortho_route(Pt a, Pt b, bool h_first) {
    std::vector<Pt> mid;
    if (a == b) return mid;
    if (std::fabs(b.first - a.first) < 1e-9 ||
        std::fabs(b.second - a.second) < 1e-9)
        return mid;
    mid.push_back(h_first ? Pt{b.first, a.second} : Pt{a.first, b.second});
    return mid;
}

// Does the segment ab lie on (or pass through) `p` strictly between its
// endpoints? Used to detect T-junctions at corners added by ortho_fix.
bool segment_hits(const std::vector<std::pair<Pt, Pt>>& segs, Pt p,
                  double tol = 1.0) {
    for (const auto& s : segs) {
        if (std::hypot(s.first.first - p.first, s.first.second - p.second) <= tol ||
            std::hypot(s.second.first - p.first, s.second.second - p.second) <= tol)
            continue; // ignore endpoint matches
        double vx = s.second.first - s.first.first,
               vy = s.second.second - s.first.second;
        double wx = p.first - s.first.first, wy = p.second - s.first.second;
        double L2 = vx * vx + vy * vy;
        if (L2 < 1e-9) continue;
        double t = (wx * vx + wy * vy) / L2;
        if (t <= 0.0 || t >= 1.0) continue;
        double px = s.first.first + t * vx, py = s.first.second + t * vy;
        if (std::hypot(p.first - px, p.second - py) <= tol) return true;
    }
    return false;
}

// Orthogonalise a wire: walk its polyline and replace every diagonal
// segment with two axis-aligned legs joined at a corner. The corner
// direction alternates so a series of diagonals doesn't all bend the same
// way (which would produce visible "stairs"). When `other_segs` (segments
// of OTHER wires in the schematic) is provided, the corner is placed on
// the side that doesn't land on another wire -- otherwise the corner
// becomes an unintended T-junction and the schematic fills with solder
// dots every time the user drags a component. The wire's general shape is
// preserved; only the leg directions are normalised. This is what schematic
// editors (Cadence Virtuoso, KiCad, etc.) do when you drag a component
// through its wires -- every connected wire stays rectangular.
void ortho_fix_wire(std::vector<Pt>& pts,
                    const std::vector<std::pair<Pt, Pt>>& other_segs = {}) {
    if (pts.size() < 2) return;
    std::vector<Pt> out;
    out.push_back(pts[0]);
    bool prev_was_horizontal = false;
    for (size_t i = 0; i + 1 < pts.size(); ++i) {
        Pt a = pts[i];
        Pt b = pts[i + 1];
        bool horiz = std::fabs(b.second - a.second) < 1e-9;
        bool vert = std::fabs(b.first - a.first) < 1e-9;
        if (horiz || vert) {
            out.push_back(b);
            prev_was_horizontal = horiz;
            continue;
        }
        // Diagonal. Two corners are valid: (b.x, a.y) and (a.x, b.y). Pick
        // the one whose resulting corner doesn't land on another wire's
        // middle; if both are clear, alternate from the previous leg so a
        // run of diagonals doesn't stack the bend on the same side.
        Pt c1{b.first, a.second}; // horizontal-first corner
        Pt c2{a.first, b.second}; // vertical-first corner
        bool h_first;
        if (other_segs.empty()) {
            h_first = !prev_was_horizontal;
        } else {
            bool c1_clean = !segment_hits(other_segs, c1);
            bool c2_clean = !segment_hits(other_segs, c2);
            if (c1_clean && !c2_clean) h_first = true;
            else if (c2_clean && !c1_clean) h_first = false;
            else h_first = !prev_was_horizontal; // both clean (or both busy)
        }
        Pt corner = h_first ? c1 : c2;
        if (std::fabs(out.back().first - corner.first) > 1e-9 ||
            std::fabs(out.back().second - corner.second) > 1e-9)
            out.push_back(corner);
        out.push_back(b);
        prev_was_horizontal = !h_first;
    }
    pts = std::move(out);
}
} // namespace

SchematicCanvas::SchematicCanvas(wxWindow* parent, Document* doc)
    : wxWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
               wxWANTS_CHARS | wxFULL_REPAINT_ON_RESIZE),
      doc_(doc) {
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetBackgroundColour(*wxWHITE);
    SetFocus();
}

// ---------------------------------------------------------------------------
// tool / mode
// ---------------------------------------------------------------------------
void SchematicCanvas::set_tool(Tool t, Kind k) {
    // Preserve the ghost (and its rotation/flips) when re-selecting the same
    // placement kind, so shortcuts like Space are not undone by a palette sync.
    if (t == Tool::Place && placing_ && k == place_kind_) {
        tool_ = t;
        return;
    }
    tool_ = t;
    place_kind_ = k;
    place_rot_ = 0;
    place_flip_h_ = place_flip_v_ = false;
    placing_ = (t == Tool::Place);
    wire_finish();
    label_queue_.clear();
    box_selecting_ = false;
    SetFocus();
    Refresh();
}

void SchematicCanvas::begin_place(Kind k, int rot) {
    // Keep the current ghost (rotation/flips) if the same kind is already
    // being placed, so a palette re-selection does not undo Space/rotate.
    bool same = placing_ && k == place_kind_;
    tool_ = Tool::Place;
    placing_ = true;
    place_kind_ = k;
    if (!same) {
        place_rot_ = rot;
        place_flip_h_ = place_flip_v_ = false;
    }
    wire_finish();
    box_selecting_ = false;
    label_queue_.clear();
    SetFocus();
    Refresh();
}

void SchematicCanvas::begin_label(const std::string& names) {
    label_queue_.clear();
    std::string cur;
    for (char c : names) {
        if (c == ' ' || c == '\t' || c == '\n' || c == ',') {
            if (!cur.empty()) label_queue_.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) label_queue_.push_back(cur);
    if (label_queue_.empty()) return;
    tool_ = Tool::Label;
    placing_ = false;
    wire_finish();
    SetFocus();
    Refresh();
}

void SchematicCanvas::cancel_current() {
    placing_ = false;
    wire_finish(); // Esc: drop only the uncommitted segment
    label_queue_.clear();
    box_selecting_ = false;
    tool_ = Tool::Select;
    Refresh();
}

// Leave wiring mode. Any wire already committed to the document stays; only
// the in-progress (uncommitted) segment is discarded.
void SchematicCanvas::wire_finish() {
    wiring_ = false;
    wire_pts_.clear();
    wire_idx_ = -1;
}

// Commit the current segment (from the last vertex to the snapped cursor) as a
// real wire vertex. Used by Enter; clicking does the same thing.
void SchematicCanvas::wire_commit_segment() {
    if (!wiring_ || wire_pts_.empty()) return;
    Pt target = snap(to_doc(mouse_));
    Pt last = wire_pts_.back();
    if (target == last) return;

    std::vector<Pt> seg = wire_pts_;
    auto mids = ortho_route(last, target, wire_h_first_);
    for (const auto& q : mids) seg.push_back(q);
    seg.push_back(target);

    if (on_push_undo) on_push_undo();
    if (wire_idx_ < 0) {
        Wire w;
        w.pts = seg;
        doc_->wires.push_back(w);
        wire_idx_ = int(doc_->wires.size()) - 1;
    } else {
        doc_->wires[wire_idx_].pts = seg;
    }
    wire_pts_ = seg;
    notify_doc();
}

// ---------------------------------------------------------------------------
// transforms (infinite pan: the view is an origin + zoom, no scrollbars)
// ---------------------------------------------------------------------------
Pt SchematicCanvas::to_doc(const wxPoint& p) const {
    return {view_x_ + p.x / zoom_, view_y_ + p.y / zoom_};
}

Pt SchematicCanvas::to_view(Pt p) const {
    return {(p.first - view_x_) * zoom_, (p.second - view_y_) * zoom_};
}

Pt SchematicCanvas::snap(Pt p) const {
    return {std::round(p.first / kGrid) * kGrid,
            std::round(p.second / kGrid) * kGrid};
}

void SchematicCanvas::report_view() {
    if (on_view_changed) on_view_changed(zoom_, view_x_, view_y_);
}

void SchematicCanvas::set_zoom(double z, wxPoint anchor) {
    z = std::max(kMinZoom, std::min(kMaxZoom, z));
    if (std::fabs(z - zoom_) < 1e-12) return;
    // keep the document point under `anchor` fixed on screen
    Pt d = to_doc(anchor);
    zoom_ = z;
    view_x_ = d.first - anchor.x / zoom_;
    view_y_ = d.second - anchor.y / zoom_;
    report_view();
    Refresh(false);
}

// ---------------------------------------------------------------------------
// content bounds / zoom to fit (#10)
// ---------------------------------------------------------------------------
bool SchematicCanvas::content_bounds(double& x0, double& y0, double& x1,
                                     double& y1) const {
    bool any = false;
    x0 = y0 = 1e300;
    x1 = y1 = -1e300;
    auto acc = [&](double x, double y) {
        any = true;
        x0 = std::min(x0, x);
        y0 = std::min(y0, y);
        x1 = std::max(x1, x);
        y1 = std::max(y1, y);
    };
    for (const auto& c : doc_->circuit.comps) {
        auto pl = doc_->placements.find(c.ref);
        if (pl == doc_->placements.end()) continue;
        double bx0, by0, bx1, by1;
        symbol_bbox(c, pl->second, bx0, by0, bx1, by1);
        acc(bx0, by0);
        acc(bx1, by1);
    }
    for (const auto& w : doc_->wires)
        for (const auto& p : w.pts) acc(p.first, p.second);
    for (const auto& l : doc_->labels) acc(l.pt.first, l.pt.second);
    return any;
}

// A point is a junction when wire segments meet there in a T or cross, i.e.
// an endpoint of one segment lies strictly inside another segment, or three or
// more segment-ends coincide. Endpoints that merely touch end-to-end are a
// corner, not a junction, and get no dot.
std::vector<Pt> SchematicCanvas::junction_pts() const {
    // Gather every segment, and every vertex with the number of segment-ends
    // landing on it.
    struct Seg { Pt a, b; };
    std::vector<Seg> segs;
    for (const auto& w : doc_->wires)
        for (size_t k = 1; k < w.pts.size(); ++k)
            segs.push_back({w.pts[k - 1], w.pts[k]});

    // ("near"/"far" are legacy Windows macros, so avoid those names.)
    auto close_to = [](Pt p, Pt q) {
        return std::hypot(p.first - q.first, p.second - q.second) <= 1.0;
    };
    auto on_span = [&](Pt p, const Seg& s) {
        // strictly between the endpoints (not at either end)
        if (close_to(p, s.a) || close_to(p, s.b)) return false;
        double vx = s.b.first - s.a.first, vy = s.b.second - s.a.second;
        double wx = p.first - s.a.first, wy = p.second - s.a.second;
        double L2 = vx * vx + vy * vy;
        if (L2 < 1e-9) return false;
        double t = (wx * vx + wy * vy) / L2;
        if (t <= 0.0 || t >= 1.0) return false;
        double px = s.a.first + t * vx, py = s.a.second + t * vy;
        return std::hypot(p.first - px, p.second - py) <= 1.0;
    };

    // Every vertex of every wire, and every segment crossing/ending-on them.
    std::vector<Pt> verts;
    for (const auto& w : doc_->wires)
        for (const auto& v : w.pts) verts.push_back(v);

    std::vector<Pt> out;
    for (const auto& v : verts) {
        int ends = 0, through = 0;
        for (const auto& s : segs) {
            if (close_to(v, s.a) || close_to(v, s.b)) ++ends;
            else if (on_span(v, s)) ++through;
        }
        // a T (endpoint on another wire's middle) or a 3+ way meeting
        if (through > 0 || ends >= 3) {
            bool dup = false;
            for (const auto& q : out)
                if (close_to(v, q)) { dup = true; break; }
            if (!dup) out.push_back(v);
        }
    }
    return out;
}

void SchematicCanvas::zoom_to_fit() {
    double x0, y0, x1, y1;
    if (!content_bounds(x0, y0, x1, y1)) {
        // nothing to frame: reset to a comfortable default view
        zoom_ = 1.0;
        view_x_ = -50.0;
        view_y_ = -50.0;
        report_view();
        Refresh(false);
        return;
    }
    wxSize cs = GetClientSize();
    if (cs.x < 30 || cs.y < 30) return;
    double w = std::max(x1 - x0, 1.0);
    double h = std::max(y1 - y0, 1.0);
    double margin = 48.0; // screen px
    double zx = (cs.x - margin) / w;
    double zy = (cs.y - margin) / h;
    zoom_ = std::max(kMinZoom, std::min(kMaxZoom, std::min(zx, zy)));
    // centre the content
    double cx = (x0 + x1) / 2;
    double cy = (y0 + y1) / 2;
    view_x_ = cx - (cs.x / 2) / zoom_;
    view_y_ = cy - (cs.y / 2) / zoom_;
    report_view();
    Refresh(false);
}

// If the viewport no longer intersects the content, pan it back so the
// components are on screen again (#10).
void SchematicCanvas::clamp_view() {
    double x0, y0, x1, y1;
    if (!content_bounds(x0, y0, x1, y1)) return;
    wxSize cs = GetClientSize();
    double vx0 = view_x_, vy0 = view_y_;
    double vx1 = view_x_ + cs.x / zoom_, vy1 = view_y_ + cs.y / zoom_;
    const double pad = 40.0 / zoom_;
    bool intersects = !(vx1 < x0 - pad || vx0 > x1 + pad ||
                        vy1 < y0 - pad || vy0 > y1 + pad);
    if (intersects) return;
    // put the content's nearest edge just inside the viewport
    if (vx1 < x0 - pad) view_x_ = x0 - pad - cs.x / zoom_;
    if (vx0 > x1 + pad) view_x_ = x1 + pad;
    if (vy1 < y0 - pad) view_y_ = y0 - pad - cs.y / zoom_;
    if (vy0 > y1 + pad) view_y_ = y1 + pad;
    Refresh(false);
}

// ---------------------------------------------------------------------------
// selection
// ---------------------------------------------------------------------------
void SchematicCanvas::set_selection(const std::string& s) {
    bool same = (sel_ == s) &&
                (sel_set_.size() == (s.empty() ? size_t(0) : size_t(1))) &&
                (s.empty() || sel_set_.count(s) == 1);
    if (same) return;
    sel_ = s;
    sel_set_.clear();
    if (!s.empty()) sel_set_.insert(s);
    notify_sel();
    Refresh();
}

Selection SchematicCanvas::selection_info() const {
    Selection s;
    if (sel_.empty()) return s;
    if (sel_[0] != '#') {
        s.type = Selection::Component;
        s.ref = sel_;
        return s;
    }
    if (sel_.rfind("#wire", 0) == 0) {
        int colon = int(sel_.find(':'));
        std::string nums = colon < 0 ? sel_.substr(5)
                                     : sel_.substr(5, colon - 5);
        s.type = colon < 0 ? Selection::Wire : Selection::WireSegment;
        s.wire = std::atoi(nums.c_str());
        if (colon >= 0) s.seg = std::atoi(sel_.c_str() + colon + 1);
        return s;
    }
    if (sel_.rfind("#label", 0) == 0) {
        s.type = Selection::Label;
        s.label = std::atoi(sel_.c_str() + 6);
        return s;
    }
    return s;
}

// ---------------------------------------------------------------------------
// hit testing
// ---------------------------------------------------------------------------
std::string SchematicCanvas::hit_component(Pt p) const {
    // Only the symbol body's own box (small pad) counts as a hit, so a click
    // near a pin lands on the wire attached there instead of on the symbol.
    for (auto it = doc_->circuit.comps.rbegin();
         it != doc_->circuit.comps.rend(); ++it) {
        auto pl = doc_->placements.find(it->ref);
        if (pl == doc_->placements.end()) continue;
        double x0, y0, x1, y1;
        symbol_bbox(*it, pl->second, x0, y0, x1, y1, 4.0);
        if (p.first >= x0 && p.first <= x1 && p.second >= y0 && p.second <= y1)
            return it->ref;
    }
    return "";
}

int SchematicCanvas::hit_pin(const std::string& ref, Pt p) const {
    const Component* c = doc_->circuit.find(ref);
    if (!c) return -1;
    auto pl = doc_->placements.find(ref);
    if (pl == doc_->placements.end()) return -1;
    int n = int(pin_offsets(c->kind).size());
    for (int i = 0; i < n; ++i)
        if (dist(pin_world(*c, pl->second, i), p) <= kSnapR) return i;
    return -1;
}

int SchematicCanvas::hit_any_pin(Pt p, std::string& ref) const {
    for (const auto& c : doc_->circuit.comps) {
        auto pl = doc_->placements.find(c.ref);
        if (pl == doc_->placements.end()) continue;
        int n = int(pin_offsets(c.kind).size());
        for (int i = 0; i < n; ++i)
            if (dist(pin_world(c, pl->second, i), p) <= kSnapR) {
                ref = c.ref;
                return i;
            }
    }
    ref.clear();
    return -1;
}

bool SchematicCanvas::hit_wire(Pt p, int& idx) const {
    for (int i = int(doc_->wires.size()) - 1; i >= 0; --i) {
        const auto& w = doc_->wires[i];
        for (size_t k = 1; k < w.pts.size(); ++k)
            if (seg_dist(p, w.pts[k - 1], w.pts[k]) <= kSnapR) {
                idx = i;
                return true;
            }
    }
    return false;
}

bool SchematicCanvas::hit_wire_segment(Pt p, int& idx, int& seg) const {
    for (int i = int(doc_->wires.size()) - 1; i >= 0; --i) {
        const auto& w = doc_->wires[i];
        for (size_t k = 1; k < w.pts.size(); ++k)
            if (seg_dist(p, w.pts[k - 1], w.pts[k]) <= kSnapR) {
                idx = i;
                seg = int(k) - 1;
                return true;
            }
    }
    return false;
}

bool SchematicCanvas::hit_label(Pt p, int& idx) const {
    for (int i = int(doc_->labels.size()) - 1; i >= 0; --i) {
        const auto& l = doc_->labels[i];
        Pt a = l.pt;
        // clickable area: the text drawn around `pt` and the anchor dot
        double fs = std::max(8, l.font_size);
        double halfw =
            std::max(14.0, fs * 0.32 * std::max<size_t>(1, l.name.size()));
        double top = a.second - fs * 1.4;
        if (p.first >= a.first - halfw && p.first <= a.first + halfw &&
            p.second >= top && p.second <= a.second + 8) {
            idx = i;
            return true;
        }
        if (dist(a, p) <= kSnapR) {
            idx = i;
            return true;
        }
        if (dist(l.anchor, p) <= kSnapR) {
            idx = i;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// painting
// ---------------------------------------------------------------------------
void SchematicCanvas::on_paint(wxPaintEvent&) {
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(*wxWHITE));
    dc.Clear();

    wxSize cs = GetClientSize();
    if (cs.x <= 0 || cs.y <= 0) return;

    // Map document coordinates to the window: a logical point (x,y) is drawn
    // at (x*zoom + origin). This is what keeps symbols, wires, labels and the
    // ghost all in the same space as the hit-testing (to_view).
    dc.SetUserScale(zoom_, zoom_);
    dc.SetDeviceOrigin(int(std::lround(-view_x_ * zoom_)),
                       int(std::lround(-view_y_ * zoom_)));

    // viewport in document units
    double x0 = view_x_, y0 = view_y_;
    double x1 = view_x_ + cs.x / zoom_, y1 = view_y_ + cs.y / zoom_;

    // grid: faint lines, spaced so they never crowd on screen
    dc.SetPen(wxPen(wxColour(232, 232, 236)));
    double step = kGrid;
    while (step * zoom_ < 22.0) step *= 2.0;
    for (double x = std::floor(x0 / step) * step; x < x1 + step; x += step)
        dc.DrawLine(wxPoint(int(x), int(y0)), wxPoint(int(x), int(y1)));
    for (double y = std::floor(y0 / step) * step; y < y1 + step; y += step)
        dc.DrawLine(wxPoint(int(x0), int(y)), wxPoint(int(x1), int(y)));

    auto doc_line = [&](Pt a, Pt b, const wxColour& col, int w) {
        dc.SetPen(wxPen(col, w));
        dc.DrawLine(wxPoint(int(a.first), int(a.second)),
                    wxPoint(int(b.first), int(b.second)));
    };
    auto doc_circle = [&](Pt c, double r_px, const wxColour& col, bool filled) {
        dc.SetPen(filled ? *wxTRANSPARENT_PEN : wxPen(col, 2));
        dc.SetBrush(filled ? wxBrush(col) : *wxTRANSPARENT_BRUSH);
        dc.DrawCircle(wxPoint(int(c.first), int(c.second)),
                      int(std::lround(r_px / zoom_)));
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
    };
    auto on_screen = [&](double x, double y, double m) {
        return x >= x0 - m && x <= x1 + m && y >= y0 - m && y <= y1 + m;
    };

    // wires
    Selection si = selection_info();
    for (size_t i = 0; i < doc_->wires.size(); ++i) {
        const auto& w = doc_->wires[i];
        bool whole = si.type == Selection::Wire && si.wire == int(i);
        for (size_t k = 1; k < w.pts.size(); ++k) {
            const Pt& a = w.pts[k - 1];
            const Pt& b = w.pts[k];
            if (std::max(a.first, b.first) < x0 - 40 ||
                std::min(a.first, b.first) > x1 + 40 ||
                std::max(a.second, b.second) < y0 - 40 ||
                std::min(a.second, b.second) > y1 + 40)
                continue;
            bool seg_sel = si.type == Selection::WireSegment &&
                           si.wire == int(i) && si.seg == int(k) - 1;
            doc_line(a, b,
                     (seg_sel || whole) ? wxColour(0, 92, 200) : wxColour(0, 0, 0),
                     (seg_sel || whole) ? 3 : 2);
        }
    }

    // solder dots at T-junctions / 3-way meetings (item: perpendicular wire
    // into the middle of another must read as a real connection)
    for (const auto& j : junction_pts()) {
        if (!on_screen(j.first, j.second, 20)) continue;
        doc_circle(j, 4, wxColour(0, 0, 0), true);
    }

    // wire in progress: the current (uncommitted) segment, from the last
    // vertex to the snapped cursor
    if (wiring_ && !wire_pts_.empty()) {
        Pt last = wire_pts_.back();
        Pt m = snap(to_doc(mouse_));
        auto mids = ortho_route(last, m, wire_h_first_);
        Pt prev = last;
        for (const auto& q : mids) {
            doc_line(prev, q, wxColour(0, 120, 200), 2);
            prev = q;
        }
        doc_line(prev, m, wxColour(0, 120, 200), 2);
    }

    // net labels (font size is per-label; drawn above the anchor point)
    wxFont pre_labels_font = dc.GetFont();
    for (size_t i = 0; i < doc_->labels.size(); ++i) {
        const auto& l = doc_->labels[i];
        if (!on_screen(l.pt.first, l.pt.second, 80)) continue;
        if (l.name.empty()) continue;
        bool is_sel = sel_ == "#label" + std::to_string(i);
        wxFont f(l.font_size, wxFONTFAMILY_SWISS, wxFONTSTYLE_NORMAL,
                 wxFONTWEIGHT_NORMAL);
        dc.SetFont(f);
        wxString txt = wxString::FromUTF8(l.name);
        wxSize ts = dc.GetTextExtent(txt); // logical units (DC is scaled)
        int rot = ((l.rot % 360) + 360) % 360;
        // Position the text so the centre-bottom of the glyph sits at `pt`,
        // and rotate around that anchor. When rotated 90/270 we use the same
        // visible footprint: a centred box around `pt`.
        wxPoint anchor(int(l.pt.first), int(l.pt.second));
        if (is_sel) {
            dc.SetBrush(wxBrush(wxColour(0, 92, 200)));
            dc.SetPen(*wxTRANSPARENT_PEN);
            int bw = ts.x + 6, bh = ts.y + 2;
            if (rot == 90 || rot == 270) std::swap(bw, bh);
            dc.DrawRectangle(wxRect(anchor.x - bw / 2, anchor.y - bh / 2,
                                    bw, bh));
            dc.SetBrush(*wxTRANSPARENT_BRUSH);
            dc.SetTextForeground(*wxWHITE);
        } else {
            dc.SetTextForeground(wxColour(0, 0, 0));
        }
        if (rot == 0 || rot == 180) {
            dc.DrawText(txt,
                        wxPoint(anchor.x - ts.x / 2, anchor.y - ts.y / 2));
        } else {
            dc.DrawRotatedText(txt, anchor.x, anchor.y, rot);
        }
        dc.SetTextForeground(*wxBLACK);

        // Anchor dot (red) shows where on the net this label sits; useful so
        // the user can see that dragging the text does not detach the label.
        if (std::fabs(l.anchor.first - l.pt.first) > 0.5 ||
            std::fabs(l.anchor.second - l.pt.second) > 0.5 ||
            is_sel) {
            doc_circle({l.anchor.first, l.anchor.second}, 3,
                       wxColour(200, 40, 40), true);
        }
    }
    // Restore the font before drawing components: draw_symbol() reads the
    // current DC font as its base for ref/value text, so leaving a label's
    // custom font installed would change the size of every component name.
    dc.SetFont(pre_labels_font);

    // components (multi-selection highlights all)
    for (const auto& c : doc_->circuit.comps) {
        auto pl = doc_->placements.find(c.ref);
        if (pl == doc_->placements.end()) continue;
        if (!on_screen(pl->second.x, pl->second.y, 160)) continue;
        draw_symbol(dc, c, pl->second, sel_set_.count(c.ref) > 0);
    }

    // box selection rubber band
    if (box_selecting_) {
        wxRect r(int(std::min(box_a_.first, box_b_.first)),
                 int(std::min(box_a_.second, box_b_.second)),
                 int(std::fabs(box_b_.first - box_a_.first)),
                 int(std::fabs(box_b_.second - box_a_.second)));
        dc.SetPen(wxPen(wxColour(0, 92, 200), 1, wxPENSTYLE_SHORT_DASH));
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.DrawRectangle(r);
    }

    // pending net-label placement: show the next name and its anchor point
    if (tool_ == Tool::Label && !label_queue_.empty() && has_mouse_) {
        int wi = -1;
        Pt anchor = snap(to_doc(mouse_));
        if (hit_wire(to_doc(mouse_), wi)) {
            Pt best = anchor;
            double bestd = kSnapR / zoom_;
            for (const auto& v : doc_->wires[wi].pts)
                if (dist(v, to_doc(mouse_)) < bestd) {
                    bestd = dist(v, to_doc(mouse_));
                    best = v;
                }
            anchor = best;
        }
        doc_circle(anchor, 5, wxColour(200, 40, 40), false);
        wxFont saved = dc.GetFont();
        wxFont f(9, wxFONTFAMILY_SWISS, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD);
        dc.SetFont(f);
        wxString txt = wxString::FromUTF8(label_queue_.front());
        wxSize ts = dc.GetTextExtent(txt);
        dc.SetTextForeground(wxColour(0, 92, 200));
        dc.DrawText(txt, wxPoint(int(anchor.first - ts.x / 2.0),
                                 int(anchor.second - ts.y - 6.0)));
        dc.SetTextForeground(*wxBLACK);
        dc.SetFont(saved);
    }

    // red dot while in wiring mode: jumps to the nearest valid grid point so
    // it shows exactly where the segment/vertex will land
    if (tool_ == Tool::Wire && has_mouse_) {
        Pt g = snap(to_doc(mouse_));
        doc_circle(g, 4, wxColour(210, 30, 30), true);
    }

    // ghost of component being placed
    if (placing_ && has_mouse_) {
        Pt m = snap(to_doc(mouse_));
        Component tmp;
        tmp.ref = "";
        tmp.kind = place_kind_;
        Placement pl{m.first, m.second, place_rot_};
        pl.flip_h = place_flip_h_;
        pl.flip_v = place_flip_v_;
        dc.SetPen(wxPen(wxColour(0, 120, 200), 1, wxPENSTYLE_SHORT_DASH));
        draw_symbol(dc, tmp, pl, false);
    }

    // Net-name tooltip near the cursor (not snapped to the grid; floats with
    // the cursor as the user moves it across a wire or a pin). Drawn last so
    // it sits on top of everything else.
    if (!hover_net_.empty() && has_mouse_) {
        // Remember the DC font so we can restore it; the canvas's default font
        // is shared with the components' value/ref labels (the text on each
        // symbol), so an unrestored font would make them reflow between paints.
        wxFont old = dc.GetFont();
        wxFont f(12, wxFONTFAMILY_SWISS, wxFONTSTYLE_NORMAL,
                 wxFONTWEIGHT_BOLD);
        dc.SetFont(f);
        wxString txt = wxString::FromUTF8(hover_net_);
        wxSize ts = dc.GetTextExtent(txt);
        // Anchor the tooltip up-and-to-the-left of the cursor in *screen*
        // units, so it stays glued to the cursor regardless of zoom and never
        // sits under the cursor (where it would obscure the wire).
        int tx = mouse_.x - ts.x - 12;
        int ty = mouse_.y - ts.y - 12;
        // If the tooltip would go off the left/top edge, flip it.
        if (tx < 0) tx = mouse_.x + 14;
        if (ty < 0) ty = mouse_.y + 14;
        dc.SetPen(wxPen(wxColour(80, 80, 90)));
        dc.SetBrush(wxBrush(wxColour(255, 252, 220)));
        dc.DrawRectangle(wxRect(tx - 4, ty - 2, ts.x + 8, ts.y + 4));
        dc.SetTextForeground(wxColour(40, 40, 40));
        dc.DrawText(txt, wxPoint(tx, ty));
        dc.SetTextForeground(*wxBLACK);
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.SetFont(old);
    }
}

// ---------------------------------------------------------------------------
// mouse
// ---------------------------------------------------------------------------
void SchematicCanvas::on_left_down(wxMouseEvent& e) {
    SetFocus();
    Pt p = to_doc(e.GetPosition());
    mouse_ = e.GetPosition();
    has_mouse_ = true;
    bool shift = e.ShiftDown();

    switch (tool_) {
    case Tool::Place: {
        Pt s = snap(p);
        Component c;
        c.kind = place_kind_;
        if (on_push_undo) on_push_undo();
        std::string ref = doc_->add(c, s.first, s.second);
        auto pl = doc_->placements.find(ref);
        if (pl != doc_->placements.end()) {
            pl->second.rot = place_rot_;
            pl->second.flip_h = place_flip_h_;
            pl->second.flip_v = place_flip_v_;
        }
        notify_doc();
        set_selection(ref);
        // one component per key press: drop back to Select so the next click
        // does not place again (and Escape is not needed)
        placing_ = false;
        tool_ = Tool::Select;
        break;
    }
    case Tool::Delete: {
        int wi, li;
        std::string ref = hit_component(p);
        if (!ref.empty()) {
            if (on_push_undo) on_push_undo();
            doc_->remove(ref);
            set_selection("");
        } else if (hit_wire(p, wi)) {
            if (on_push_undo) on_push_undo();
            doc_->wires.erase(doc_->wires.begin() + wi);
        } else if (hit_label(p, li)) {
            if (on_push_undo) on_push_undo();
            doc_->labels.erase(doc_->labels.begin() + li);
        }
        notify_doc();
        break;
    }
    case Tool::Wire: {
        std::string ref;
        int pin = hit_any_pin(p, ref);
        Pt target = snap(p);
        if (pin >= 0) {
            const Component* c = doc_->circuit.find(ref);
            auto pl = doc_->placements.find(ref);
            if (c && pl != doc_->placements.end())
                target = pin_world(*c, pl->second, pin); // exact pin position
        }

        // Entering wiring mode: the first click sets the start point (a pin or
        // a grid point). Each later click commits a segment, so what you drew
        // stays visible, and terminating on a pin ends the mode.
        if (!wiring_) {
            wiring_ = true;
            wire_h_first_ = true;
            wire_pts_.clear();
            wire_pts_.push_back(target);
            wire_idx_ = -1;
            Refresh(false);
            break;
        }

        Pt last = wire_pts_.back();
        if (target == last) break;

        // commit the segment (visible from now on)
        std::vector<Pt> seg = wire_pts_;
        auto mids = ortho_route(last, target, wire_h_first_);
        for (const auto& q : mids) seg.push_back(q);
        seg.push_back(target);
        if (on_push_undo) on_push_undo();
        if (wire_idx_ < 0) {
            Wire w;
            w.pts = seg;
            doc_->wires.push_back(w);
            wire_idx_ = int(doc_->wires.size()) - 1;
        } else {
            doc_->wires[wire_idx_].pts = seg;
        }
        wire_pts_ = seg;

        bool on_wire_node = false;
        for (size_t wi = 0; wi < doc_->wires.size() && !on_wire_node; ++wi) {
            if (int(wi) == wire_idx_) continue;
            for (const auto& v : doc_->wires[wi].pts)
                if (dist(v, target) < 1.0) { on_wire_node = true; break; }
        }

        notify_doc();
        if (pin >= 0 || on_wire_node) {
            // terminated: leave wiring mode (the committed wire stays)
            wire_finish();
            Refresh();
        } else {
            Refresh(false);
        }
        break;
    }
    case Tool::Label: {
        if (label_queue_.empty()) break;
        if (on_push_undo) on_push_undo();
        NetLabel l;
        int wi = -1;
        Pt target = snap(p);
        // The anchor is what attaches to the net; if the click is on a wire,
        // snap to the wire. Otherwise anchor at the grid point itself.
        Pt anchor = target;
        if (hit_wire(p, wi) && wi >= 0) {
            Pt best = anchor;
            double bestd = 1e300;
            const auto& w = doc_->wires[wi];
            for (size_t k = 1; k < w.pts.size(); ++k) {
                Pt a = w.pts[k - 1], b = w.pts[k];
                double vx = b.first - a.first, vy = b.second - a.second;
                double L2 = vx * vx + vy * vy;
                double t = L2 > 1e-12
                               ? ((p.first - a.first) * vx +
                                  (p.second - a.second) * vy) / L2
                               : 0.0;
                t = std::max(0.0, std::min(1.0, t));
                Pt q{a.first + t * vx, a.second + t * vy};
                if (dist(q, p) < bestd) { bestd = dist(q, p); best = q; }
            }
            anchor = best;
        }
        l.anchor = anchor;
        l.name = label_queue_.front();
        l.font_size = 9; // matches the component ref/value text size
        l.rot = 0;
        // Put the text on the readable side of the wire (right of a vertical
        // wire, above a horizontal one) instead of straddling it.
        l.pt = doc_->label_display_pt(wi, anchor, int(l.name.size()));
        doc_->labels.push_back(l);
        label_queue_.erase(label_queue_.begin());
        if (label_queue_.empty()) tool_ = Tool::Select;
        notify_doc();
        break;
    }
    case Tool::Select: {
        std::string ref = hit_component(p);
        if (!ref.empty()) {
            bool already = sel_set_.count(ref) > 0;
            if (shift) {
                // toggle this component in the multi-selection
                if (already) {
                    sel_set_.erase(ref);
                    sel_ = sel_set_.empty() ? "" : *sel_set_.begin();
                } else {
                    sel_set_.insert(ref);
                    sel_ = ref;
                }
                notify_sel();
                Refresh();
            } else if (!already) {
                // click a fresh component: select just it
                sel_set_.clear();
                sel_set_.insert(ref);
                sel_ = ref;
                notify_sel();
                Refresh();
            }
            // (clicking an already-selected component keeps the whole group)
            dragging_ = true;
            drag_start_.clear();
            drag_anchor_ = p;
            for (const auto& r : sel_set_)
                if (auto pl = doc_->placements.find(r);
                    pl != doc_->placements.end())
                    drag_start_.push_back({r, {pl->second.x, pl->second.y}});
            return;
        }
        int wi, li, sg;
        if (hit_wire_segment(p, wi, sg)) {
            sel_set_.clear();
            sel_ = "#wire" + std::to_string(wi) + ":" + std::to_string(sg);
            sel_set_.insert(sel_);
            notify_sel();
            if (on_wire_selected) {
                on_wire_selected(wi, doc_->net_name_of_wire(wi));
            }
            // Start a wire-segment drag so it carries connected geometry.
            if (wi >= 0 && wi < int(doc_->wires.size())) {
                drag_wire_ = wi;
                drag_wire_pts_backup_ = doc_->wires[wi].pts;
                drag_anchor_ = p;
                drag_start_.clear();
                drag_label_ = -1;
                dragging_ = true;
            }
            Refresh();
            return;
        }
        if (hit_wire(p, wi)) {
            // Click on the wire body (not a tight segment hit) selects the
            // whole wire; dragging translates every vertex and any connected
            // geometry attached at a shared vertex.
            sel_set_.clear();
            sel_ = "#wire" + std::to_string(wi);
            sel_set_.insert(sel_);
            notify_sel();
            if (on_wire_selected) {
                on_wire_selected(wi, doc_->net_name_of_wire(wi));
            }
            if (wi >= 0 && wi < int(doc_->wires.size())) {
                drag_wire_ = wi;
                drag_wire_pts_backup_ = doc_->wires[wi].pts;
                drag_anchor_ = p;
                drag_start_.clear();
                drag_label_ = -1;
                dragging_ = true;
            }
            Refresh();
            return;
        }
        if (hit_label(p, li)) {
            sel_set_.clear();
            std::string lsel = "#label" + std::to_string(li);
            sel_ = lsel;
            sel_set_.insert(lsel);
            notify_sel();
            // Labels are moveable but the wire they name is *not* -- moving
            // changes only `pt` (the display position). `anchor` stays.
            if (li >= 0 && li < int(doc_->labels.size())) {
                drag_label_ = li;
                drag_label_pt_backup_ = doc_->labels[li].pt;
                drag_label_anchor_backup_ = doc_->labels[li].anchor;
                drag_anchor_ = p;
                drag_start_.clear();
                drag_wire_ = -1;
                dragging_ = true;
            }
            Refresh();
            return;
        }
        // empty space: begin a box selection (#4)
        sel_ = "";
        sel_set_.clear();
        notify_sel();
        box_selecting_ = true;
        box_a_ = box_b_ = p;
        Refresh(false);
        break;
    }
    }
}

void SchematicCanvas::on_left_up(wxMouseEvent&) {
    if (box_selecting_) {
        box_selecting_ = false;
        // select every component and wire whose geometry intersects the box
        double x0 = std::min(box_a_.first, box_b_.first);
        double x1 = std::max(box_a_.first, box_b_.first);
        double y0 = std::min(box_a_.second, box_b_.second);
        double y1 = std::max(box_a_.second, box_b_.second);
        if (std::fabs(x1 - x0) > 2 || std::fabs(y1 - y0) > 2) {
            sel_set_.clear();
            for (const auto& c : doc_->circuit.comps) {
                auto pl = doc_->placements.find(c.ref);
                if (pl == doc_->placements.end()) continue;
                double bx0, by0, bx1, by1;
                symbol_bbox(c, pl->second, bx0, by0, bx1, by1);
                bool hit = !(bx1 < x0 || bx0 > x1 || by1 < y0 || by0 > y1);
                if (hit) sel_set_.insert(c.ref);
            }
            // Wires whose bounding box (per-vertex) intersects the box.
            for (size_t i = 0; i < doc_->wires.size(); ++i) {
                bool hit = false;
                for (const auto& pt : doc_->wires[i].pts)
                    if (pt.first >= x0 && pt.first <= x1 &&
                        pt.second >= y0 && pt.second <= y1) {
                        hit = true;
                        break;
                    }
                if (hit) sel_set_.insert("#wire" + std::to_string(i));
            }
            sel_ = sel_set_.empty() ? "" : *sel_set_.begin();
            notify_sel();
        }
        Refresh();
        return;
    }
    if (dragging_) {
        dragging_ = false;
        // wire / label drags already wrote into the doc; nudge dirty + repaint
        if (drag_wire_ >= 0 || drag_label_ >= 0) {
            drag_wire_ = -1;
            drag_wire_pts_backup_.clear();
            drag_label_ = -1;
            notify_doc();
        } else {
            notify_doc();
        }
    }
}

void SchematicCanvas::on_left_dclick(wxMouseEvent& e) {
    // Double-click ends wiring mode; the segments already committed stay.
    if (tool_ == Tool::Wire && wiring_) {
        notify_doc();
        wire_finish();
        Refresh();
        return;
    }
    e.Skip();
}

void SchematicCanvas::on_motion(wxMouseEvent& e) {
    mouse_ = e.GetPosition();
    has_mouse_ = true;
    Pt p = to_doc(mouse_);

    // right-button drag pans the view. Mouse capture keeps the events coming
    // even when the cursor leaves the window, so panning is unbounded (#3).
    if (panning_ && HasCapture()) {
        int dx = mouse_.x - pan_last_.x;
        int dy = mouse_.y - pan_last_.y;
        pan_total_ += std::abs(dx) + std::abs(dy);
        view_x_ -= dx / zoom_;
        view_y_ -= dy / zoom_;
        pan_last_ = mouse_;
        report_view();
        Refresh(false);
        return;
    }

    if (box_selecting_) {
        box_b_ = p;
        Refresh(false);
        return;
    }
    if (dragging_ && !drag_start_.empty()) {
        // absolute grid-snapped offset from where the drag began
        double dx = p.first - drag_anchor_.first;
        double dy = p.second - drag_anchor_.second;
        bool moved = false;
        for (const auto& ds : drag_start_) {
            auto pl = doc_->placements.find(ds.first);
            if (pl == doc_->placements.end()) continue;
            double nx = std::round((ds.second.first + dx) / kGrid) * kGrid;
            double ny = std::round((ds.second.second + dy) / kGrid) * kGrid;
            if (nx == pl->second.x && ny == pl->second.y) continue;
            move_component(ds.first, nx, ny);
            moved = true;
        }
        if (moved) Refresh(false);
        return;
    }

    if (dragging_ && drag_label_ >= 0 && drag_label_ < int(doc_->labels.size())) {
        // Labels translate only `pt` -- the `anchor` (which names the net)
        // stays where it is, so a label dragged off the wire still names it.
        double nx = std::round((drag_label_pt_backup_.first + p.first -
                                drag_anchor_.first) / kGrid) * kGrid;
        double ny = std::round((drag_label_pt_backup_.second + p.second -
                                drag_anchor_.second) / kGrid) * kGrid;
        Pt cur = doc_->labels[drag_label_].pt;
        if (cur.first != nx || cur.second != ny) {
            doc_->labels[drag_label_].pt = {nx, ny};
            // anchor unchanged
            Refresh(false);
        }
        return;
    }

    if (dragging_ && drag_wire_ >= 0 &&
        drag_wire_ < int(doc_->wires.size()) &&
        !drag_wire_pts_backup_.empty()) {
        // Whole-wire or wire-segment drag: translate the dragged vertices and
        // any tributary geometry that shares an old vertex (other wires'
        // coincident endpoints; labels whose anchor sits on a moved vertex).
        // A wire segment (sel_ = "#wireN:segM") moves only that segment's two
        // endpoints; tributaries are still carried.
        Selection si = selection_info();
        bool segment = (si.type == Selection::WireSegment);
        int seg_a = -1, seg_b = -1;
        if (segment) {
            seg_a = si.seg;
            seg_b = si.seg + 1;
        }
        double dx = std::round((p.first - drag_anchor_.first) / kGrid) * kGrid;
        double dy = std::round((p.second - drag_anchor_.second) / kGrid) * kGrid;

        // New positions for the dragged wire's vertices.
        std::vector<Pt> new_pts = drag_wire_pts_backup_;
        for (size_t k = 0; k < new_pts.size(); ++k) {
            if (segment && int(k) != seg_a && int(k) != seg_b) continue;
            new_pts[k].first += dx;
            new_pts[k].second += dy;
        }
        // Old -> new mapping for the moved vertices, so other wires can
        // follow exactly those endpoints.
        std::map<std::pair<double, double>, Pt> moved;
        for (size_t k = 0; k < new_pts.size(); ++k) {
            Pt& o = drag_wire_pts_backup_[k];
            Pt& n = new_pts[k];
            if (segment && int(k) != seg_a && int(k) != seg_b) continue;
            if (o.first != n.first || o.second != n.second)
                moved[{o.first, o.second}] = n;
        }
        // Apply to the dragged wire itself.
        doc_->wires[drag_wire_].pts = new_pts;

        // Carry other wires whose vertices coincide with a moved vertex.
        for (size_t wi = 0; wi < doc_->wires.size(); ++wi) {
            if (int(wi) == drag_wire_) continue;
            bool any = false;
            for (auto& v : doc_->wires[wi].pts) {
                auto it = moved.find({v.first, v.second});
                if (it != moved.end()) {
                    v = it->second;
                    any = true;
                }
            }
            if (any) (void)0;
        }
        // Carry labels whose anchor sits on a moved vertex.
        for (auto& l : doc_->labels) {
            auto it = moved.find({l.anchor.first, l.anchor.second});
            if (it == moved.end()) continue;
            Pt da = it->second;
            Pt old = {it->first.first, it->first.second};
            double ddx = da.first - old.first;
            double ddy = da.second - old.second;
            l.anchor = da;
            l.pt.first += ddx;
            l.pt.second += ddy;
        }
        Refresh(false);
        return;
    }

    std::string nh;
    if (tool_ == Tool::Wire) {
        std::string ref;
        nh = hit_any_pin(p, ref) >= 0 ? "pin:" + ref : "wire";
    } else if (tool_ == Tool::Label) {
        nh = "label";
    } else if (tool_ == Tool::Delete) {
        int wi, li;
        if (!hit_component(p).empty()) nh = "del-comp";
        else if (hit_wire(p, wi)) nh = "del-wire";
        else if (hit_label(p, li)) nh = "del-label";
    } else {
        std::string ref;
        if (hit_any_pin(p, ref) >= 0) nh = "pin:" + ref;
        else nh = hit_component(p);
    }
    if (nh != hover_) {
        hover_ = nh;
        if (on_status) {
            std::string msg;
            if (hover_.empty()) msg = "";
            else if (hover_ == "wire")
                msg = wiring_ ? "Click to place a segment; click a pin to "
                                "terminate; Enter ends, Esc cancels"
                              : "Click to start a wire; the red dot snaps to "
                                "the grid";
            else if (hover_ == "label")
                msg = "Click a net to place the label; Esc stops";
            else if (hover_ == "del-comp") msg = "Click to delete component";
            else if (hover_ == "del-wire") msg = "Click to delete wire";
            else if (hover_ == "del-label") msg = "Click to delete label";
            else if (hover_.rfind("pin:", 0) == 0)
                msg = "Pin of " + hover_.substr(4);
            else msg = hover_;
            on_status(msg);
        }
    }
    // Repaint whenever something follows the cursor: the placement ghost, the
    // wire rubber band / red cursor dot (both before and after the first
    // click), or the pending net label. The hover tooltip also follows the
    // cursor smoothly and is painted in on_paint, so we need a repaint
    // whenever the hover net changes or the cursor moves while one is shown.
    std::string hn;
    {
        std::string ref;
        int pin = hit_any_pin(p, ref);
        if (pin >= 0) {
            hn = net_name_pin_cached(ref, pin);
        } else {
            int wi = -1;
            if (hit_wire(p, wi)) hn = net_name_wire_cached(wi);
        }
    }
    if (hn != hover_net_) {
        hover_net_ = hn;
        Refresh(false);
    } else if (!hn.empty()) {
        // tooltip follows the cursor smoothly: repaint every motion while a
        // net is hovered.
        Refresh(false);
    }
    if (wiring_ || tool_ == Tool::Place || tool_ == Tool::Label ||
        tool_ == Tool::Wire)
        Refresh(false);
}

void SchematicCanvas::on_right_down(wxMouseEvent& e) {
    panning_ = true;
    pan_total_ = 0;
    pan_last_ = e.GetPosition();
    mouse_ = e.GetPosition();
    has_mouse_ = true;
    CaptureMouse();
}

void SchematicCanvas::on_right_up(wxMouseEvent&) {
    if (panning_) {
        panning_ = false;
        if (HasCapture()) ReleaseMouse();
        // a click without a real drag cancels the current tool / wire
        if (pan_total_ <= 3) {
            if (wiring_ || placing_ || tool_ == Tool::Label) {
                cancel_current();
                if (on_status) on_status("");
            } else {
                set_tool(Tool::Select);
                if (on_status) on_status("");
            }
        } else {
            clamp_view(); // keep the content reachable after a long pan
        }
    }
}

void SchematicCanvas::on_capture_changed(wxMouseCaptureChangedEvent& e) {
    // safety net: never stay stuck in panning mode
    if (!HasCapture() && panning_) {
        panning_ = false;
        clamp_view();
    }
    e.Skip();
}

void SchematicCanvas::on_mousewheel(wxMouseEvent& e) {
    mouse_ = e.GetPosition();
    has_mouse_ = true;
    double factor = e.GetWheelRotation() > 0 ? 1.12 : 1.0 / 1.12;
    double nz = zoom_ * factor;

    // Zooming out to (or past) the hard floor snaps once to "fit around the
    // components" rather than leaving the user lost on an empty sheet. This is
    // a one-shot action at the floor -- it must not fire on ordinary zoom-out
    // steps, which is what made it feel like F was being pressed repeatedly.
    if (factor < 1.0 && nz < kMinZoom) {
        zoom_to_fit();
        return;
    }
    set_zoom(nz, e.GetPosition());
}

void SchematicCanvas::on_leave(wxMouseEvent&) {
    // Do NOT release capture here: panning must keep working while the cursor
    // is outside the window (#3).
    if (tool_ == Tool::Place || wiring_ || tool_ == Tool::Label) Refresh(false);
    // Hide the hover tooltip when the cursor leaves the canvas.
    if (!hover_net_.empty()) {
        hover_net_.clear();
        Refresh(false);
    }
}

void SchematicCanvas::on_size(wxSizeEvent& e) {
    Refresh(false);
    e.Skip();
}

// ---------------------------------------------------------------------------
// transforms for moving components
// ---------------------------------------------------------------------------
void SchematicCanvas::move_component(const std::string& ref, double nx,
                                     double ny) {
    const Component* c = doc_->circuit.find(ref);
    auto pl = doc_->placements.find(ref);
    if (!c || pl == doc_->placements.end()) return;
    double dx = nx - pl->second.x;
    double dy = ny - pl->second.y;
    if (dx == 0.0 && dy == 0.0) return;

    std::vector<Pt> old_pins;
    int np = int(pin_offsets(c->kind).size());
    for (int i = 0; i < np; ++i) old_pins.push_back(pin_world(*c, pl->second, i));

    pl->second.x = nx;
    pl->second.y = ny;

    // Build a flat list of segments belonging to OTHER wires (everything
    // except the wire we're about to re-route). ortho_fix_wire uses it to
    // avoid landing new corners on another wire's middle -- which would
    // create an unintended T-junction and a stray solder dot.
    auto build_other_segs = [&](size_t skip_idx) {
        std::vector<std::pair<Pt, Pt>> segs;
        for (size_t wi = 0; wi < doc_->wires.size(); ++wi) {
            if (wi == skip_idx) continue;
            const auto& ow = doc_->wires[wi];
            for (size_t k = 1; k < ow.pts.size(); ++k)
                segs.push_back({ow.pts[k - 1], ow.pts[k]});
        }
        return segs;
    };

    for (size_t wi = 0; wi < doc_->wires.size(); ++wi) {
        auto& w = doc_->wires[wi];
        bool touched = false;
        for (auto& v : w.pts) {
            for (size_t i = 0; i < old_pins.size(); ++i) {
                if (dist(v, old_pins[i]) <= 1.0) {
                    v.first += dx;
                    v.second += dy;
                    touched = true;
                    break;
                }
            }
        }
        // Cadence-style: any wire that had a vertex dragged along with the
        // component is re-routed so the wire stays orthogonal. Without this,
        // a component moving perpendicular to its attached wire leaves a
        // diagonal stub between the pin and the first un-touched corner,
        // which looks broken and breaks the "follow the wire" intuition.
        // We pass the other wires' segments so the corner can be placed on
        // the side that doesn't create an unintended T-junction (and a
        // stray solder dot) on a bystander wire.
        if (touched) ortho_fix_wire(w.pts, build_other_segs(wi));
    }
    for (auto& l : doc_->labels) {
        for (size_t i = 0; i < old_pins.size(); ++i)
            if (dist(l.anchor, old_pins[i]) <= 1.0) {
                // Pin moved under the label's anchor: move both the anchor
                // (the net's grip on the label) and the displayed text.
                l.anchor.first += dx;
                l.anchor.second += dy;
                l.pt.first += dx;
                l.pt.second += dy;
                break;
            }
    }
}

void SchematicCanvas::notify_doc() {
    doc_->dirty = true;
    net_cache_valid_ = false;
    if (on_document_changed) on_document_changed();
    Refresh();
}

// Lazy net-map cache: rebuilt at most once per edit, so the hover tooltip's
// per-motion lookups are cheap.
const NetMap& SchematicCanvas::nets() {
    if (!net_cache_valid_) {
        net_cache_ = doc_->net_map();
        net_cache_valid_ = true;
    }
    return net_cache_;
}

std::string SchematicCanvas::net_name_wire_cached(int wi) {
    const NetMap& nm = nets();
    if (wi < 0 || wi >= int(nm.wire_root.size())) return std::string();
    auto it = nm.name.find(nm.wire_root[wi]);
    return it == nm.name.end() ? std::string() : it->second;
}

std::string SchematicCanvas::net_name_pin_cached(const std::string& ref,
                                                 int pin) {
    int ci = -1;
    for (size_t i = 0; i < doc_->circuit.comps.size(); ++i)
        if (doc_->circuit.comps[i].ref == ref) { ci = int(i); break; }
    if (ci < 0) return std::string();
    const NetMap& nm = nets();
    int r = nm.root_of_pin(ci, pin);
    auto it = nm.name.find(r);
    return it == nm.name.end() ? std::string() : it->second;
}

void SchematicCanvas::notify_sel() {
    if (on_selection_changed) on_selection_changed(sel_);
}

// ---------------------------------------------------------------------------
// transforms / editing
// ---------------------------------------------------------------------------
void SchematicCanvas::rotate_ghost(int delta) {
    if (placing_) {
        place_rot_ = ((place_rot_ + delta) % 360 + 360) % 360;
        Refresh();
        return;
    }
    if (sel_set_.empty()) return;
    if (on_push_undo) on_push_undo();
    bool any = false;
    for (const auto& r : sel_set_) {
        if (r.empty() || r[0] == '#') continue;
        auto pl = doc_->placements.find(r);
        if (pl == doc_->placements.end()) continue;
        const Component* c = doc_->circuit.find(r);
        if (!c) continue;
        // Snapshot pin world positions BEFORE the rotation. Any wire vertex
        // coincident with an old pin gets re-anchored to the new pin position
        // and the whole wire is re-routed to stay orthogonal -- a 90deg
        // rotation about a component's centre moves pins by exact grid steps
        // in our schematic, but the user can pre-rotate at any angle and we
        // want the same Cadence-style behaviour as a drag.
        std::vector<Pt> old_pins;
        int np = int(pin_offsets(c->kind).size());
        for (int i = 0; i < np; ++i)
            old_pins.push_back(pin_world(*c, pl->second, i));

        pl->second.rot = ((pl->second.rot + delta) % 360 + 360) % 360;
        std::vector<Pt> new_pins;
        for (int i = 0; i < np; ++i)
            new_pins.push_back(pin_world(*c, pl->second, i));

        // For each touched wire, snap every old-pin vertex to its new pin
        // and orthogonalise the result. This is the same logic as
        // move_component, but per-component so a multi-component selection
        // re-routes cleanly.
        for (size_t wi = 0; wi < doc_->wires.size(); ++wi) {
            auto& w = doc_->wires[wi];
            bool touched = false;
            for (auto& v : w.pts) {
                for (size_t i = 0; i < old_pins.size(); ++i) {
                    if (dist(v, old_pins[i]) <= 1.0) {
                        v = new_pins[i];
                        touched = true;
                        break;
                    }
                }
            }
            if (!touched) continue;
            std::vector<std::pair<Pt, Pt>> other_segs;
            for (size_t oi = 0; oi < doc_->wires.size(); ++oi) {
                if (oi == wi) continue;
                const auto& ow = doc_->wires[oi];
                for (size_t k = 1; k < ow.pts.size(); ++k)
                    other_segs.push_back({ow.pts[k - 1], ow.pts[k]});
            }
            ortho_fix_wire(w.pts, other_segs);
        }
        any = true;
    }
    if (any) notify_doc();
}

void SchematicCanvas::flip_ghost(bool horizontal) {
    if (placing_) {
        if (horizontal) place_flip_h_ = !place_flip_h_;
        else place_flip_v_ = !place_flip_v_;
        Refresh();
        return;
    }
    if (sel_set_.empty()) return;
    if (on_push_undo) on_push_undo();
    bool any = false;
    for (const auto& r : sel_set_) {
        if (r.empty() || r[0] == '#') continue;
        auto pl = doc_->placements.find(r);
        if (pl == doc_->placements.end()) continue;
        if (horizontal) pl->second.flip_h = !pl->second.flip_h;
        else pl->second.flip_v = !pl->second.flip_v;
        any = true;
    }
    if (any) notify_doc();
}

void SchematicCanvas::toggle_wire_orient() {
    if (!wiring_) return;
    wire_h_first_ = !wire_h_first_;
    Refresh();
}

void SchematicCanvas::set_wire_net_name(int wire_index, const std::string& name) {
    if (wire_index < 0 || wire_index >= int(doc_->wires.size())) return;
    // Use the wire's midpoint as a sensible default click point for the label.
    const auto& w = doc_->wires[wire_index];
    Pt at = {0, 0};
    if (!w.pts.empty()) {
        at = w.pts[w.pts.size() / 2];
    }
    int li = doc_->ensure_label_on_wire(wire_index, at);
    if (li < 0) return;
    if (name.empty()) {
        // clearing the name removes the label
        doc_->labels.erase(doc_->labels.begin() + li);
        if (sel_ == "#label" + std::to_string(li)) set_selection("");
    } else {
        doc_->labels[li].name = name;
        if (doc_->labels[li].font_size <= 0) doc_->labels[li].font_size = 9;
    }
    notify_doc();
}

void SchematicCanvas::set_label_font_size(int label_index, int size) {
    if (label_index < 0 || label_index >= int(doc_->labels.size())) return;
    if (size < 6) size = 6;
    if (size > 96) size = 96;
    if (doc_->labels[label_index].font_size == size) return;
    doc_->labels[label_index].font_size = size;
    notify_doc();
}

void SchematicCanvas::rotate_selection() { rotate_ghost(90); }
void SchematicCanvas::flip_selection_h() { flip_ghost(true); }
void SchematicCanvas::flip_selection_v() { flip_ghost(false); }

void SchematicCanvas::delete_selection() {
    if (sel_set_.empty() && sel_.empty()) return;

    // Collect wires/labels/components in selection order, then delete from the
    // end backwards so earlier indices remain valid as we erase.
    std::vector<int> wires, labels;
    std::vector<std::string> refs;
    for (const auto& r : sel_set_) {
        if (r.empty()) continue;
        if (r[0] == '#') {
            if (r.compare(0, 5, "#wire") == 0) {
                int colon = int(r.find(':'));
                int wi = std::atoi(colon < 0 ? r.c_str() + 5
                                              : r.substr(5, colon - 5).c_str());
                wires.push_back(wi);
            } else if (r.compare(0, 6, "#label") == 0) {
                labels.push_back(std::atoi(r.c_str() + 6));
            }
        } else {
            refs.push_back(r);
        }
    }
    // Fall back to the primary selection if sel_set_ only has one item and we
    // already covered it above.
    if (wires.empty() && labels.empty() && refs.empty() && !sel_.empty()) {
        if (sel_.compare(0, 5, "#wire") == 0) {
            int colon = int(sel_.find(':'));
            int wi = std::atoi(colon < 0 ? sel_.c_str() + 5
                                          : sel_.substr(5, colon - 5).c_str());
            wires.push_back(wi);
        } else if (sel_.compare(0, 6, "#label") == 0) {
            labels.push_back(std::atoi(sel_.c_str() + 6));
        } else {
            refs.push_back(sel_);
        }
    }
    if (wires.empty() && labels.empty() && refs.empty()) return;
    if (on_push_undo) on_push_undo();

    // For wires, erasing shifts indices; sort descending and skip out-of-range.
    std::sort(wires.rbegin(), wires.rend());
    for (int wi : wires)
        if (wi >= 0 && wi < int(doc_->wires.size())) {
            doc_->wires.erase(doc_->wires.begin() + wi);
            // After erasing a wire, indices > wi shift down by 1; for safety
            // and to keep the operation predictable, remove remaining wires
            // whose indices were higher than the deleted one.
            // (Sorted descending means no earlier element has a higher index.)
        }
    std::sort(labels.rbegin(), labels.rend());
    for (int li : labels)
        if (li >= 0 && li < int(doc_->labels.size()))
            doc_->labels.erase(doc_->labels.begin() + li);
    for (const auto& r : refs) doc_->remove(r);

    set_selection("");
    notify_doc();
}

bool SchematicCanvas::handle_key(wxKeyEvent& e) {
    switch (e.GetKeyCode()) {
    case WXK_DELETE:
    case WXK_BACK:
        delete_selection();
        return true;
    case WXK_ESCAPE:
        cancel_current();
        return true;
    case WXK_RETURN:
    case WXK_NUMPAD_ENTER:
        // Enter commits the current wire segment and leaves wiring mode; the
        // segments already placed stay on the canvas.
        if (wiring_) {
            wire_commit_segment();
            wire_finish();
            Refresh();
            return true;
        }
        return false;
    }
    return false;
}

} // namespace symcirc
