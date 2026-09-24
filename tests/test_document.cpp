// SymCirc Document tests: net-name resolution, label persistence (the new
// anchor / display-point model), and round-trip serialization. These are the
// pieces that back the Cadence-style wire interaction in the GUI.
//
// Built as test_document; run by CTest. Pulls in app/Document.cpp and
// app/Symbols.cpp directly so we can exercise the net_map / net_name_of_*
// helpers without dragging in wxWidgets.
#include "app/Document.h"

#include "core/Netlist.h"

#include <cmath>
#include <cstdio>
#include <string>

using namespace symcirc;
using namespace syms;

static int g_fail = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            ++g_fail;                                                         \
            std::printf("  FAIL line %d: %s\n", __LINE__, #c);                \
        }                                                                     \
    } while (0)

// Build a small schematic: a voltage source V1 driving a resistive divider R1
// / R2 with an explicit GND, plus a label on the divider's tap.
//
// Geometry chosen so wires land exactly on pin positions:
//   V1 at (0, 50)  -> pins at (0, 20) "+" and (0, 80) "-"
//   R1 at (100, 50) -> pins at (70, 50) and (130, 50)
//   R2 at (200, 50) -> pins at (170, 50) and (230, 50)
//   GND at (200, 150) -> pin at (200, 150)
static Document divider_doc() {
    Document d;
    Component v;
    v.kind = Kind::V;
    v.ref = "V1";
    v.value_text = "1";
    v.nodes = {"in", "0"};
    d.circuit.comps.push_back(v);

    Component r1;
    r1.kind = Kind::R;
    r1.ref = "R1";
    r1.value_text = "1k";
    r1.nodes = {"in", "tap"};
    d.circuit.comps.push_back(r1);

    Component r2;
    r2.kind = Kind::R;
    r2.ref = "R2";
    r2.value_text = "1k";
    r2.nodes = {"tap", "0"};
    d.circuit.comps.push_back(r2);

    Component g;
    g.kind = Kind::GND;
    g.ref = "GND1";
    g.nodes = {"0"};
    d.circuit.comps.push_back(g);

    Placement pl;
    pl.x = 0; pl.y = 50;
    d.placements["V1"] = pl;
    pl.x = 100; pl.y = 50;
    d.placements["R1"] = pl;
    pl.x = 200; pl.y = 50;
    d.placements["R2"] = pl;
    pl.x = 200; pl.y = 150;
    d.placements["GND1"] = pl;

    // Three straight wires connect the components pin-to-pin.
    Wire w;
    w.pts = {{0, 20}, {70, 50}, {70, 50}};   // V1 + -> R1 left
    // (slight redundancy isn't ideal; use 2-point wires everywhere)
    d.wires.clear();
    w.pts = {{0, 20}, {70, 50}};
    d.wires.push_back(w);
    w.pts = {{130, 50}, {170, 50}};
    d.wires.push_back(w);
    w.pts = {{230, 50}, {200, 150}};
    d.wires.push_back(w);
    return d;
}

// The union-find net map correctly groups pins, wires, and label anchors.
static void test_net_map_topology() {
    Document d = divider_doc();
    NetMap nm = d.net_map();
    // 4 components, GND has 1 pin, V has 2, R1/R2 each have 2 = 7 pins.
    CHECK(nm.pin_comp.size() == 7);
    CHECK(nm.wire_root.size() == d.wires.size());
    // Print every pin's resolved name and every wire's resolved name for
    // diagnosis if this test ever fails.
    std::printf("pin roots/names:\n");
    for (size_t i = 0; i < nm.pin_comp.size(); ++i) {
        const auto& c = d.circuit.comps[nm.pin_comp[i]];
        int r = nm.pin_root[i];
        std::printf("  %s.%d -> %s\n", c.ref.c_str(), nm.pin_index[i],
                    nm.name.count(r) ? nm.name[r].c_str() : "?");
    }
    std::printf("wire roots/names:\n");
    for (size_t i = 0; i < d.wires.size(); ++i) {
        int r = nm.wire_root[i];
        std::printf("  wire%zu -> %s\n", i,
                    nm.name.count(r) ? nm.name[r].c_str() : "?");
    }
    // Three distinct nets: in, tap, 0.
    int distinct = 0;
    int last = -1;
    for (int r : nm.wire_root) {
        if (r != last) { ++distinct; last = r; }
    }
    CHECK(distinct == 3);
}

// net_name_of_wire returns the resolved (auto or label) name even when no
// label is present. Auto names are deterministic (n1, n2, ...) in topological
// order -- wires are the only "anonymous" net anchors here, so wires get the
// first auto names.
static void test_net_name_default() {
    Document d = divider_doc();
    // Two of the three nets get auto names; the GND-pin net is named "0".
    // We don't pin the exact auto names (they depend on traversal order), but
    // every wire's resolved name must be non-empty and "0" must be reserved
    // for the ground net.
    CHECK(!d.net_name_of_wire(0).empty());
    CHECK(!d.net_name_of_wire(1).empty());
    CHECK(d.net_name_of_wire(2) == "0");
    // All three nets are distinct.
    CHECK(d.net_name_of_wire(0) != d.net_name_of_wire(1));
    CHECK(d.net_name_of_wire(1) != d.net_name_of_wire(2));
    CHECK(d.net_name_of_wire(0) != d.net_name_of_wire(2));
}

// Placing a label overrides the auto-name; the resolved name comes back
// through net_name_of_wire.
static void test_label_overrides_auto() {
    Document d = divider_doc();
    int li = d.ensure_label_on_wire(1, {150, 50});
    CHECK(li >= 0);
    d.labels[li].name = "vout";
    CHECK(d.net_name_of_wire(1) == "vout");
    // The connected R1 / R2 pins also report the new name (the same net).
    CHECK(d.net_name_of_pin("R1", 1) == "vout");
    CHECK(d.net_name_of_pin("R2", 0) == "vout");
}

// The anchor / display-point split: moving the display point off the wire
// does NOT detach the label -- the wire keeps the resolved name. This is the
// behaviour the GUI relies on for the "drag the text but keep the wire
// labelled" interaction.
static void test_label_anchor_is_stable() {
    Document d = divider_doc();
    int li = d.ensure_label_on_wire(1, {150, 50});
    d.labels[li].name = "vout";
    CHECK(d.net_name_of_wire(1) == "vout");
    // Move the display point far from the wire.
    d.labels[li].pt = {150, 500};
    // Anchor is unchanged: the wire still resolves to "vout".
    CHECK(d.net_name_of_wire(1) == "vout");
    // And the pin name reflects it.
    CHECK(d.net_name_of_pin("R1", 1) == "vout");
}

// Serialization round-trip preserves the anchor, display point, name, font
// size, and rotation of every label.
static void test_serialize_round_trip() {
    Document a = divider_doc();
    int li = a.ensure_label_on_wire(1, {150, 50});
    a.labels[li].name = "vout";
    a.labels[li].font_size = 18;
    a.labels[li].rot = 90;
    // Move the display point so it's different from the anchor.
    a.labels[li].pt = {200, 30};

    std::string s = a.serialize();
    Document b;
    std::string err;
    CHECK(b.deserialize(s, err));
    CHECK(b.labels.size() == a.labels.size());
    CHECK(b.labels[li].name == "vout");
    CHECK(b.labels[li].font_size == 18);
    CHECK(b.labels[li].rot == 90);
    CHECK(b.labels[li].pt.first == 200);
    CHECK(b.labels[li].pt.second == 30);
    // The anchor should also survive (it's the same as the wire midpoint).
    CHECK(std::fabs(b.labels[li].anchor.first -
                    a.labels[li].anchor.first) < 1e-6);
    CHECK(std::fabs(b.labels[li].anchor.second -
                    a.labels[li].anchor.second) < 1e-6);
}

// Old files (anchor == display point) still load: the format is a single
// point token followed by a quoted name.
static void test_serialize_legacy_loads() {
    std::string data =
        "symcirc 1\n"
        "req \"V1\" \"V(out)\" 1 1e9 0 10 1 1 1 1\n"
        "comp \"V1\" \"V\" 0 0 0 0 0 0 \"1\"\n"
        "comp \"GND1\" \"GND\" 0 100 0 0 0 0 \"\"\n"
        "wire 0,50 100,50\n"
        "netlabel 100,50 \"out\"\n";
    Document d;
    std::string err;
    CHECK(d.deserialize(data, err));
    CHECK(d.labels.size() == 1);
    // Legacy load: anchor = pt = (100, 50).
    CHECK(d.labels[0].anchor.first == 100);
    CHECK(d.labels[0].anchor.second == 50);
    CHECK(d.labels[0].pt.first == 100);
    CHECK(d.labels[0].pt.second == 50);
    CHECK(d.labels[0].name == "out");
    CHECK(d.labels[0].rot == 0);
}

int main() {
    test_net_map_topology();
    test_net_name_default();
    test_label_overrides_auto();
    test_label_anchor_is_stable();
    test_serialize_round_trip();
    test_serialize_legacy_loads();
    std::printf("%s (%d failure(s))\n",
                g_fail ? "DOCUMENT FAILED" : "document ok", g_fail);
    return g_fail == 0 ? 0 : 1;
}
