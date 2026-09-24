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
constexpr double kSnapR = 8.0; // pin capture radius (screen px)
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
    wiring_ = false;
    wire_draft_.clear();
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
    wiring_ = false;
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
    wiring_ = false;
    wire_draft_.clear();
    SetFocus();
    Refresh();
}

void SchematicCanvas::cancel_current() {
    placing_ = false;
    wiring_ = false;
    wire_draft_.clear();
    label_queue_.clear();
    box_selecting_ = false;
    tool_ = Tool::Select;
    Refresh();
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

void SchematicCanvas::set_zoom(double z, wxPoint anchor) {
    z = std::max(kMinZoom, std::min(kMaxZoom, z));
    if (std::fabs(z - zoom_) < 1e-12) return;
    // keep the document point under `anchor` fixed on screen
    Pt d = to_doc(anchor);
    zoom_ = z;
    view_x_ = d.first - anchor.x / zoom_;
    view_y_ = d.second - anchor.y / zoom_;
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

void SchematicCanvas::zoom_to_fit() {
    double x0, y0, x1, y1;
    if (!content_bounds(x0, y0, x1, y1)) {
        // nothing to frame: reset to a comfortable default view
        zoom_ = 1.0;
        view_x_ = -50.0;
        view_y_ = -50.0;
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
    Pt v = to_view(p);
    for (auto it = doc_->circuit.comps.rbegin();
         it != doc_->circuit.comps.rend(); ++it) {
        auto pl = doc_->placements.find(it->ref);
        if (pl == doc_->placements.end()) continue;
        double x0, y0, x1, y1;
        symbol_bbox(*it, pl->second, x0, y0, x1, y1);
        Pt a = to_view({x0, y0}), b = to_view({x1, y1});
        if (v.first >= a.first && v.first <= b.first && v.second >= a.second &&
            v.second <= b.second)
            return it->ref;
    }
    return "";
}

int SchematicCanvas::hit_pin(const std::string& ref, Pt p) const {
    const Component* c = doc_->circuit.find(ref);
    if (!c) return -1;
    auto pl = doc_->placements.find(ref);
    if (pl == doc_->placements.end()) return -1;
    Pt v = to_view(p);
    int n = int(pin_offsets(c->kind).size());
    for (int i = 0; i < n; ++i)
        if (dist(to_view(pin_world(*c, pl->second, i)), v) <= kSnapR)
            return i;
    return -1;
}

int SchematicCanvas::hit_any_pin(Pt p, std::string& ref) const {
    Pt v = to_view(p);
    for (const auto& c : doc_->circuit.comps) {
        auto pl = doc_->placements.find(c.ref);
        if (pl == doc_->placements.end()) continue;
        int n = int(pin_offsets(c.kind).size());
        for (int i = 0; i < n; ++i)
            if (dist(to_view(pin_world(c, pl->second, i)), v) <= kSnapR) {
                ref = c.ref;
                return i;
            }
    }
    ref.clear();
    return -1;
}

bool SchematicCanvas::hit_wire(Pt p, int& idx) const {
    Pt v = to_view(p);
    for (int i = int(doc_->wires.size()) - 1; i >= 0; --i) {
        const auto& w = doc_->wires[i];
        for (size_t k = 1; k < w.pts.size(); ++k)
            if (seg_dist(v, to_view(w.pts[k - 1]), to_view(w.pts[k])) <= 5.0) {
                idx = i;
                return true;
            }
    }
    return false;
}

bool SchematicCanvas::hit_wire_segment(Pt p, int& idx, int& seg) const {
    Pt v = to_view(p);
    for (int i = int(doc_->wires.size()) - 1; i >= 0; --i) {
        const auto& w = doc_->wires[i];
        for (size_t k = 1; k < w.pts.size(); ++k)
            if (seg_dist(v, to_view(w.pts[k - 1]), to_view(w.pts[k])) <= 6.0) {
                idx = i;
                seg = int(k) - 1;
                return true;
            }
    }
    return false;
}

bool SchematicCanvas::hit_label(Pt p, int& idx) const {
    Pt v = to_view(p);
    for (int i = int(doc_->labels.size()) - 1; i >= 0; --i) {
        const auto& l = doc_->labels[i];
        Pt a = to_view(l.pt);
        // clickable area: the anchor dot plus the text drawn above it
        double fs = std::max(8, l.font_size) * zoom_;
        double halfw = std::max(14.0, fs * 0.32 * std::max<size_t>(1, l.name.size()));
        double top = a.second - fs * 1.4;
        if (v.first >= a.first - halfw && v.first <= a.first + halfw &&
            v.second >= top && v.second <= a.second + 8) {
            idx = i;
            return true;
        }
        if (dist(a, v) <= 10.0) {
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

    // wire in progress: orthogonal route from the anchor to the cursor
    if (wiring_ && !wire_draft_.empty()) {
        Pt last = wire_draft_.back();
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
        Pt p{l.pt.first - ts.x / 2.0, l.pt.second - ts.y - 4.0};
        if (is_sel) {
            dc.SetBrush(wxBrush(wxColour(0, 92, 200)));
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.DrawRectangle(wxRect(int(p.first) - 3, int(p.second) - 1,
                                    ts.x + 6, ts.y + 2));
            dc.SetBrush(*wxTRANSPARENT_BRUSH);
            dc.SetTextForeground(*wxWHITE);
        } else {
            dc.SetTextForeground(wxColour(0, 0, 0));
        }
        dc.DrawText(txt, wxPoint(int(p.first), int(p.second)));
        dc.SetTextForeground(*wxBLACK);
    }

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
        wxFont f(14, wxFONTFAMILY_SWISS, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD);
        dc.SetFont(f);
        wxString txt = wxString::FromUTF8(label_queue_.front());
        wxSize ts = dc.GetTextExtent(txt);
        dc.SetTextForeground(wxColour(0, 92, 200));
        dc.DrawText(txt, wxPoint(int(anchor.first - ts.x / 2.0),
                                 int(anchor.second - ts.y - 6.0)));
        dc.SetTextForeground(*wxBLACK);
    }

    // red dot at the cursor while wiring (#8): follows the pointer exactly
    if (tool_ == Tool::Wire && has_mouse_) {
        Pt cursor = to_doc(mouse_);
        doc_circle(cursor, 4, wxColour(210, 30, 30), true);
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

        if (!wiring_) {
            // first click: anchor the wire at a pin (or the clicked point)
            wiring_ = true;
            wire_h_first_ = true;
            wire_draft_.clear();
            wire_draft_.push_back(target);
            Refresh(false);
            break;
        }
        if (target == wire_draft_.back()) break;

        // add the orthogonal route to this point as a new corner
        auto mids = ortho_route(wire_draft_.back(), target, wire_h_first_);
        for (const auto& q : mids) wire_draft_.push_back(q);
        wire_draft_.push_back(target);

        // Terminate on a component pin, or on an existing wire vertex (a tap);
        // otherwise keep the wire open so the user can add more corners.
        bool on_wire_node = false;
        if (pin < 0) {
            for (const auto& w : doc_->wires) {
                for (const auto& v : w.pts)
                    if (dist(v, target) < 1.0) { on_wire_node = true; break; }
                if (on_wire_node) break;
            }
        }
        if (pin >= 0 || on_wire_node) {
            // terminating: commit the wire here
            if (on_push_undo) on_push_undo();
            Wire w;
            w.pts = wire_draft_;
            doc_->wires.push_back(w);
            wiring_ = false;
            wire_draft_.clear();
            notify_doc();
        } else {
            // not a terminal: keep wiring from this corner
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
        if (hit_wire(p, wi) && wi >= 0) {
            Pt best = target;
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
            target = best;
        }
        l.pt = target;
        l.name = label_queue_.front();
        l.font_size = 14;
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
                std::string err;
                on_wire_selected(wi, doc_->net_name_of_wire(wi, err));
            }
            Refresh();
            return;
        }
        if (hit_label(p, li)) {
            sel_set_.clear();
            sel_ = "#label" + std::to_string(li);
            sel_set_.insert(sel_);
            notify_sel();
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
        // select every component whose bbox intersects the rubber band
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
            sel_ = sel_set_.empty() ? "" : *sel_set_.begin();
            notify_sel();
        }
        Refresh();
        return;
    }
    if (dragging_) {
        dragging_ = false;
        notify_doc();
    }
}

void SchematicCanvas::on_left_dclick(wxMouseEvent& e) {
    // Double-click finishes an in-progress wire at the current corner (the
    // single click has already added the point). This is the way to end a wire
    // that does not terminate on a pin.
    if (tool_ == Tool::Wire && wiring_ && wire_draft_.size() >= 2) {
        if (on_push_undo) on_push_undo();
        Wire w;
        w.pts = wire_draft_;
        doc_->wires.push_back(w);
        wiring_ = false;
        wire_draft_.clear();
        notify_doc();
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
                msg = wiring_ ? "Click to add a corner; click a pin or wire to "
                                "finish (Space swaps the route)"
                              : "Click to start a wire; the red dot is the "
                                "snap point";
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
    // click), or the pending net label.
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

    for (auto& w : doc_->wires) {
        for (auto& v : w.pts) {
            for (size_t i = 0; i < old_pins.size(); ++i) {
                if (dist(v, old_pins[i]) <= 1.0) {
                    v.first += dx;
                    v.second += dy;
                    break;
                }
            }
        }
    }
    for (auto& l : doc_->labels) {
        for (size_t i = 0; i < old_pins.size(); ++i)
            if (dist(l.pt, old_pins[i]) <= 1.0) {
                l.pt.first += dx;
                l.pt.second += dy;
                break;
            }
    }
}

void SchematicCanvas::notify_doc() {
    doc_->dirty = true;
    if (on_document_changed) on_document_changed();
    Refresh();
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
        pl->second.rot = ((pl->second.rot + delta) % 360 + 360) % 360;
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
    std::string err;
    int li = doc_->ensure_label_on_wire(wire_index, err);
    if (li < 0) return;
    if (name.empty()) {
        // clearing the name removes the label
        doc_->labels.erase(doc_->labels.begin() + li);
        if (sel_ == "#label" + std::to_string(li)) set_selection("");
    } else {
        doc_->labels[li].name = name;
        if (doc_->labels[li].font_size <= 0) doc_->labels[li].font_size = 14;
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
    Selection si = selection_info();
    if (si.type == Selection::Wire || si.type == Selection::WireSegment) {
        if (si.wire >= 0 && si.wire < int(doc_->wires.size())) {
            if (on_push_undo) on_push_undo();
            doc_->wires.erase(doc_->wires.begin() + si.wire);
        }
        set_selection("");
        notify_doc();
        return;
    }
    if (si.type == Selection::Label) {
        if (si.label >= 0 && si.label < int(doc_->labels.size())) {
            if (on_push_undo) on_push_undo();
            doc_->labels.erase(doc_->labels.begin() + si.label);
        }
        set_selection("");
        notify_doc();
        return;
    }
    // components (possibly several)
    if (on_push_undo) on_push_undo();
    std::vector<std::string> refs;
    for (const auto& r : sel_set_)
        if (!r.empty() && r[0] != '#') refs.push_back(r);
    if (refs.empty() && !sel_.empty() && sel_[0] != '#') refs.push_back(sel_);
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
    }
    return false;
}

} // namespace symcirc
