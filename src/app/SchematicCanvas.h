#pragma once
#include "Document.h"
#include "core/Netlist.h"

#include <wx/wx.h>

#include <functional>
#include <string>

namespace symcirc {

// Tools available on the canvas (mirrors the palette).
enum class Tool { Select, Wire, Delete, Place };

class SchematicCanvas : public wxScrolledWindow {
public:
    SchematicCanvas(wxWindow* parent, Document* doc);

    void set_tool(Tool t, syms::Kind k = syms::Kind::R);
    Tool tool() const { return tool_; }
    syms::Kind place_kind() const { return place_kind_; }

    // Enter placement mode for a specific kind (keyboard shortcuts / menu).
    void begin_place(syms::Kind k, int rot = 0);

    // Selection is a component ref ("" = none). Wire/label selection uses
    // negative sentinels: "#wireN", "#labelN".
    const std::string& selection() const { return sel_; }
    void set_selection(const std::string& s);

    // Notify the outside world that the document changed (dirty flag set
    // by the caller's edit path) or selection changed.
    std::function<void()> on_document_changed;
    std::function<void(const std::string&)> on_selection_changed;
    std::function<void(const std::string&)> on_status; // hover hint

    // Transform the pending ghost (Place) or the selected component.
    void rotate_ghost(int delta);      // Space = +90
    void flip_ghost(bool horizontal);  // Shift+Space / Ctrl+Space

    void rotate_selection();
    void flip_selection_h();
    void flip_selection_v();
    void delete_selection();  // Delete
    bool handle_key(wxKeyEvent& e);

private:
    Document* doc_;
    Tool tool_ = Tool::Select;
    syms::Kind place_kind_ = syms::Kind::R;
    int place_rot_ = 0;
    bool place_flip_h_ = false, place_flip_v_ = false;
    std::string sel_;

    // interaction state
    bool dragging_ = false;
    double drag_dx_ = 0, drag_dy_ = 0;
    bool wiring_ = false;
    std::vector<Pt> wire_draft_;
    wxPoint mouse_;
    bool has_mouse_ = false; // mouse_ has seen at least one event
    std::string hover_;

    // geometry helpers
    Pt to_doc(const wxPoint& p) const;
    std::string hit_component(Pt p) const;
    int hit_pin(const std::string& ref, Pt p) const; // -1 if none
    int hit_any_pin(Pt p, std::string& ref) const;
    bool hit_wire(Pt p, int& idx) const;
    bool hit_label(Pt p, int& idx) const;
    Pt snap(Pt p) const;

    void notify_doc();
    void notify_sel();

    void on_paint(wxPaintEvent& e);
    void on_left_down(wxMouseEvent& e);
    void on_left_up(wxMouseEvent& e);
    void on_motion(wxMouseEvent& e);
    void on_right_down(wxMouseEvent& e);
    void on_leave(wxMouseEvent& e);

    wxDECLARE_EVENT_TABLE();
};

} // namespace symcirc
