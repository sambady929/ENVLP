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

// A user-assigned net name placed at a point that belongs to a net.
struct NetLabel {
    Pt pt{0, 0};
    std::string name;
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
