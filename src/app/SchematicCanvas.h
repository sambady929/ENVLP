#pragma once
#include "Document.h"
#include "core/Netlist.h"

#include <wx/wx.h>

#include <functional>
#include <string>

namespace symcirc {

// Tools available on the canvas (mirrors the palette).
enum class Tool { Select, Wire, Delete, Place, Label };

// What the selection currently points at.
struct Selection {
    enum Type { None, Component, Wire, WireSegment, Label } type = None;
    std::string ref; // Component ref
    int wire = -1;   // Wire index
    int seg = -1;    // segment within a wire (WireSegment)
    int label = -1;  // Label index
};

class SchematicCanvas : public wxScrolledWindow {
public:
    SchematicCanvas(wxWindow* parent, Document* doc);

    void set_tool(Tool t, syms::Kind k = syms::Kind::R);
    Tool tool() const { return tool_; }
    syms::Kind place_kind() const { return place_kind_; }
    bool placing() const { return placing_; }
    bool wiring() const { return wiring_; }

    // Enter placement mode for a specific kind (keyboard shortcuts / menu).
    void begin_place(syms::Kind k, int rot = 0);

    // Selection. The canonical string form keeps the older sentinels
    // ("", ref, "#wireN", "#wireN:segM", "#labelN") so the rest of the UI
    // keeps working; structured access is via selection_info().
    const std::string& selection() const { return sel_; }
    void set_selection(const std::string& s);
    Selection selection_info() const;

    // Notify the outside world that the document changed (dirty flag set
    // by the caller's edit path) or selection changed.
    std::function<void()> on_document_changed;
    std::function<void(const std::string&)> on_selection_changed;
    std::function<void(const std::string&)> on_status; // hover hint
    std::function<void()> on_push_undo; // called before any mutating op

    // Transform the pending ghost (Place) or the selected component.
    void rotate_ghost(int delta);        // Space = +90
    void flip_ghost(bool horizontal);    // Shift+Space / Ctrl+Space
    void toggle_wire_orient();           // swap H-first <-> V-first routing

    void rotate_selection();
    void flip_selection_h();
    void flip_selection_v();
    void delete_selection();  // Delete
    void cancel_current();    // Escape
    bool handle_key(wxKeyEvent& e);

    // Net-label placement: begin placing the given label names (space
    // separated). Each click drops one label on the clicked net, in order.
    void begin_label(const std::string& names);
    bool labeling() const { return tool_ == Tool::Label; }

private:
    Document* doc_;
    Tool tool_ = Tool::Select;
    bool placing_ = false; // a ghost is following the cursor
    syms::Kind place_kind_ = syms::Kind::R;
    int place_rot_ = 0;
    bool place_flip_h_ = false, place_flip_v_ = false;
    std::string sel_;

    // interaction state
    bool dragging_ = false;
    double drag_dx_ = 0, drag_dy_ = 0;
    bool wiring_ = false;
    std::vector<Pt> wire_draft_;
    bool wire_h_first_ = true; // routing preference for the rubber band
    Pt wire_anchor_{0, 0};     // snapped point where the current wire started
    std::vector<std::string> label_queue_; // pending net names to place
    wxPoint mouse_;
    bool has_mouse_ = false; // mouse_ has seen at least one event
    std::string hover_;

    // view
    double zoom_ = 1.0;
    bool panning_ = false;
    wxPoint pan_last_;
    int pan_total_ = 0;

    // geometry helpers
    Pt to_doc(const wxPoint& p) const;
    Pt to_view(Pt p) const; // document -> client (for hit testing)
    std::string hit_component(Pt p) const;
    int hit_pin(const std::string& ref, Pt p) const; // -1 if none
    int hit_any_pin(Pt p, std::string& ref) const;
    bool hit_wire(Pt p, int& idx) const;                 // whole wire
    bool hit_wire_segment(Pt p, int& idx, int& seg) const;
    bool hit_label(Pt p, int& idx) const;
    Pt snap(Pt p) const;

    // Move `ref` to a new origin, carrying any wire endpoints that were
    // attached to its pins along with it.
    void move_component(const std::string& ref, double nx, double ny);

    void notify_doc();
    void notify_sel();

    void on_paint(wxPaintEvent& e);
    void on_left_down(wxMouseEvent& e);
    void on_left_up(wxMouseEvent& e);
    void on_left_dclick(wxMouseEvent& e);
    void on_motion(wxMouseEvent& e);
    void on_right_down(wxMouseEvent& e);
    void on_right_up(wxMouseEvent& e);
    void on_mousewheel(wxMouseEvent& e);
    void on_leave(wxMouseEvent& e);

    wxDECLARE_EVENT_TABLE();
};

} // namespace symcirc
