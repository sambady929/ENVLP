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
} // namespace

SchematicCanvas::SchematicCanvas(wxWindow* parent, Document* doc)
    : wxScrolledWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                       wxHSCROLL | wxVSCROLL | wxWANTS_CHARS),
      doc_(doc) {
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetBackgroundColour(wxColour(250, 250, 248));
    SetVirtualSize(2000, 1400);
    SetScrollRate(20, 20);
}

void SchematicCanvas::set_tool(Tool t, Kind k) {
    tool_ = t;
    place_kind_ = k;
    wiring_ = false;
    wire_draft_.clear();
    Refresh();
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
    return {double(x), double(y)};
}

Pt SchematicCanvas::snap(Pt p) const {
    return {std::round(p.first / kGrid) * kGrid,
            std::round(p.second / kGrid) * kGrid};
}

std::string SchematicCanvas::hit_component(Pt p) const {
    // topmost last drawn wins -> iterate in reverse
    for (auto it = doc_->circuit.comps.rbegin();
         it != doc_->circuit.comps.rend(); ++it) {
        auto pl = doc_->placements.find(it->ref);
        if (pl == doc_->placements.end()) continue;
        double x0, y0, x1, y1;
        symbol_bbox(*it, pl->second, x0, y0, x1, y1);
        if (p.first >= x0 && p.first <= x1 && p.second >= y0 &&
            p.second <= y1)
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
            if (seg_dist(p, w.pts[k - 1], w.pts[k]) <= 5.0) {
                idx = i;
                return true;
            }
    }
    return false;
}

bool SchematicCanvas::hit_label(Pt p, int& idx) const {
    for (int i = int(doc_->labels.size()) - 1; i >= 0; --i)
        if (dist(doc_->labels[i].pt, p) <= 12.0) {
            idx = i;
            return true;
        }
    return false;
}

// ---------------------------------------------------------------------------
void SchematicCanvas::on_paint(wxPaintEvent&) {
    wxAutoBufferedPaintDC dc(this);
    DoPrepareDC(dc);

    // grid
    dc.SetPen(wxPen(wxColour(228, 228, 224)));
    wxPoint tl;
    CalcUnscrolledPosition(0, 0, &tl.x, &tl.y);
    wxSize cs = GetClientSize();
    wxPoint br;
    CalcUnscrolledPosition(cs.x, cs.y, &br.x, &br.y);
    for (double x = std::floor(tl.x / kGrid) * kGrid; x < br.x + kGrid;
         x += kGrid)
        for (double y = std::floor(tl.y / kGrid) * kGrid; y < br.y + kGrid;
             y += kGrid)
            dc.DrawPoint(wxPoint(int(x), int(y)));

    // wires
    for (size_t i = 0; i < doc_->wires.size(); ++i) {
        const auto& w = doc_->wires[i];
        bool is_sel = sel_ == "#wire" + std::to_string(i);
        dc.SetPen(wxPen(is_sel ? wxColour(0, 120, 215) : wxColour(70, 70, 75),
                        is_sel ? 3 : 2));
        for (size_t k = 1; k < w.pts.size(); ++k)
            dc.DrawLine(wxPoint(int(w.pts[k - 1].first), int(w.pts[k - 1].second)),
                        wxPoint(int(w.pts[k].first), int(w.pts[k].second)));
    }

    // wire in progress
    if (wiring_ && !wire_draft_.empty()) {
        dc.SetPen(wxPen(wxColour(0, 150, 90), 2, wxPENSTYLE_SHORT_DASH));
        for (size_t k = 1; k < wire_draft_.size(); ++k)
            dc.DrawLine(
                wxPoint(int(wire_draft_[k - 1].first), int(wire_draft_[k - 1].second)),
                wxPoint(int(wire_draft_[k].first), int(wire_draft_[k].second)));
        if (has_mouse_) {
            Pt m = to_doc(mouse_);
            Pt last = wire_draft_.back();
            dc.DrawLine(wxPoint(int(last.first), int(last.second)),
                        wxPoint(int(m.first), int(m.second)));
        }
    }

    // labels
    dc.SetTextForeground(wxColour(140, 60, 160));
    for (size_t i = 0; i < doc_->labels.size(); ++i) {
        const auto& l = doc_->labels[i];
        bool is_sel = sel_ == "#label" + std::to_string(i);
        wxString txt = wxString::FromUTF8(l.name);
        wxSize ts = dc.GetTextExtent(txt);
        wxPoint p(int(l.pt.first) - ts.x / 2, int(l.pt.second) - ts.y - 6);
        dc.SetTextBackground(is_sel ? wxColour(0, 120, 215)
                                    : GetBackgroundColour());
        dc.SetTextForeground(is_sel ? *wxWHITE : wxColour(140, 60, 160));
        dc.DrawText(txt, p.x, p.y);
        dc.SetTextBackground(GetBackgroundColour());
    }

    // components
    for (const auto& c : doc_->circuit.comps) {
        auto pl = doc_->placements.find(c.ref);
        if (pl == doc_->placements.end()) continue;
        draw_symbol(dc, c, pl->second, sel_ == c.ref);
    }

    // pin dots (subtle) + hover highlight while wiring
    dc.SetPen(wxPen(wxColour(150, 150, 155)));
    for (const auto& c : doc_->circuit.comps) {
        auto pl = doc_->placements.find(c.ref);
        if (pl == doc_->placements.end()) continue;
        int n = int(pin_offsets(c.kind).size());
        for (int i = 0; i < n; ++i) {
            Pt w = pin_world(c, pl->second, i);
            dc.DrawCircle(wxPoint(int(w.first), int(w.second)), 2);
        }
    }
    if (wiring_) {
        std::string ref;
        Pt m = to_doc(mouse_);
        if (hit_any_pin(m, ref) >= 0) {
            int i = hit_any_pin(m, ref);
            const Component* c = doc_->circuit.find(ref);
            auto pl = doc_->placements.find(ref);
            if (c && pl != doc_->placements.end()) {
                Pt w = pin_world(*c, pl->second, i);
                dc.SetPen(wxPen(wxColour(0, 150, 90), 2));
                dc.SetBrush(*wxTRANSPARENT_BRUSH);
                dc.DrawCircle(wxPoint(int(w.first), int(w.second)), 6);
            }
        }
    }

    // ghost of component being placed
    if (tool_ == Tool::Place && has_mouse_) {
        Pt m = snap(to_doc(mouse_));
        Component tmp;
        tmp.ref = "?";
        tmp.kind = place_kind_;
        Placement pl{m.first, m.second, 0};
        dc.SetPen(wxPen(wxColour(0, 150, 90), 1, wxPENSTYLE_DOT));
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
        c.nodes.assign(syms::pin_count(place_kind_), "");
        std::string ref = doc_->add(c, s.first, s.second);
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
        Pt target = p;
        int pin = hit_any_pin(p, ref);
        if (pin >= 0) target = snap(p); // pins are on the grid by construction
        else target = snap(p);

        if (!wiring_) {
            wiring_ = true;
            wire_draft_.clear();
            wire_draft_.push_back(target);
        } else {
            wire_draft_.push_back(target);
            // finish when clicking an endpoint pin (and we have >= 2 pts and
            // this click is on a pin different from the start's pin or the
            // same pin twice = no-op)
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
            if (pl != doc_->placements.end()) {
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

    if (dragging_ && !sel_.empty() && sel_[0] != '#') {
        auto pl = doc_->placements.find(sel_);
        if (pl != doc_->placements.end()) {
            pl->second.x = std::round((p.first + drag_dx_) / kGrid) * kGrid;
            pl->second.y = std::round((p.second + drag_dy_) / kGrid) * kGrid;
            Refresh();
        }
        return;
    }

    // hover feedback
    std::string nh;
    if (tool_ == Tool::Wire && wiring_) nh = "wire";
    else if (tool_ == Tool::Delete) {
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
    if (wiring_) {
        // commit draft as an open wire (segments up to here) only if 2+ points
        if (wire_draft_.size() >= 2) {
            Wire w;
            w.pts = wire_draft_;
            doc_->wires.push_back(w);
            notify_doc();
        }
        wiring_ = false;
        wire_draft_.clear();
        Refresh();
        return;
    }
    // right-click cycles tool back to Select for convenience
    set_tool(Tool::Select);
    if (on_status) on_status("");
}

void SchematicCanvas::on_leave(wxMouseEvent&) {
    if (tool_ == Tool::Place) Refresh();
}

// ---------------------------------------------------------------------------
void SchematicCanvas::rotate_selection() {
    if (sel_.empty() || sel_[0] == '#') return;
    auto pl = doc_->placements.find(sel_);
    if (pl == doc_->placements.end()) return;
    pl->second.rot = (pl->second.rot + 90) % 360;
    notify_doc();
}

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
    case 'r':
    case 'R':
        rotate_selection();
        return true;
    case WXK_ESCAPE:
        if (wiring_) {
            wiring_ = false;
            wire_draft_.clear();
            Refresh();
            return true;
        }
        set_selection("");
        return true;
    }
    return false;
}

} // namespace symcirc
