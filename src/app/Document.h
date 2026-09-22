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
};

} // namespace symcirc
