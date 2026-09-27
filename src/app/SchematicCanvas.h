#pragma once
#include "Document.h"
#include "core/Netlist.h"

#include <wx/wx.h>

#include <functional>
#include <set>
#include <string>
#include <vector>

namespace symcirc {

// Tools available on the canvas (mirrors the palette).
enum class Tool { Select, Wire, Delete, Place, Label };

// What the selection currently points at. A selection can span several
// components (box select / shift-click).
struct Selection {
    enum Type { None, Component, Wire, WireSegment, Label } type = None;
    std::string ref; // Component ref (primary)
    int wire = -1;   // Wire index
    int seg = -1;    // segment within a wire (WireSegment)
    int label = -1;  // Label index
};

class SchematicCanvas : public wxWindow {
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
    const std::set<std::string>& selected() const { return sel_set_; }

    // Notify the outside world that the document changed (dirty flag set
    // by the caller's edit path) or selection changed.
    std::function<void()> on_document_changed;
    std::function<void(const std::string&)> on_selection_changed;
    std::function<void(const std::string&)> on_status; // hover hint
    std::function<void()> on_push_undo; // called before any mutating op
    // Fires whenever the view (zoom / pan origin) changes; the frame mirrors
    // it into the second status-bar field so the current "zoom=.. view=.." is
    // always on screen. Doubles as a debugging hook.
    std::function<void(double, double, double)> on_view_changed;
    double view_x() const { return view_x_; }
    double view_y() const { return view_y_; }
    void report_view(); // emit on_view_changed with the current view
    // A wire was selected; the panel may want to offer a net name.
    std::function<void(int, std::string)> on_wire_selected;

    // Transform the pending ghost (Place) or the selected component.
    void rotate_ghost(int delta);        // Space = +90
    void flip_ghost(bool horizontal);    // Shift+Space / Ctrl+Space
    void toggle_wire_orient();           // swap H-first <-> V-first routing

    void rotate_selection();
    void flip_selection_h();
    void flip_selection_v();
    void delete_selection();  // Delete
    void copy_selection();    // Ctrl+C
    void paste_clipboard();   // Ctrl+V (paste at the cursor)
    void cancel_current();    // Escape
    bool handle_key(wxKeyEvent& e);

    // Wiring control (used by keys from the frame): commit the current segment
    // (Enter) or leave the mode, dropping only the uncommitted segment (Esc).
    void wire_commit_segment();
    void wire_finish();

    // Net-label placement: begin placing the given label names (space
    // separated). Each click drops one label on the clicked net, in order.
    void begin_label(const std::string& names);
    bool labeling() const { return tool_ == Tool::Label; }

    // Zoom / view. `zoom_to_fit` frames every component; `clamp_view`
    // scrolls back if the sheet has been panned entirely out of view.
    void zoom_to_fit();
    void clamp_view();
    double zoom() const { return zoom_; }
    void set_show_grid(bool on) { show_grid_ = on; Refresh(false); }

    // Name the net a wire belongs to (creates a label anchored on it).
    void set_wire_net_name(int wire_index, const std::string& name);
    void set_label_font_size(int label_index, int size);

    // Invalidate the cached net map; called by the frame when the document is
    // edited outside the canvas (props-panel delete button, undo/redo via the
    // frame, file open/new). The canvas's own edits invalidate via
    // notify_doc().
    void invalidate_nets() { net_cache_valid_ = false; }

private:
    Document* doc_;
    Tool tool_ = Tool::Select;
    bool placing_ = false; // a ghost is following the cursor
    syms::Kind place_kind_ = syms::Kind::R;
    int place_rot_ = 0;
    bool place_flip_h_ = false, place_flip_v_ = false;
    std::string sel_;
    std::set<std::string> sel_set_; // multi-component selection

    // In-app clipboard for Ctrl+C / Ctrl+V. Holds whole components plus the
    // wires that connect them, so pasting a diff pair or a mirror keeps its
    // internal wiring.
    struct ClipComp {
        syms::Component comp;
        Placement place;
    };
    struct ClipWire {
        std::vector<Pt> pts;
        WireEnd a, b;
    };
    std::vector<ClipComp> clip_comps_;
    std::vector<ClipWire> clip_wires_;

    // interaction state
    bool dragging_ = false;
    std::vector<std::pair<std::string, Pt>> drag_start_; // ref -> original origin
    Pt drag_anchor_{0, 0}; // doc point where the drag began
    // Wire/segment drag: the indices of every wire being dragged, plus the
    // original vertex positions, so we can compute the drag delta in doc
    // units and translate "tributary" geometry (other wires sharing a vertex,
    // labels anchored on a moved vertex) accordingly.
    int drag_wire_ = -1;
    std::vector<Pt> drag_wire_pts_backup_;
    int drag_label_ = -1;
    Pt drag_label_pt_backup_;
    Pt drag_label_anchor_backup_;
    bool box_selecting_ = false;
    Pt box_a_{0, 0}, box_b_{0, 0};
    // Wiring (Cadence-style): `wiring_` is the mode, `wire_pts_` holds the
    // committed vertices of the wire being drawn (kept in sync with the doc
    // wire at `wire_idx_`). The "current segment" is the rubber band from
    // wire_pts_.back() to the snapped cursor; it is committed on each click
    // or on Enter, and dropped by Esc.
    bool wiring_ = false;
    std::vector<Pt> wire_pts_;
    int wire_idx_ = -1; // index of the in-progress wire in doc_->wires, or -1
    bool wire_h_first_ = true; // routing preference
    bool show_grid_ = true;    // dot lattice on/off (View toggle)
    std::vector<std::string> label_queue_; // pending net names to place
    wxPoint mouse_;
    bool has_mouse_ = false; // mouse_ has seen at least one event
    std::string hover_;
    std::string hover_net_; // resolved net name at the cursor, "" if none
    Pt hover_world_{-1e18, -1e18}; // world point of the hovered conductor;
                                   // used to anchor the tooltip so it tracks
                                   // the document, not the cursor -- which is
                                   // what makes pan/zoom carry it naturally
                                   // instead of leaving a stale box floating
                                   // in screen space.

    // Cached union-find net map, rebuilt lazily and invalidated on edits.
    // The hover tooltip needs the resolved net name on *every* mouse move, and
    // Document::net_map() is O(n^2) -- far too slow to call in on_motion.
    NetMap net_cache_;
    bool net_cache_valid_ = false;
    const NetMap& nets();
    std::string net_name_wire_cached(int wi);
    std::string net_name_pin_cached(const std::string& ref, int pin);

    // view: an origin + zoom, giving effectively infinite panning
    double zoom_ = 1.0;
    double view_x_ = -60.0, view_y_ = -60.0; // document coord at client (0,0)
    bool panning_ = false;
    wxPoint pan_last_;
    int pan_total_ = 0;

    // geometry helpers
    Pt to_doc(const wxPoint& p) const;
    Pt to_view(Pt p) const; // document -> client (for hit testing)
    wxPoint to_screen(Pt p) const; // document -> integer screen (for labels)
    // Electrical hit tolerance in document units (constant on screen: 6 px).
    double snap_r() const { return 6.0 / zoom_; }
    std::string hit_component(Pt p) const;
    int hit_pin(const std::string& ref, Pt p) const; // -1 if none
    int hit_any_pin(Pt p, std::string& ref) const;
    bool hit_wire(Pt p, int& idx) const;                 // whole wire
    bool hit_wire_segment(Pt p, int& idx, int& seg) const;
    bool hit_label(Pt p, int& idx) const;
    Pt snap(Pt p) const;

    // content bounds (components / wires / labels) in document units
    bool content_bounds(double& x0, double& y0, double& x1, double& y1) const;
    void set_zoom(double z, wxPoint anchor);

    // Document points where three or more wire segments meet (or a wire
    // endpoint lands mid-segment on another wire) -- drawn as solder dots.
    std::vector<Pt> junction_pts() const;

    // Move `ref` to a new origin, carrying any wire endpoints that were
    // attached to its pins along with it.
    void move_component(const std::string& ref, double nx, double ny);
    void collapse_collinear(std::vector<Pt>& pts);

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
    void on_capture_changed(wxMouseCaptureChangedEvent& e);
    void on_size(wxSizeEvent& e);

    wxDECLARE_EVENT_TABLE();
};

} // namespace symcirc
