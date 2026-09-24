// Regression: dragging a component re-routes connected wires to stay
// orthogonal (Cadence Virtuoso behaviour). Pure stdlib / Document stuff.
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

void ortho_fix_wire(std::vector<symcirc::Pt>& pts,
                    const std::vector<std::pair<symcirc::Pt, symcirc::Pt>>& other_segs = {}) {
    if (pts.size() < 2) return;
    auto seg_hits = [&](symcirc::Pt p) {
        for (const auto& s : other_segs) {
            if (std::hypot(s.first.first - p.first, s.first.second - p.second) < 1.0 ||
                std::hypot(s.second.first - p.first, s.second.second - p.second) < 1.0)
                continue;
            double vx = s.second.first - s.first.first,
                   vy = s.second.second - s.first.second;
            double wx = p.first - s.first.first, wy = p.second - s.first.second;
            double L2 = vx * vx + vy * vy;
            if (L2 < 1e-9) continue;
            double t = (wx * vx + wy * vy) / L2;
            if (t <= 0 || t >= 1) continue;
            double px = s.first.first + t * vx, py = s.first.second + t * vy;
            if (std::hypot(p.first - px, p.second - py) <= 1.0) return true;
        }
        return false;
    };
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
        symcirc::Pt c1{b.first, a.second};
        symcirc::Pt c2{a.first, b.second};
        bool h_first;
        if (other_segs.empty()) {
            h_first = !prev_was_horizontal;
        } else {
            bool c1_clean = !seg_hits(c1);
            bool c2_clean = !seg_hits(c2);
            if (c1_clean && !c2_clean) h_first = true;
            else if (c2_clean && !c1_clean) h_first = false;
            else h_first = !prev_was_horizontal;
        }
        symcirc::Pt corner = h_first ? c1 : c2;
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

static void dump(const std::vector<symcirc::Pt>& pts, const char* tag) {
    std::printf("    %s pts (%zu):", tag, pts.size());
    for (auto& p : pts)
        std::printf(" (%.0f,%.0f)", p.first, p.second);
    std::printf("\n");
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

    // ----- Real-scenario regression: a wire attached to a moved pin. -----
    // Original: [(160,290), (160,240), (400,240)] -- a V1 output that goes
    // up then right to M1's gate at (400,240). The user drags M1 down-right
    // so the gate is now at (610, 360). Pin (400,240) moves to (610,360);
    // the wire's third vertex becomes (610,360) and the last segment
    // (160,240)->(610,360) is diagonal. After ortho-fix we want a clean
    // three-segment L: up from V1, then right to M1's x, then down to M1's
    // y -- NOT a hook through a stray corner.
    {
        std::vector<symcirc::Pt> w = {{160,290},{160,240},{610,360}};
        dump(w, "input");
        ortho_fix_wire(w);
        dump(w, "output");
        check_all_axis_aligned(w, "dragged-pin");

        // Expected route:
        //   (160,290) -> (160,240)  (up from V1)
        //   (160,240) -> (610,240)  (right at y=240)
        //   (610,240) -> (610,360)  (down to M1's gate)
        // The middle vertex is at (610,240), NOT at (160, 360).
        CHECK(w.size() == 4);
        bool found_corner = false;
        for (auto& p : w)
            if (std::fabs(p.first - 160.0) < 1e-6 &&
                std::fabs(p.second - 360.0) < 1e-6) found_corner = true;
        CHECK(!found_corner); // must NOT route through (160, 360) -- that's a hook
    }

    // ----- Multi-step drag: two successive moves, both reorthogonalise. -----
    {
        // After the previous step the wire was [(160,290), (160,240),
        // (610,240), (610,360)]. Now drag again: M1 gate moves to
        // (760, 480). The 4th vertex shifts to (760, 480) and the segment
        // (610,360)->(760,480) is diagonal. Reorthogonalise.
        std::vector<symcirc::Pt> w = {{160,290},{160,240},{610,240},{760,480}};
        dump(w, "input2");
        ortho_fix_wire(w);
        dump(w, "output2");
        check_all_axis_aligned(w, "dragged-pin-2");
        // The diagonal segment (610,240)->(760,480) becomes L-L: 4 legs.
        CHECK(w.size() == 5);
    }

    // ----- Wire-aware: avoid landing a corner on another wire. -----
    // A horizontal wire at y=240 passes through (760, 240). The default
    // corner for a diagonal (160,240)->(760,480) is (760,240), which lies
    // ON the horizontal wire -> would create a stray T-junction. Pass the
    // other wire's segments and the helper should pick the other corner
    // (160, 480) instead.
    {
        std::vector<symcirc::Pt> w = {{160,240},{760,480}};
        std::vector<std::pair<symcirc::Pt, symcirc::Pt>> others = {
            {{600,240},{900,240}} // horizontal at y=240
        };
        ortho_fix_wire(w, others);
        dump(w, "wire-aware");
        check_all_axis_aligned(w, "wire-aware");
        // The corner should be at (160, 480), NOT at (760, 240).
        bool bad_corner = false;
        for (auto& p : w)
            if (std::fabs(p.first - 760.0) < 1e-6 &&
                std::fabs(p.second - 240.0) < 1e-6) bad_corner = true;
        CHECK(!bad_corner);
    }

    std::printf("%s (%d failure(s))\n",
                g_fail ? "ORTHO FAILED" : "ortho ok", g_fail);
    return g_fail == 0 ? 0 : 1;
}