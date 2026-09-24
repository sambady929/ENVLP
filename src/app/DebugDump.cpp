// Debug-only dump of the NetMap: print every pin's world position, its root,
// and the connected label (if any). Built only when DEBUG_DUMP is defined so
// it never affects normal compilation.
#include "Document.h"
#include "SymbolGeom.h"
#include "core/Netlist.h"
#include <cstdio>

namespace symcirc {

#ifdef DEBUG_DUMP
void dump_netmap(const Document& d) {
    auto nm = d.net_map();
    printf("--- NetMap dump (%d roots) ---\n", (int)nm.name.size());
    int n = 0;
    for (size_t i = 0; i < d.circuit.comps.size(); ++i) {
        const auto& c = d.circuit.comps[i];
        auto offs = pin_offsets(c.kind);
        for (size_t p = 0; p < offs.size(); ++p) {
            auto wp = pin_world(c, c.pl, int(p));
            int root = nm.pin_root.empty() ? -1 :
                       (i < nm.pin_comp.size() && nm.pin_comp[i] == (int)i) ?
                       nm.pin_root[i] : -1;
            (void)root;
            n++;
            printf("  comp %zu (%s) pin%zu = (%.0f,%.0f)\n",
                i, pw(c.kind), p, wp.first, wp.second);
        }
    }
    printf("  -- roots --\n");
    for (const auto& kv : nm.name) {
        printf("    root %d -> '%s'\n", kv.first, kv.second.c_str());
    }
}
#else
void dump_netmap(const Document&) {}
#endif

} // namespace symcirc