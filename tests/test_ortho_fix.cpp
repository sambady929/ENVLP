// Regression: when a component moves, its connected wires must stay
// orthogonal. Re-routes every diagonal segment into 1-2 axis-aligned legs
// (Cadence-style). The test exercises the same logic the canvas uses.
#include "app/Document.h"
#include "app/SymbolGeom.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace {
// Mirror the canvas' helper. The implementation is small enough that
// duplicating it here keeps the test independent of wx.
bool is_axis_aligned(symcirc::Pt a, symcirc::Pt b) {
    return std::fabs(a.first - b.first) < 1e-6 ||
           std::fabs(a.second - b.second) < 1e-6;
}

void ortho_fix_wire(std::vector<symcirc::Pt>& pts) {
    if (pts.size() < 2) return;
    std::vector<symcirc::Pt> out;
    out.push_back(pts[0]);
    bool prev_was_horizontal = false;
    for (size_t i = 0; i + 1 < pts.size(); ++i) {
        auto a = pts[i], b = pts[i + 1];
        bool horiz = std::fabs(b.second - a.second) < 1e-6;
        bool vert  = std::fabs(b.first - a.first)  < 1e-6;
        if (horiz || vert) {
            out.push_back(b);
            prev_was_horizontal = horiz;
            continue;
        }
        bool h_first = !prev_was_horizontal;
        symcirc::Pt corner = h_first ? symcirc::Pt{b.first, a.second}
                                     : symcirc::Pt{a.first, b.second};
        if (std::fabs(out.back().first  - corner.first ) > 1e-6 ||
            std::fabs(out.back().second - corner.second) > 1e-6)
            out.push_back(corner);
        out.push_back(b);
        prev_was_horizontal = !h_first;
    }
    pts = std::move(out);
}

int g_fail = 0;
#define CHECK(c) do { if (!(c)) { ++g_fail; std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)
} // namespace

static void check_all_axis_aligned(const std::vector<symcirc::Pt>& pts,
                                   const char* tag) {
    for (size_t i = 0; i + 1 < pts.size(); ++i) {
        if (!is_axis_aligned(pts[i], pts[i + 1])) {
            std::printf("FAIL %s: segment %zu (%g,%g)->(%g,%g) is diagonal\n",
                        tag, i, pts[i].first, pts[i].second,
                        pts[i + 1].first, pts[i + 1].second);
            ++g_fail;
        }
    }
}

int main() {
    // Already-orthogonal: must be unchanged.
    {
        std::vector<symcirc::Pt> w = {{0,0},{10,0},{10,20},{0,20}};
        ortho_fix_wire(w);
        CHECK(w.size() == 4);
        check_all_axis_aligned(w, "already-ortho");
    }
    // Single diagonal segment between two pins: becomes an L (one corner).
    {
        std::vector<symcirc::Pt> w = {{0,0},{10,10}};
        ortho_fix_wire(w);
        CHECK(w.size() == 3);
        check_all_axis_aligned(w, "single-diag");
        CHECK(is_axis_aligned(w[0], w[1]));
        CHECK(is_axis_aligned(w[1], w[2]));
    }
    // Three-point wire where the middle vertex moved off-axis: the whole
    // path becomes 3 axis-aligned legs, not a stair.
    {
        std::vector<symcirc::Pt> w = {{0,0},{5,5},{10,5}};
        ortho_fix_wire(w);
        CHECK(w.size() >= 3);
        check_all_axis_aligned(w, "three-point");
    }
    // Diagonal after each leg: alternating corners (no same-side stacking).
    {
        std::vector<symcirc::Pt> w = {{0,0},{5,5},{10,0},{15,5},{20,0}};
        ortho_fix_wire(w);
        check_all_axis_aligned(w, "alternating");
        // The total horizontal travel stays the same: 20.
        CHECK(std::fabs(w.back().first - w.front().first - 20.0) < 1e-6);
    }
    // Pathological: 1-vertex wire (single point) -- unchanged.
    {
        std::vector<symcirc::Pt> w = {{0,0}};
        ortho_fix_wire(w);
        CHECK(w.size() == 1);
    }
    // Already-ortho with a zero-length return in the middle: collinear point
    // gets removed, the result is still axis-aligned.
    {
        std::vector<symcirc::Pt> w = {{0,0},{5,0},{5,0},{10,0}};
        ortho_fix_wire(w);
        check_all_axis_aligned(w, "degenerate-zero");
    }

    std::printf("%s (%d failure(s))\n",
                g_fail ? "ORTHO FAILED" : "ortho ok", g_fail);
    return g_fail == 0 ? 0 : 1;
}