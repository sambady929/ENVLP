// Regression: moving/rotating a component must carry its wires via the
// endpoint *binding* (analog-canvas model), not by hunting for vertices that
// happen to coincide with the old pin position. After the move the wire's
// endpoint must resolve to the pin's new world position, and the net map
// must still see the pin connected to the far end of the wire.
#include "app/Document.h"
#include "app/SymbolGeom.h"

#include <cmath>
#include <cstdio>

using symcirc::Document;
using symcirc::Pt;
using symcirc::Wire;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { ++g_fail; std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

int main() {
    // Build: V1 --wire--> R1, with the wire endpoint bound to R1's pin 0.
    Document d;
    syms::Component v;
    v.ref = "V1";
    v.kind = syms::Kind::V;
    v.nodes = {"in", "0"};
    v.value_text = "1";
    d.add(v, 100, 100);

    syms::Component r;
    r.ref = "R1";
    r.kind = syms::Kind::R;
    r.nodes = {"a", "b"};
    r.value_text = "1k";
    d.add(r, 300, 100);

    // Wire from V1's + pin (100, 70) to R1's pin 0 (270, 100).
    Wire w;
    w.pts = {{100, 70}, {300, 70}, {300, 100}, {270, 100}};
    d.bind_wire_ends(w);
    d.wires.push_back(w);

    // The wire's far end should be bound to R1:0.
    CHECK(d.wires[0].a.kind == symcirc::WireEnd::Kind::Pin);
    CHECK(d.wires[0].a.ref == "V1");
    CHECK(d.wires[0].b.kind == symcirc::WireEnd::Kind::Pin);
    CHECK(d.wires[0].b.ref == "R1");
    CHECK(d.wires[0].b.pin == 0);

    // Move R1 far away. sync_wire_endpoints must carry the wire's end.
    auto pl = d.placements.find("R1");
    pl->second.x = 600;
    pl->second.y = 250;
    d.sync_wire_endpoints();

    Pt new_end = d.wire_end_pt(d.wires[0], false);
    CHECK(std::fabs(new_end.first - 570.0) < 1e-6);
    CHECK(std::fabs(new_end.second - 250.0) < 1e-6);
    // The stored last point must match the resolved end.
    CHECK(std::fabs(d.wires[0].pts.back().first - 570.0) < 1e-6);
    CHECK(std::fabs(d.wires[0].pts.back().second - 250.0) < 1e-6);

    // Every segment must still be axis-aligned (orthogonal re-route).
    for (size_t i = 0; i + 1 < d.wires[0].pts.size(); ++i) {
        bool axis = std::fabs(d.wires[0].pts[i].first -
                              d.wires[0].pts[i + 1].first) < 1e-6 ||
                    std::fabs(d.wires[0].pts[i].second -
                              d.wires[0].pts[i + 1].second) < 1e-6;
        CHECK(axis);
    }

    // Connectivity: R1:0 must share a net with the wire's far vertex, and
    // V1:+ with the wire's near vertex. The net map resolves the pin-bound
    // endpoints directly, so it sees the connection even without a sync.
    auto nm = d.net_map();
    int ci_r = -1, ci_v = -1;
    for (size_t i = 0; i < d.circuit.comps.size(); ++i) {
        if (d.circuit.comps[i].ref == "R1") ci_r = int(i);
        if (d.circuit.comps[i].ref == "V1") ci_v = int(i);
    }
    CHECK(ci_r >= 0 && ci_v >= 0);
    int r_root = nm.root_of_pin(ci_r, 0);
    int v_root = nm.root_of_pin(ci_v, 0);
    // The two wire ends are NOT the same net (V1 --...-- R1 with the wire
    // being the conductor; both ends belong to the same wire, so they ARE
    // one net). Verify they resolve to a single root.
    CHECK(r_root == v_root);

    // Idempotency: a second sync with no placement change must not add any
    // vertices (the escape-lead insertion has to replace, not accumulate --
    // otherwise a slow drag grows the polyline on every motion event).
    size_t before = d.wires[0].pts.size();
    d.sync_wire_endpoints();
    CHECK(d.wires[0].pts.size() == before);

    // Escape direction: R1's pin 0 is its left pin (offset -30,0), so a wire
    // leaving it must head west: the vertex right after the pin has the same
    // y and a smaller x.
    const auto& pts = d.wires[0].pts;
    Pt end = pts.back();
    Pt lead = pts[pts.size() - 2];
    CHECK(std::fabs(lead.second - end.second) < 1e-6);
    CHECK(lead.first < end.first);

    std::printf("%s (%d failure(s))\n",
                g_fail ? "WIREBIND FAILED" : "wirebind ok", g_fail);
    return g_fail == 0 ? 0 : 1;
}
