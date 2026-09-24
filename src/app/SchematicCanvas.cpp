#include "SchematicCanvas.h"
#include "Symbols.h"

#include <cmath>
#include <wx/dcbuffer.h>

namespace symcirc {

using syms::Component;
using syms::Kind;

wxBEGIN_EVENT_TABLE(SchematicCanvas, wxScrolledWindow)
    EVT_PAINT(SchematicCanvas::on_paint)
    EVT_LEFT_DOWN(SchematicCanvas::on_left_down)
    EVT_LEFT_UP(SchematicCanvas::on_left_up)
    EVT_LEFT_DCLICK(SchematicCanvas::on_left_dclick)
    EVT_MOTION(SchematicCanvas::on_motion)
    EVT_RIGHT_DOWN(SchematicCanvas::on_right_down)
    EVT_RIGHT_UP(SchematicCanvas::on_right_up)
    EVT_MOUSEWHEEL(SchematicCanvas::on_mousewheel)
    EVT_LEAVE_WINDOW(SchematicCanvas::on_leave)
wxEND_EVENT_TABLE()

namespace {
constexpr double kGrid = 10.0;
constexpr double kSnapR = 8.0; // pin capture radius (doc units)

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

// Manhattan route between two points on the grid. `h_first` selects whether
// the horizontal leg comes first (corner at {b.x, a.y}) or the vertical leg
// (corner at {a.x, b.y}). Returns the intermediate points (start/end excluded).
std::vector<Pt> ortho_route(Pt a, Pt b, bool h_first) {
    std::vector<Pt> mid;
    if (a == b) return mid;
    if (std::fabs(b.first - a.first) < 1e-9 ||
        std::fabs(b.second - a.second) < 1e-9)
        return mid; // already axis aligned
    Pt corner = h_first ? Pt{b.first, a.second} : Pt{a.first, b.second};
    mid.push_back(corner);
    return mid;
}
} // namespace

SchematicCanvas::SchematicCanvas(wxWindow* parent, Document* doc)
    : wxScrolledWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                       wxHSCROLL | wxVSCROLL | wxWANTS_CHARS),
      doc_(doc) {
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetBackgroundColour(*wxWHITE);
    SetVirtualSize(int(2400 * zoom_), int(1600 * zoom_));
    SetScrollRate(20, 20);
}

void SchematicCanvas::set_tool(Tool t, Kind k) {
    tool_ = t;
    place_kind_ = k;
    place_rot_ = 0;
    place_flip_h_ = place_flip_v_ = false;
    placing_ = (t == Tool::Place);
    wiring_ = false;
    wire_draft_.clear();
    label_queue_.clear();
    Refresh();
}

void SchematicCanvas::begin_place(Kind k, int rot) {
    tool_ = Tool::Place;
    placing_ = true;
    place_kind_ = k;
    place_rot_ = rot;
    place_flip_h_ = place_flip_v_ = false;
    wiring_ = false;
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

// Leave placement mode / abort a wire. Called by Escape.
void SchematicCanvas::cancel_current() {
    placing_ = false;
    wiring_ = false;
    wire_draft_.clear();
    label_queue_.clear();
    tool_ = Tool::Select;
    Refresh();
}

void SchematicCanvas::rotate_ghost(int delta) {
    if (placing_) {
        place_rot_ = ((place_rot_ + delta) % 360 + 360) % 360;
        Refresh();
        return;
    }
    if (!sel_.empty() && sel_[0] != '#') {
        auto pl = doc_->placements.find(sel_);
        if (pl != doc_->placements.end()) {
            if (on_push_undo) on_push_undo();
            pl->second.rot = ((pl->second.rot + delta) % 360 + 360) % 360;
            notify_doc();
        }
    }
}

void SchematicCanvas::flip_ghost(bool horizontal) {
    if (placing_) {
        if (horizontal) place_flip_h_ = !place_flip_h_;
        else place_flip_v_ = !place_flip_v_;
        Refresh();
        return;
    }
    if (!sel_.empty() && sel_[0] != '#') {
        auto pl = doc_->placements.find(sel_);
        if (pl != doc_->placements.end()) {
            if (on_push_undo) on_push_undo();
            if (horizontal) pl->second.flip_h = !pl->second.flip_h;
            else pl->second.flip_v = !pl->second.flip_v;
            notify_doc();
        }
    }
}

// Space while wiring swaps the rubber-band routing between horizontal-first
// and vertical-first.
void SchematicCanvas::toggle_wire_orient() {
    if (!wiring_) return;
    wire_h_first_ = !wire_h_first_;
    Refresh();
}

// Move a component and any wire endpoints that were attached to its pins.
void SchematicCanvas::move_component(const std::string& ref, double nx,
                                     double ny) {
    const Component* c = doc_->circuit.find(ref);
    auto pl = doc_->placements.find(ref);
    if (!c || pl == doc_->placements.end()) return;
    double dx = nx - pl->second.x;
    double dy = ny - pl->second.y;
    if (dx == 0.0 && dy == 0.0) return;

    // world positions of the pins before the move
    std::vector<Pt> old_pins;
    int np = int(pin_offsets(c->kind).size());
    for (int i = 0; i < np; ++i) old_pins.push_back(pin_world(*c, pl->second, i));

    pl->second.x = nx;
    pl->second.y = ny;

    // any wire vertex sitting exactly on an old pin follows the pin
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

void SchematicCanvas::set_selection(const std::string& s) {
    if (sel_ == s) return;
    sel_ = s;
    notify_sel();
    Refresh();
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
Pt SchematicCanvas::to_doc(const wxPoint& p) const {
    int x = 0, y = 0;
    CalcUnscrolledPosition(p.x, p.y, &x, &y);
    return {double(x) / zoom_, double(y) / zoom_};
}

Pt SchematicCanvas::to_view(Pt p) const {
    int vx, vy;
    GetViewStart(&vx, &vy);
    int sx, sy;
    GetScrollPixelsPerUnit(&sx, &sy);
    return {p.first * zoom_ - vx * sx, p.second * zoom_ - vy * sy};
}

Pt SchematicCanvas::snap(Pt p) const {
    return {std::round(p.first / kGrid) * kGrid,
            std::round(p.second / kGrid) * kGrid};
}

std::string SchematicCanvas::hit_component(Pt p) const {
    Pt v = to_view(p);
    // topmost last drawn wins -> iterate in reverse
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
        if (dist(to_view(pin_world(*c, pl->second, i)), v) <= kSnapR * zoom_)
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
            if (dist(to_view(pin_world(c, pl->second, i)), v) <= kSnapR * zoom_) {
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
            if (seg_dist(v, to_view(w.pts[k - 1]), to_view(w.pts[k])) <=
                5.0 * zoom_) {
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
            if (seg_dist(v, to_view(w.pts[k - 1]), to_view(w.pts[k])) <=
                6.0 * zoom_) {
                idx = i;
                seg = int(k) - 1;
                return true;
            }
    }
    return false;
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

bool SchematicCanvas::hit_label(Pt p, int& idx) const {
    Pt v = to_view(p);
    for (int i = int(doc_->labels.size()) - 1; i >= 0; --i)
        if (dist(to_view(doc_->labels[i].pt), v) <= 12.0 * zoom_) {
            idx = i;
            return true;
        }
    return false;
}

// ---------------------------------------------------------------------------
void SchematicCanvas::on_paint(wxPaintEvent&) {
    wxAutoBufferedPaintDC dc(this);
    // wxBG_STYLE_PAINT suppresses automatic erasing: clear explicitly so the
    // sheet is white paper with black ink.
    dc.SetBackground(wxBrush(*wxWHITE));
    dc.Clear();
    DoPrepareDC(dc);
    dc.SetUserScale(zoom_, zoom_);

    // grid: faint lines on white (sparser as we zoom out). Lines are far
    // cheaper than a dot per grid node and repaint smoothly while dragging.
    dc.SetPen(wxPen(wxColour(232, 232, 236)));
    wxPoint tl;
    CalcUnscrolledPosition(0, 0, &tl.x, &tl.y);
    wxSize cs = GetClientSize();
    wxPoint br;
    CalcUnscrolledPosition(cs.x, cs.y, &br.x, &br.y);
    double x0 = tl.x / zoom_, y0 = tl.y / zoom_;
    double x1 = br.x / zoom_, y1 = br.y / zoom_;
    double step = kGrid;
    while (step * zoom_ < 24.0) step *= 2.0; // keep >= ~24 screen px between lines
    for (double x = std::floor(x0 / step) * step; x < x1 + step; x += step)
        dc.DrawLine(wxPoint(int(x), int(y0)), wxPoint(int(x), int(y1)));
    for (double y = std::floor(y0 / step) * step; y < y1 + step; y += step)
        dc.DrawLine(wxPoint(int(x0), int(y)), wxPoint(int(x1), int(y)));

    // wires (black); the selected segment is highlighted on its own
    Selection si = selection_info();
    for (size_t i = 0; i < doc_->wires.size(); ++i) {
        const auto& w = doc_->wires[i];
        bool whole = si.type == Selection::Wire && si.wire == int(i);
        for (size_t k = 1; k < w.pts.size(); ++k) {
            bool seg_sel = si.type == Selection::WireSegment &&
                           si.wire == int(i) && si.seg == int(k) - 1;
            dc.SetPen(wxPen(seg_sel ? wxColour(0, 92, 200)
                                    : whole ? wxColour(0, 92, 200)
                                            : wxColour(0, 0, 0),
                            seg_sel ? 3 : whole ? 3 : 2));
            dc.DrawLine(wxPoint(int(w.pts[k - 1].first), int(w.pts[k - 1].second)),
                        wxPoint(int(w.pts[k].first), int(w.pts[k].second)));
        }
    }

    // wire in progress: orthogonal, with a live rubber-band segment
    if (wiring_ && !wire_draft_.empty()) {
        dc.SetPen(wxPen(wxColour(0, 120, 200), 2, wxPENSTYLE_SHORT_DASH));
        for (size_t k = 1; k < wire_draft_.size(); ++k)
            dc.DrawLine(
                wxPoint(int(wire_draft_[k - 1].first), int(wire_draft_[k - 1].second)),
                wxPoint(int(wire_draft_[k].first), int(wire_draft_[k].second)));
        if (has_mouse_) {
            Pt last = wire_draft_.back();
            Pt m = snap(to_doc(mouse_));
            auto mids = ortho_route(last, m, wire_h_first_);
            Pt prev = last;
            for (const auto& q : mids) {
                dc.DrawLine(wxPoint(int(prev.first), int(prev.second)),
                            wxPoint(int(q.first), int(q.second)));
                prev = q;
            }
            dc.DrawLine(wxPoint(int(prev.first), int(prev.second)),
                        wxPoint(int(m.first), int(m.second)));
        }
    }

    // net labels
    for (size_t i = 0; i < doc_->labels.size(); ++i) {
        const auto& l = doc_->labels[i];
        bool is_sel = sel_ == "#label" + std::to_string(i);
        wxString txt = wxString::FromUTF8(l.name);
        wxSize ts = dc.GetTextExtent(txt);
        wxPoint p(int(l.pt.first) - ts.x / 2, int(l.pt.second) - ts.y - 6);
        dc.SetTextForeground(is_sel ? *wxWHITE : wxColour(0, 0, 0));
        dc.SetTextBackground(is_sel ? wxColour(0, 92, 200) : *wxWHITE);
        dc.DrawText(txt, p.x, p.y);
        dc.SetTextBackground(*wxWHITE);
    }

    // components
    for (const auto& c : doc_->circuit.comps) {
        auto pl = doc_->placements.find(c.ref);
        if (pl == doc_->placements.end()) continue;
        draw_symbol(dc, c, pl->second, sel_ == c.ref);
    }

    // pending net-label placement: show the next name following the cursor
    if (tool_ == Tool::Label && !label_queue_.empty() && has_mouse_) {
        wxString txt = wxString::FromUTF8(label_queue_.front());
        wxSize ts = dc.GetTextExtent(txt);
        wxPoint p(mouse_.x - ts.x / 2, mouse_.y - ts.y - 8);
        dc.SetTextForeground(wxColour(0, 92, 200));
        dc.SetTextBackground(*wxWHITE);
        dc.DrawText(txt, p.x, p.y);
        dc.SetTextBackground(*wxWHITE);
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
void SchematicCanvas::on_left_down(wxMouseEvent& e) {
    SetFocus();
    Pt p = to_doc(e.GetPosition());
    mouse_ = e.GetPosition();
    has_mouse_ = true;

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
    }    case Tool::Delete: {
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
            // start a new wire at the clicked pin (or grid point)
            wiring_ = true;
            wire_h_first_ = true;
            wire_draft_.clear();
            wire_draft_.push_back(target);
            Refresh();
            break;
        }
        if (target == wire_draft_.back()) break; // same point: ignore

        // commit the orthogonal route from the last vertex to this point
        auto mids = ortho_route(wire_draft_.back(), target, wire_h_first_);
        for (const auto& q : mids) wire_draft_.push_back(q);
        wire_draft_.push_back(target);

        if (pin >= 0) {
            // finishing on a pin: end the wire here
            if (on_push_undo) on_push_undo();
            Wire w;
            w.pts = wire_draft_;
            doc_->wires.push_back(w);
            wiring_ = false;
            wire_draft_.clear();
            notify_doc();
        }
        Refresh();
        break;
    }
    case Tool::Label: {
        if (label_queue_.empty()) break;
        if (on_push_undo) on_push_undo();
        NetLabel l;
        l.pt = snap(p);
        l.name = label_queue_.front();
        doc_->labels.push_back(l);
        label_queue_.erase(label_queue_.begin());
        if (label_queue_.empty()) tool_ = Tool::Select;
        notify_doc();
        break;
    }
    case Tool::Select: {
        std::string ref = hit_component(p);
        if (!ref.empty()) {
            set_selection(ref);
            auto pl = doc_->placements.find(ref);
            if (pl != doc_->placements.end() && ref != "") {
                dragging_ = true;
                drag_dx_ = pl->second.x - p.first;
                drag_dy_ = pl->second.y - p.second;
            }
            return;
        }
        int wi, li, sg;
        if (hit_wire_segment(p, wi, sg)) {
            set_selection("#wire" + std::to_string(wi) + ":" +
                          std::to_string(sg));
            return;
        }
        if (hit_label(p, li)) {
            set_selection("#label" + std::to_string(li));
            return;
        }
        set_selection("");
        break;
    }
    }
}

void SchematicCanvas::on_left_dclick(wxMouseEvent& e) {
    // A double-click finishes an in-progress wire (the first click of the
    // pair already added the corner).
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

void SchematicCanvas::on_left_up(wxMouseEvent&) {
    if (dragging_) {
        dragging_ = false;
        notify_doc();
    }
}

void SchematicCanvas::on_motion(wxMouseEvent& e) {
    mouse_ = e.GetPosition();
    has_mouse_ = true;
    Pt p = to_doc(mouse_);

    // right-button drag pans the view (scroll positions are in pixels here,
    // so no zoom/scroll-unit conversion is needed)
    if (panning_ && e.RightIsDown()) {
        int pan_x, pan_y;
        int x = 0, y = 0;
        CalcUnscrolledPosition(0, 0, &x, &y);
        (void)x;
        (void)y;
        // pan_x/pan_y from GetViewStart are in scroll units
        int sx, sy;
        GetScrollPixelsPerUnit(&sx, &sy);
        if (!sx) sx = 1;
        if (!sy) sy = 1;
        int vx, vy;
        GetViewStart(&vx, &vy);
        int dx = mouse_.x - pan_last_.x; // pixels
        int dy = mouse_.y - pan_last_.y;
        pan_total_ += std::abs(dx) + std::abs(dy);
        Scroll((vx * sx - dx) / sx, (vy * sy - dy) / sy);
        pan_last_ = mouse_;
        Refresh();
        return;
    }

    if (dragging_ && !sel_.empty() && sel_[0] != '#') {
        auto pl = doc_->placements.find(sel_);
        if (pl != doc_->placements.end()) {
            double nx = std::round((p.first + drag_dx_) / kGrid) * kGrid;
            double ny = std::round((p.second + drag_dy_) / kGrid) * kGrid;
            move_component(sel_, nx, ny);
            Refresh();
        }
        return;
    }

    std::string nh;
    if (tool_ == Tool::Wire && wiring_) {
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
                msg = "Click a pin (or point) to add a corner; Space swaps the "
                      "route; click a pin to finish";
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
    // Repaint only when something visual actually changed. In Select mode the
    // ghost is gone, so a plain mouse move needs no repaint (this is what made
    // dragging/scrubbing feel choppy).
    if (wiring_ || tool_ == Tool::Place || tool_ == Tool::Label) Refresh(false);
}

void SchematicCanvas::on_right_down(wxMouseEvent& e) {
    (void)e;
    // press-and-hold starts panning; a plain click (no drag) is handled in
    // on_right_up and simply returns to Select mode
    panning_ = true;
    pan_total_ = 0;
    pan_last_ = e.GetPosition();
    mouse_ = e.GetPosition();
    has_mouse_ = true;
    CaptureMouse();
}

void SchematicCanvas::on_right_up(wxMouseEvent& e) {
    (void)e;
    if (panning_) {
        panning_ = false;
        if (HasCapture()) ReleaseMouse();
        // a click without a real drag cancels the current tool
        if (pan_total_ <= 3 && !wiring_ && !placing_) {
            set_tool(Tool::Select);
            if (on_status) on_status("");
        }
    }
}

void SchematicCanvas::on_mousewheel(wxMouseEvent& e) {
    const double kMinZoom = 0.35, kMaxZoom = 6.0;
    mouse_ = e.GetPosition();
    has_mouse_ = true;
    double old = zoom_;
    double factor = e.GetWheelRotation() > 0 ? 1.1 : 1.0 / 1.1;
    double nz = std::max(kMinZoom, std::min(kMaxZoom, old * factor));
    if (std::fabs(nz - old) < 1e-9) return;

    // keep the point under the cursor stationary
    int vx, vy;
    GetViewStart(&vx, &vy);
    int sx, sy;
    GetScrollPixelsPerUnit(&sx, &sy);
    if (!sx) sx = 1;
    if (!sy) sy = 1;
    double docx = (vx * sx + mouse_.x) / old;
    double docy = (vy * sy + mouse_.y) / old;
    zoom_ = nz;
    SetVirtualSize(int(2400 * zoom_), int(1600 * zoom_));
    double nx = docx * nz - mouse_.x;
    double ny = docy * nz - mouse_.y;
    SetScrollPos(wxHORIZONTAL, int(std::lround(nx / sx)), true);
    SetScrollPos(wxVERTICAL, int(std::lround(ny / sy)), true);
    Refresh();
}

void SchematicCanvas::on_leave(wxMouseEvent&) {
    if (panning_ && HasCapture()) ReleaseMouse();
    panning_ = false;
    if (tool_ == Tool::Place) Refresh();
}

// ---------------------------------------------------------------------------
void SchematicCanvas::rotate_selection() { rotate_ghost(90); }
void SchematicCanvas::flip_selection_h() { flip_ghost(true); }
void SchematicCanvas::flip_selection_v() { flip_ghost(false); }

void SchematicCanvas::delete_selection() {
    if (sel_.empty()) return;
    Selection si = selection_info();
    if (si.type == Selection::Wire) {
        if (si.wire >= 0 && si.wire < int(doc_->wires.size())) {
            if (on_push_undo) on_push_undo();
            doc_->wires.erase(doc_->wires.begin() + si.wire);
        }
        set_selection("");
        notify_doc();
        return;
    }
    if (si.type == Selection::WireSegment) {
        if (si.wire >= 0 && si.wire < int(doc_->wires.size()) && si.seg >= 0) {
            auto& w = doc_->wires[si.wire];
            if (si.seg + 1 < int(w.pts.size())) {
                if (on_push_undo) on_push_undo();
                // dropping one segment splits the polyline in two
                std::vector<Pt> left(w.pts.begin(), w.pts.begin() + si.seg + 1);
                std::vector<Pt> right(w.pts.begin() + si.seg + 1, w.pts.end());
                doc_->wires.erase(doc_->wires.begin() + si.wire);
                size_t at = si.wire;
                if (left.size() >= 2) {
                    Wire lw;
                    lw.pts = left;
                    doc_->wires.insert(doc_->wires.begin() + at, lw);
                    ++at;
                }
                if (right.size() >= 2) {
                    Wire rw;
                    rw.pts = right;
                    doc_->wires.insert(doc_->wires.begin() + at, rw);
                }
                set_selection("");
                notify_doc();
                return;
            }
        }
        set_selection("");
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
    // component
    if (on_push_undo) on_push_undo();
    std::string ref = sel_;
    doc_->remove(ref);
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
        // Always leave whatever interaction is in progress, then clear the
        // selection. (Previously this could be missed when placing_ had been
        // cleared but the tool was still Place.)
        cancel_current();
        return true;
    }
    return false;
}

} // namespace symcirc
