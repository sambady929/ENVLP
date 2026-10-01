// Quick diagnostic: load the user's cs_test, dump every pin's WORLD position
// and the net root it belongs to, so we can see *exactly* why R1 ends up
// with mismatched node names.
#include "app/Document.h"
#include "app/SymbolGeom.h"
#include "core/Netlist.h"
#include <cstdio>
#include <cmath>

int main(int argc, char** argv) {
    // The path to the fixture is passed by CMake so the test does not depend
    // on the checkout's folder name.
    const char* path =
        argc > 1 ? argv[1]
                 : "D:/Projects/Programming/ENVLP/examples/cs_test.scx";
    envlp::Document d;
    std::string err;
    if (!d.load(path, err)) {
        std::printf("load failed: %s\n", err.c_str());
        return 1;
    }
    auto nm = d.net_map();
    std::printf("=== wires (%zu) ===\n", d.wires.size());
    for (size_t i = 0; i < d.wires.size(); ++i) {
        std::printf("  w%zu:", i);
        for (auto& p : d.wires[i].pts)
            std::printf(" (%.0f,%.0f)", p.first, p.second);
        std::printf("\n");
    }
    std::printf("=== labels (%zu) ===\n", d.labels.size());
    for (size_t i = 0; i < d.labels.size(); ++i)
        std::printf("  l%zu name='%s' anchor=(%.0f,%.0f) -> root=%d\n",
            i, d.labels[i].name.c_str(),
            d.labels[i].anchor.first, d.labels[i].anchor.second,
            nm.pin_root.empty() ? -1 :
            nm.wire_root.empty() ? -1 : 0);
    std::printf("=== pins ===\n");
    for (size_t i = 0; i < d.circuit.comps.size(); ++i) {
        const auto& c = d.circuit.comps[i];
        auto offs = envlp::pin_offsets(c.kind);
        for (size_t p = 0; p < offs.size(); ++p) {
            auto plt = d.placements.find(c.ref);
            envlp::Placement pl{0,0,0,false,false};
            if (plt != d.placements.end()) pl = plt->second;
            auto wp = envlp::pin_world(c, pl, int(p));
            // Find root: search pin_comp for a match
            int root = -1;
            for (size_t k = 0; k < nm.pin_comp.size(); ++k) {
                if (nm.pin_comp[k] == (int)i && nm.pin_index[k] == (int)p) {
                    root = nm.pin_root[k]; break;
                }
            }
            const char* rn = "-";
            for (const auto& kv : nm.name)
                if (kv.first == root) rn = kv.second.c_str();
            std::printf("  %s pin%zu = (%.0f,%.0f) root=%d name='%s'\n",
                c.ref.c_str(), p, wp.first, wp.second, root, rn);
        }
    }
    return 0;
}