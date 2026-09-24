#pragma once
#include "core/Netlist.h"
#include "core/Solver.h"

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace symcirc {

using Pt = std::pair<double, double>;

// Where a component sits on the schematic canvas (degrees, multiples of 90).
struct Placement {
    double x = 0, y = 0;
    int rot = 0;
    bool flip_h = false;
    bool flip_v = false;
};

// A polyline of wire segments. Consecutive points are electrically one net;
// separate wires that touch (within tolerance) are also one net.
struct Wire {
    std::vector<Pt> pts;
};

// A user-assigned net name placed near a net. `anchor` is the point that
// actually attaches to the net (a point on a wire / a pin); `pt` is where the
// text is drawn. Moving or rotating the label changes only `pt`, so the label
// stays attached to the same net.
struct NetLabel {
    Pt anchor{0, 0};  // attachment point on the net
    Pt pt{0, 0};      // text position (defaults to the anchor)
    std::string name;
    int font_size = 9; // points; matches the component ref/value text size
    int rot = 0;       // text rotation, degrees (multiples of 90)
};

// Resolved net identity for the interactive helpers: the union-find root of
// each pin / wire / label, plus the resolved name of each root.
struct NetMap {
    std::vector<int> pin_root;   // one per pin, in circuit/comp-pin order
    std::vector<int> pin_comp;   // component index of each pin
    std::vector<int> pin_index;  // pin index within the component
    std::vector<int> wire_root;  // one per wire
    std::map<int, std::string> name; // root -> resolved net name
    int root_of_pin(int comp, int pin) const;
};

// The schematic: circuit data + graphical data + analysis request.
// Component.pin node names are *derived* from wire topology at analyze time
// (Document::resolved), so the .scx file stays geometry-based.
class Document {
public:
    syms::Circuit circuit;
    std::map<std::string, Placement> placements; // ref -> placement
    std::vector<Wire> wires;
    std::vector<NetLabel> labels;
    syms::AnalysisRequest req;
    std::string path;  // "" = never saved
    bool dirty = false;

    // Analysis cards are owned by the UI; the document stores them verbatim
    // so a .scx round-trips the configured analysis steps.
    std::string analysis_cards;

    // Adds the component and gives it a free reference/placement.
    std::string add(const syms::Component& c, double x, double y);
    void remove(const std::string& ref);

    // Returns the circuit with every pin's node name filled in from the
    // wire topology. Fills `err` (instead of throwing) on structural
    // problems; returns an empty circuit in that case.
    syms::Circuit resolved(std::string& err) const;

    // Union-find net topology at a point in time: which pins/wires share a net
    // and what each net is called. Used by the interactive helpers below and by
    // resolved() itself, so hit-testing and analysis always agree.
    NetMap net_map() const;

    // Resolved net name of a wire / a component pin (auto "n1"/"0"/"VDD" or a
    // user label). Empty string if the index is invalid.
    std::string net_name_of_wire(int wire_index) const;
    std::string net_name_of_pin(const std::string& ref, int pin) const;
    // Anchor point on the net nearest `near` (for attaching a label).
    Pt net_anchor_near(const std::string& ref, int pin, Pt near) const;
    Pt net_anchor_near_wire(int wire_index, Pt near) const;

    // Suggested *display* point for a net label whose anchor lies on
    // `wire_index`. The text is drawn centred on this point, so it is offset
    // to whichever side of the wire keeps it readable: to the right of a
    // vertical wire, above a horizontal one. `name_len` sizes the offset so
    // the text clears the wire; pass the label's character count.
    Pt label_display_pt(int wire_index, Pt anchor, int name_len) const;

    // Find or create a label attached to the net a wire belongs to; the text
    // is drawn at `at` (defaults to the anchor). Returns the label index.
    int ensure_label_on_wire(int wire_index, Pt at);
    // True if the label still sits on the net it names.
    bool label_attached(int label_index, const NetMap& nm) const;

    bool save(const std::string& p, std::string& err);
    bool load(const std::string& p, std::string& err);

    std::string serialize() const;
    bool deserialize(const std::string& data, std::string& err);

    // Undo / redo: whole-document snapshots of the serialized form, so every
    // kind of edit (components, wires, labels, placements) is covered.
    // Call push_undo() *before* mutating.
    void push_undo();
    bool undo();
    bool redo();
    bool can_undo() const { return !undo_.empty(); }
    bool can_redo() const { return !redo_.empty(); }
    int undo_depth() const { return int(undo_.size()); }
    int redo_depth() const { return int(redo_.size()); }

private:
    std::vector<std::string> undo_, redo_;
};

} // namespace symcirc
