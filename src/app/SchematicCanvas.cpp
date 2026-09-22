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

// Manhattan route between two points on the grid. Horizontal-first when the
// horizontal movement dominates, vertical-first otherwise. Returns the points
// in between (start and end excluded).
std::vector<Pt> ortho_route(Pt a, Pt b) {
    std::vector<Pt> mid;
    if (a == b) return mid;
    if (std::fabs(b.first - a.first) < 1e-9 ||
        std::fabs(b.second - a.second) < 1e-9)
        return mid; // already axis aligned
    if (std::fabs(b.first - a.first) >= std::fabs(b.second - a.second)) {
        Pt corner{b.first, a.second};
        mid.push_back(corner);
    } else {
        Pt corner{a.first, b.second};
        mid.push_back(corner);
    }
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
    wiring_ = false;
    wire_draft_.clear();
    Refresh();
}

void SchematicCanvas::begin_place(Kind k, int rot) {
    tool_ = Tool::Place;
    place_kind_ = k;
    place_rot_ = rot;
    place_flip_h_ = place_flip_v_ = false;
    Refresh();
}

void SchematicCanvas::rotate_ghost(int delta) {
    if (tool_ == Tool::Place) {
        place_rot_ = ((place_rot_ + delta) % 360 + 360) % 360;
        Refresh();
        return;
    }
    if (!sel_.empty() && sel_[0] != '#') {
        auto pl = doc_->placements.find(sel_);
        if (pl != doc_->placements.end()) {
            pl->second.rot = ((pl->second.rot + delta) % 360 + 360) % 360;
            notify_doc();
        }
    }
}

void SchematicCanvas::flip_ghost(bool horizontal) {
    if (tool_ == Tool::Place) {
        if (horizontal) place_flip_h_ = !place_flip_h_;
        else place_flip_v_ = !place_flip_v_;
        Refresh();
        return;
    }
    if (!sel_.empty() && sel_[0] != '#') {
        auto pl = doc_->placements.find(sel_);
        if (pl != doc_->placements.end()) {
            if (horizontal) pl->second.flip_h = !pl->second.flip_h;
            else pl->second.flip_v = !pl->second.flip_v;
            notify_doc();
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

    // grid: faint dots on white (denser as we zoom in)
    dc.SetPen(wxPen(wxColour(226, 226, 226)));
    wxPoint tl;
    CalcUnscrolledPosition(0, 0, &tl.x, &tl.y);
    wxSize cs = GetClientSize();
    wxPoint br;
    CalcUnscrolledPosition(cs.x, cs.y, &br.x, &br.y);
    double x0 = tl.x / zoom_, y0 = tl.y / zoom_;
    double x1 = br.x / zoom_, y1 = br.y / zoom_;
    double step = kGrid;
    if (zoom_ <= 1.0) step = 5 * kGrid;   // fewer dots when zoomed out
    else if (zoom_ >= 3.0) step = 2.5 * kGrid / 2.5; // = kGrid
    for (double x = std::floor(x0 / step) * step; x < x1 + step; x += step)
        for (double y = std::floor(y0 / step) * step; y < y1 + step;
             y += step)
            dc.DrawPoint(wxPoint(int(x), int(y)));

    // wires (black)
    for (size_t i = 0; i < doc_->wires.size(); ++i) {
        const auto& w = doc_->wires[i];
        bool is_sel = sel_ == "#wire" + std::to_string(i);
        dc.SetPen(wxPen(is_sel ? wxColour(0, 92, 200) : wxColour(0, 0, 0),
                        is_sel ? 3 : 2));
        for (size_t k = 1; k < w.pts.size(); ++k)
            dc.DrawLine(wxPoint(int(w.pts[k - 1].first), int(w.pts[k - 1].second)),
                        wxPoint(int(w.pts[k].first), int(w.pts[k].second)));
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
            auto mids = ortho_route(last, m);
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

    // hover highlight while wiring onto a pin
    if (wiring_ && has_mouse_) {
        std::string ref;
        Pt m = to_doc(mouse_);
        int i = hit_any_pin(m, ref);
        if (i >= 0) {
            const Component* c = doc_->circuit.find(ref);
            auto pl = doc_->placements.find(ref);
            if (c && pl != doc_->placements.end()) {
                Pt w = to_view(pin_world(*c, pl->second, i));
                dc.SetPen(wxPen(wxColour(0, 120, 200), 2));
                dc.SetBrush(*wxTRANSPARENT_BRUSH);
                dc.DrawCircle(wxPoint(int(w.first), int(w.second)), 6);
            }
        }
    }

    // ghost of component being placed
    if (tool_ == Tool::Place && has_mouse_) {
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
        std::string ref = doc_->add(c, s.first, s.second);
        auto pl = doc_->placements.find(ref);
        if (pl != doc_->placements.end()) {
            pl->second.rot = place_rot_;
            pl->second.flip_h = place_flip_h_;
            pl->second.flip_v = place_flip_v_;
        }
        notify_doc();
        set_selection(ref);
        break;
    }
    case Tool::Delete: {
        int wi, li;
        std::string ref = hit_component(p);
        if (!ref.empty()) {
            doc_->remove(ref);
            set_selection("");
        } else if (hit_wire(p, wi)) {
            doc_->wires.erase(doc_->wires.begin() + wi);
        } else if (hit_label(p, li)) {
            doc_->labels.erase(doc_->labels.begin() + li);
        }
        notify_doc();
        break;
    }
    case Tool::Wire: {
        std::string ref;
        Pt target = snap(p);
        int pin = hit_any_pin(p, ref);
        if (pin >= 0) target = snap(p);

        if (!wiring_) {
            wiring_ = true;
            wire_draft_.clear();
            wire_draft_.push_back(target);
        } else if (target == wire_draft_.back()) {
            // same point: ignore
        } else {
            auto mids = ortho_route(wire_draft_.back(), target);
            for (const auto& q : mids) wire_draft_.push_back(q);
            wire_draft_.push_back(target);
            if (pin >= 0 && wire_draft_.size() >= 2) {
                Wire w;
                w.pts = wire_draft_;
                doc_->wires.push_back(w);
                wiring_ = false;
                wire_draft_.clear();
                notify_doc();
            }
        }
        Refresh();
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
        int wi, li;
        if (hit_wire(p, wi)) {
            set_selection("#wire" + std::to_string(wi));
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

    // right-button drag pans the view
    if (panning_ && e.RightIsDown()) {
        int vx, vy;
        GetViewStart(&vx, &vy); // scroll units
        int sx, sy;
        GetScrollPixelsPerUnit(&sx, &sy);
        if (!sx) sx = 1;
        if (!sy) sy = 1;
        double dx = double(mouse_.x - pan_last_.x) / (sx * zoom_);
        double dy = double(mouse_.y - pan_last_.y) / (sy * zoom_);
        SetScrollPos(wxHORIZONTAL, vx - int(std::lround(dx)), true);
        SetScrollPos(wxVERTICAL, vy - int(std::lround(dy)), true);
        pan_last_ = mouse_;
        Refresh();
        return;
    }

    if (dragging_ && !sel_.empty() && sel_[0] != '#') {
        auto pl = doc_->placements.find(sel_);
        if (pl != doc_->placements.end()) {
            pl->second.x = std::round((p.first + drag_dx_) / kGrid) * kGrid;
            pl->second.y = std::round((p.second + drag_dy_) / kGrid) * kGrid;
            Refresh();
        }
        return;
    }

    std::string nh;
    if (tool_ == Tool::Wire && wiring_) nh = "wire";    else if (tool_ == Tool::Delete) {
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
            else if (hover_ == "wire") msg = "Click a pin to finish the wire";
            else if (hover_ == "del-comp") msg = "Click to delete component";
            else if (hover_ == "del-wire") msg = "Click to delete wire";
            else if (hover_ == "del-label") msg = "Click to delete label";
            else if (hover_.rfind("pin:", 0) == 0)
                msg = "Pin of " + hover_.substr(4);
            else msg = hover_;
            on_status(msg);
        }
    }
    if (wiring_ || tool_ == Tool::Place) Refresh();
}

void SchematicCanvas::on_right_down(wxMouseEvent& e) {
    (void)e;
    // press-and-hold starts panning; a plain click (no drag) is handled in
    // on_right_up and simply returns to Select mode
    panning_ = true;
    pan_last_ = e.GetPosition();
    mouse_ = e.GetPosition();
    has_mouse_ = true;
    CaptureMouse();
}

void SchematicCanvas::on_right_up(wxMouseEvent& e) {
    if (panning_) {
        bool moved = std::abs(e.GetPosition().x - pan_last_.x) > 3 ||
                     std::abs(e.GetPosition().y - pan_last_.y) > 3;
        panning_ = false;
        if (HasCapture()) ReleaseMouse();
        // a click without a drag cancels the current tool (Select), like the
        // old behaviour, but a drag was a pan and must not cancel anything
        if (!moved && !wiring_ && tool_ != Tool::Place) {
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
    if (sel_[0] == '#') {
        if (sel_.rfind("#wire", 0) == 0) {
            int i = std::atoi(sel_.c_str() + 5);
            if (i >= 0 && i < int(doc_->wires.size()))
                doc_->wires.erase(doc_->wires.begin() + i);
        } else if (sel_.rfind("#label", 0) == 0) {
            int i = std::atoi(sel_.c_str() + 6);
            if (i >= 0 && i < int(doc_->labels.size()))
                doc_->labels.erase(doc_->labels.begin() + i);
        }
        set_selection("");
        notify_doc();
        return;
    }
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
        if (wiring_ || tool_ == Tool::Place) {
            wiring_ = false;
            wire_draft_.clear();
            set_tool(Tool::Select);
            Refresh();
            return true;
        }
        set_selection("");
        return true;
    }
    return false;
}

} // namespace symcirc
