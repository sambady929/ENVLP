#include "Document.h"
#include "SymbolGeom.h"
#include "core/Eng.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <unordered_map>

namespace envlp {

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
namespace {

constexpr double kJoinTol = 9.0; // px: pin/wire endpoints within this touch

// Defined below (net resolution); used by remove_wire above.
bool point_on_seg(Pt p, Pt a, Pt b, double tol);

// Union-find over quantized points.
struct UnionFind {
    std::vector<int> p;
    explicit UnionFind(int n) : p(n) {
        for (int i = 0; i < n; ++i) p[i] = i;
    }
    int find(int a) {
        while (p[a] != a) a = p[a] = p[p[a]];
        return a;
    }
    void join(int a, int b) {
        a = find(a);
        b = find(b);
        if (a != b) p[a] = b;
    }
};

std::string quote(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\';
        o += c;
    }
    o += '"';
    return o;
}

// Reads the next whitespace-delimited token; supports "quoted" strings.
bool next_token(const std::string& s, size_t& i, std::string& out) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' ||
                            s[i] == '\n'))
        ++i;
    if (i >= s.size()) return false;
    out.clear();
    if (s[i] == '"') {
        ++i;
        while (i < s.size() && s[i] != '"') {
            if (s[i] == '\\' && i + 1 < s.size()) ++i;
            out += s[i++];
        }
        if (i < s.size() && s[i] == '"') ++i;
        return true;
    }
    while (i < s.size() && s[i] != ' ' && s[i] != '\t' && s[i] != '\r' &&
           s[i] != '\n')
        out += s[i++];
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// add / remove
// ---------------------------------------------------------------------------
std::string Document::add(const syms::Component& c, double x, double y) {
    syms::Component nc = c;
    nc.ref = syms::next_ref(circuit, c.kind);
    nc.nodes.assign(syms::pin_count(c.kind), "");
    circuit.comps.push_back(nc);

    Placement pl;
    // snap to10 px grid
    pl.x = std::round(x / 10.0) * 10.0;
    pl.y = std::round(y / 10.0) * 10.0;
    pl.rot = 0;
    placements[nc.ref] = pl;
    dirty = true;
    return nc.ref;
}

void Document::remove(const std::string& ref) {
    auto it = std::find_if(circuit.comps.begin(), circuit.comps.end(),
                           [&](const syms::Component& c) { return c.ref == ref; });
    if (it == circuit.comps.end()) return;
    // Detach every wire end bound to this component's pins: a dangling Pin
    // binding would silently re-attach if the reference is ever reused
    // (next_ref hands a removed ref straight back), which can bridge two
    // unrelated nets -- e.g. a wire that used to feed a deleted source
    // snapping onto a freshly placed symbol's pin. Freeze each such end at
    // its current coordinate so the wire stays exactly where it was drawn.
    for (auto& w : wires) {
        if (w.pts.size() < 2) continue;
        if (w.a.kind == WireEnd::Kind::Pin && w.a.ref == ref) {
            Pt p = wire_end_pt(w, true);
            w.a.kind = WireEnd::Kind::Free;
            w.a.ref.clear();
            w.a.pin = -1;
            w.pts.front() = p;
        }
        if (w.b.kind == WireEnd::Kind::Pin && w.b.ref == ref) {
            Pt p = wire_end_pt(w, false);
            w.b.kind = WireEnd::Kind::Free;
            w.b.ref.clear();
            w.b.pin = -1;
            w.pts.back() = p;
        }
        // An interior pin of a merged wire becomes an ordinary free waypoint
        // at its last coordinate.
        for (auto& pp : w.pin_points)
            if (pp.end.kind == WireEnd::Kind::Pin && pp.end.ref == ref)
                pp.end = WireEnd{};
        w.pin_points.erase(
            std::remove_if(w.pin_points.begin(), w.pin_points.end(),
                           [](const Wire::PinPoint& pp) {
                               return pp.end.kind != WireEnd::Kind::Pin;
                           }),
            w.pin_points.end());
    }
    circuit.comps.erase(it);
    placements.erase(ref);
    dirty = true;
}

// Remove a wire; drop any label that was attached only to that wire. A label
// shared with another wire of the same net (or anchored to a pin) survives.
void Document::remove_wire(int wire_index) {
    if (wire_index < 0 || wire_index >= int(wires.size())) return;
    wires.erase(wires.begin() + wire_index);
    // Fix label bindings: drop those bound to the removed wire, and shift the
    // rest down by one where their wire index was higher.
    for (auto& l : labels) {
        if (l.bind_wire == wire_index) l.bind_wire = -1;
        else if (l.bind_wire > wire_index) --l.bind_wire;
    }
    prune_orphan_labels();
    dirty = true;
}

// Two wires that meet end to end are one wire, not two that happen to touch:
// the polylines are stitched together (the shared point appears once) and
// the outer ends keep their bindings. Without this, drawing a wire from the
// end of another leaves a junction and the two move apart.
bool Document::join_wires(int i, int j) {
    if (i == j || i < 0 || j < 0 || i >= int(wires.size()) ||
        j >= int(wires.size()))
        return false;
    Wire& A = wires[i];
    Wire& B = wires[j];
    if (A.pts.size() < 2 || B.pts.size() < 2) return false;
    // The same tolerance the net resolver uses: if two endpoints are close
    // enough to be one net, they are close enough to be one physical wire.
    auto same = [](const Pt& a, const Pt& b) {
        return std::hypot(a.first - b.first, a.second - b.second) <= kJoinTol;
    };

    // Which end of A meets which end of B, and in which direction the
    // stitched polyline runs.
    bool revA = false, revB = false, ok = false;
    bool a_pin_end = false, b_pin_end = false;
    bool a_front = false, b_front = false;
    auto pin_end = [](const WireEnd& e) { return e.kind == WireEnd::Kind::Pin; };
    if (same(A.pts.back(), B.pts.front())) {
        ok = true;
        a_front = false; b_front = true;
        a_pin_end = pin_end(A.b);
        b_pin_end = pin_end(B.a);
    } else if (same(A.pts.back(), B.pts.back())) {
        ok = true;
        revB = true;
        a_front = false; b_front = false;
        a_pin_end = pin_end(A.b);
        b_pin_end = pin_end(B.b);
    } else if (same(A.pts.front(), B.pts.front())) {
        ok = true;
        revA = true;
        a_front = true; b_front = true;
        a_pin_end = pin_end(A.a);
        b_pin_end = pin_end(B.a);
    } else if (same(A.pts.front(), B.pts.back())) {
        ok = true;
        revA = true;
        revB = true;
        a_front = true; b_front = false;
        a_pin_end = pin_end(A.a);
        b_pin_end = pin_end(B.b);
    }
    if (!ok) return false;

    std::vector<Pt> pa = A.pts;
    const std::vector<Pt> pb = B.pts;
    const std::vector<Pt> wa = A.waypoints, wb = B.waypoints;
    const auto ppa = A.pin_points, ppb = B.pin_points;
    const WireEnd ea = A.a, eb = A.b, fa = B.a, fb = B.b;
    // Close a sub-tolerance gap: the shared point is the pin when one side is
    // pinned (the pin is the exact terminal), otherwise A's endpoint. Without
    // this, two endpoints can be one net but visibly apart.
    auto pin_position = [&](const WireEnd& e, bool& found) {
        found = false;
        if (e.kind != WireEnd::Kind::Pin) return Pt{0, 0};
        const syms::Component* c = circuit.find(e.ref);
        auto pl = placements.find(e.ref);
        if (!c || pl == placements.end()) return Pt{0, 0};
        found = true;
        return pin_world(*c, pl->second, e.pin);
    };
    Pt common = a_front ? pa.front() : pa.back();
    WireEnd shared_pin;
    if (a_pin_end) {
        bool found = false;
        Pt p = pin_position(a_front ? A.a : A.b, found);
        common = found ? p : (a_front ? pa.front() : pa.back());
        shared_pin = a_front ? A.a : A.b;
    } else if (b_pin_end) {
        bool found = false;
        Pt p = pin_position(b_front ? B.a : B.b, found);
        common = found ? p : (b_front ? pb.front() : pb.back());
        shared_pin = b_front ? B.a : B.b;
    }
    if (a_front) pa.front() = common;
    else pa.back() = common;

    Wire M;
    M.pts.clear();
    const size_t na = pa.size(), nb = pb.size();
    if (revA) {
        for (size_t k = na; k-- > 0;) M.pts.push_back(pa[k]);
    } else {
        for (size_t k = 0; k < na; ++k) M.pts.push_back(pa[k]);
    }
    // The shared point ends A's run, so B's run starts one past it.
    if (revB) {
        for (size_t k = nb - 1; k > 0; --k) M.pts.push_back(pb[k - 1]);
    } else {
        for (size_t k = 1; k < nb; ++k) M.pts.push_back(pb[k]);
    }
    // The outer ends: the end of A that is not the meeting point, and the end
    // of B that is not the meeting point.
    if (!revA) {
        M.a = ea;
        M.b = revB ? fa : fb;
    } else {
        M.a = eb;
        M.b = revB ? fa : fb;
    }
    // A sub-tolerance snap can leave a zero-length segment.
    std::vector<Pt> cleaned;
    for (const auto& p : M.pts) {
        if (cleaned.empty() ||
            std::hypot(p.first - cleaned.back().first,
                       p.second - cleaned.back().second) > 1e-9)
            cleaned.push_back(p);
    }
    M.pts = std::move(cleaned);
    if (M.pts.size() > 2)
        M.waypoints.assign(M.pts.begin() + 1, M.pts.end() - 1);
    else
        M.waypoints.clear();

    // When the meeting point is a pin, keep it bound as an interior waypoint
    // so a later component move carries the middle of the merged wire too.
    auto waypoint_index_of = [&](const Pt& q) {
        for (size_t k = 0; k < M.waypoints.size(); ++k)
            if (std::hypot(M.waypoints[k].first - q.first,
                           M.waypoints[k].second - q.second) < 1e-6)
                return int(k);
        return -1;
    };
    auto add_pin_point = [&](const Pt& q, const WireEnd& e) {
        if (e.kind != WireEnd::Kind::Pin) return;
        int k = waypoint_index_of(q);
        if (k < 0) return;
        for (const auto& old : M.pin_points)
            if (old.waypoint == k && old.end.ref == e.ref && old.end.pin == e.pin)
                return;
        Wire::PinPoint pp;
        pp.waypoint = k;
        pp.end = e;
        M.pin_points.push_back(pp);
    };
    if (a_pin_end || b_pin_end) add_pin_point(common, shared_pin);
    // Carry over interior pin bindings from both halves, re-indexed against
    // the merged waypoint list by coordinate.
    auto carry_pins = [&](const std::vector<Pt>& old_waypoints,
                          const std::vector<Wire::PinPoint>& old_pins) {
        for (const auto& pp : old_pins) {
            if (pp.waypoint < 0 || pp.waypoint >= int(old_waypoints.size()))
                continue;
            add_pin_point(old_waypoints[pp.waypoint], pp.end);
        }
    };
    carry_pins(wa, ppa);
    carry_pins(wb, ppb);

    finish_join(i, j, std::move(M));
    return true;
}

int Document::finish_join(int i, int j, Wire M) {
    wires[i] = std::move(M);
    wires.erase(wires.begin() + j);
    // The merged wire's index after the erase. Labels that were bound to
    // either half follow the merged wire, and their fraction is re-resolved
    // against the new polyline so they stay on the same point.
    const int merged = (j < i) ? i - 1 : i;
    for (auto& l : labels) {
        const bool on_merged = (l.bind_wire == i || l.bind_wire == j);
        if (on_merged) {
            l.bind_wire = merged;
            l.bind_frac = wire_frac_of(merged, l.anchor);
        } else if (l.bind_wire > j) {
            --l.bind_wire;
        }
    }
    dirty = true;
    return merged;
}

// Merge two collinear straight segments whose intervals overlap or touch.
// The endpoint-only join below cannot see a partial overlap (one wire starts
// or ends inside the other); this is the 1D union that makes such a pair one
// physical segment. Non-collinear meetings are left to join_wires (a corner
// or a T-junction).
bool Document::merge_collinear(int i, int j) {
    if (i == j || i < 0 || j < 0 || i >= int(wires.size()) ||
        j >= int(wires.size()))
        return false;
    const Wire& A = wires[i];
    const Wire& B = wires[j];
    if (A.pts.size() < 2 || B.pts.size() < 2) return false;
    // A straight wire may carry interior pin vertices (three or more collinear
    // points); it is still a straight interval for the union.
    auto horiz = [](const Wire& w) {
        double y = w.pts[0].second;
        double lo = w.pts[0].first, hi = lo;
        for (const auto& p : w.pts) {
            if (std::fabs(p.second - y) > 1.0) return false;
            lo = std::min(lo, p.first);
            hi = std::max(hi, p.first);
        }
        return hi - lo > 1e-9;
    };
    auto vert = [](const Wire& w) {
        double x = w.pts[0].first;
        double lo = w.pts[0].second, hi = lo;
        for (const auto& p : w.pts) {
            if (std::fabs(p.first - x) > 1.0) return false;
            lo = std::min(lo, p.second);
            hi = std::max(hi, p.second);
        }
        return hi - lo > 1e-9;
    };
    const bool ah = horiz(A), av = vert(A);
    const bool bh = horiz(B), bv = vert(B);
    if (!((ah && bh) || (av && bv))) return false;

    const bool horizontal = ah && bh;
    double axis; // the common y (horizontal) or x (vertical)
    auto has_pin = [](const Wire& w) {
        return w.a.kind == WireEnd::Kind::Pin || w.b.kind == WireEnd::Kind::Pin ||
               !w.pin_points.empty();
    };
    if (horizontal) {
        // A pin's exact coordinate wins over the other wire's stored line.
        auto pin_y = [&](const Wire& w) -> double {
            for (const WireEnd& e : {w.a, w.b}) {
                if (e.kind != WireEnd::Kind::Pin) continue;
                const syms::Component* c = circuit.find(e.ref);
                auto pl = placements.find(e.ref);
                if (c && pl != placements.end())
                    return pin_world(*c, pl->second, e.pin).second;
            }
            for (const auto& pp : w.pin_points) {
                if (pp.end.kind != WireEnd::Kind::Pin) continue;
                const syms::Component* c = circuit.find(pp.end.ref);
                auto pl = placements.find(pp.end.ref);
                if (c && pl != placements.end())
                    return pin_world(*c, pl->second, pp.end.pin).second;
            }
            return w.pts[0].second;
        };
        double ay = pin_y(A), by = pin_y(B);
        if (std::fabs(ay - by) > 1.0) return false;
        axis = has_pin(A) ? ay : (has_pin(B) ? by : A.pts[0].second);
    } else {
        auto pin_x = [&](const Wire& w) -> double {
            for (const WireEnd& e : {w.a, w.b}) {
                if (e.kind != WireEnd::Kind::Pin) continue;
                const syms::Component* c = circuit.find(e.ref);
                auto pl = placements.find(e.ref);
                if (c && pl != placements.end())
                    return pin_world(*c, pl->second, e.pin).first;
            }
            for (const auto& pp : w.pin_points) {
                if (pp.end.kind != WireEnd::Kind::Pin) continue;
                const syms::Component* c = circuit.find(pp.end.ref);
                auto pl = placements.find(pp.end.ref);
                if (c && pl != placements.end())
                    return pin_world(*c, pl->second, pp.end.pin).first;
            }
            return w.pts[0].first;
        };
        double ax = pin_x(A), bx = pin_x(B);
        if (std::fabs(ax - bx) > 1.0) return false;
        axis = has_pin(A) ? ax : (has_pin(B) ? bx : A.pts[0].first);
    }
    // Collinearity: the other coordinate of every point must be on `axis`.
    auto on_axis = [&](const Pt& p) {
        return std::fabs((horizontal ? p.second : p.first) - axis) <= 1.0;
    };
    for (const auto& p : A.pts)
        if (!on_axis(p)) return false;
    for (const auto& p : B.pts)
        if (!on_axis(p)) return false;

    auto proj = [&](const Pt& p) { return horizontal ? p.first : p.second; };
    auto interval = [&](const Wire& w) {
        double lo = proj(w.pts[0]), hi = lo;
        for (const auto& p : w.pts) {
            lo = std::min(lo, proj(p));
            hi = std::max(hi, proj(p));
        }
        return std::pair<double, double>{lo, hi};
    };
    auto ai = interval(A), bi = interval(B);
    double a0 = ai.first, a1 = ai.second;
    double b0 = bi.first, b1 = bi.second;
    // They must overlap or touch, not merely lie on the same infinite line.
    if (std::max(a0, b0) > std::min(a1, b1) + 1.0) return false;
    const double u0 = std::min(a0, b0);
    const double u1 = std::max(a1, b1);
    if (u1 - u0 < 1e-9) return false;

    auto pt_at = [&](double t) {
        return horizontal ? Pt{t, axis} : Pt{axis, t};
    };
    Wire M;
    // The extreme endpoint bindings. A pin binding wins over a free one at the
    // same end. An endpoint in the middle of the union becomes an interior pin
    // waypoint, so a partial overlap through a pin still follows the part.
    auto consider_end = [&](double t, const WireEnd& e, double target,
                            double& best, WireEnd& out) {
        double d = std::fabs(t - target);
        if (d > 1.0) return;
        if (d < best - 1e-9 ||
            (std::fabs(d - best) <= 1e-9 && e.kind == WireEnd::Kind::Pin &&
             out.kind != WireEnd::Kind::Pin)) {
            best = d;
            out = e;
        }
    };
    double best0 = 1e9, best1 = 1e9;
    WireEnd e0, e1;
    consider_end(proj(A.pts[0]), A.a, u0, best0, e0);
    consider_end(proj(A.pts[1]), A.b, u0, best0, e0);
    consider_end(proj(B.pts[0]), B.a, u0, best0, e0);
    consider_end(proj(B.pts[1]), B.b, u0, best0, e0);
    consider_end(proj(A.pts[0]), A.a, u1, best1, e1);
    consider_end(proj(A.pts[1]), A.b, u1, best1, e1);
    consider_end(proj(B.pts[0]), B.a, u1, best1, e1);
    consider_end(proj(B.pts[1]), B.b, u1, best1, e1);
    auto consider_interior_pins = [&](const Wire& w) {
        for (const auto& pp : w.pin_points) {
            if (pp.waypoint < 0 || pp.waypoint >= int(w.waypoints.size()))
                continue;
            double t = proj(w.waypoints[pp.waypoint]);
            consider_end(t, pp.end, u0, best0, e0);
            consider_end(t, pp.end, u1, best1, e1);
        }
    };
    consider_interior_pins(A);
    consider_interior_pins(B);
    M.a = e0;
    M.b = e1;

    std::vector<std::pair<double, WireEnd>> pins;
    auto add_pin = [&](double t, const WireEnd& e) {
        if (e.kind != WireEnd::Kind::Pin) return;
        if (t <= u0 + 1.0 || t >= u1 - 1.0) return;
        for (const auto& q : pins)
            if (std::fabs(q.first - t) < 1e-6 && q.second.ref == e.ref &&
                q.second.pin == e.pin)
                return;
        pins.push_back({t, e});
    };
    add_pin(proj(A.pts[0]), A.a);
    add_pin(proj(A.pts[1]), A.b);
    add_pin(proj(B.pts[0]), B.a);
    add_pin(proj(B.pts[1]), B.b);
    auto carry_pins = [&](const Wire& w) {
        for (const auto& pp : w.pin_points) {
            if (pp.waypoint < 0 || pp.waypoint >= int(w.waypoints.size()))
                continue;
            add_pin(proj(w.waypoints[pp.waypoint]), pp.end);
        }
    };
    carry_pins(A);
    carry_pins(B);
    std::sort(pins.begin(), pins.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    M.pts.push_back(pt_at(u0));
    for (const auto& q : pins) M.pts.push_back(pt_at(q.first));
    M.pts.push_back(pt_at(u1));
    // Drop duplicate points that a pin at exactly u0/u1 would create.
    std::vector<Pt> cleaned;
    for (const auto& p : M.pts) {
        if (cleaned.empty() ||
            std::hypot(p.first - cleaned.back().first,
                       p.second - cleaned.back().second) > 1e-9)
            cleaned.push_back(p);
    }
    M.pts = std::move(cleaned);
    if (M.pts.size() > 2)
        M.waypoints.assign(M.pts.begin() + 1, M.pts.end() - 1);
    else
        M.waypoints.clear();
    for (const auto& q : pins) {
        for (size_t k = 0; k < M.waypoints.size(); ++k) {
            if (std::fabs(proj(M.waypoints[k]) - q.first) < 1e-6) {
                Wire::PinPoint pp;
                pp.waypoint = int(k);
                pp.end = q.second;
                M.pin_points.push_back(pp);
                break;
            }
        }
    }
    finish_join(i, j, std::move(M));
    return true;
}

namespace {
// A straight wire as a 1D interval on one axis.
struct WireLine {
    bool ok = false;
    bool horizontal = false;
    double axis = 0;
    double lo = 0, hi = 0;
};

WireLine wire_line(const Wire& w) {
    if (w.pts.size() < 2) return {};
    double y = w.pts[0].second, x = w.pts[0].first;
    bool h = true, v = true;
    double xmin = x, xmax = x, ymin = y, ymax = y;
    for (const auto& p : w.pts) {
        if (std::fabs(p.second - y) > 1.0) h = false;
        if (std::fabs(p.first - x) > 1.0) v = false;
        xmin = std::min(xmin, p.first);
        xmax = std::max(xmax, p.first);
        ymin = std::min(ymin, p.second);
        ymax = std::max(ymax, p.second);
    }
    WireLine r;
    if (h && xmax - xmin > 1e-9) {
        r.ok = true;
        r.horizontal = true;
        r.axis = y;
        r.lo = xmin;
        r.hi = xmax;
    } else if (v && ymax - ymin > 1e-9) {
        r.ok = true;
        r.horizontal = false;
        r.axis = x;
        r.lo = ymin;
        r.hi = ymax;
    }
    return r;
}

double line_proj(const WireLine& l, const Pt& p) {
    return l.horizontal ? p.first : p.second;
}
bool line_on(const WireLine& l, const Pt& p) {
    return std::fabs((l.horizontal ? p.second : p.first) - l.axis) <= 1.0;
}
Pt line_pt(const WireLine& l, double t) {
    return l.horizontal ? Pt{t, l.axis} : Pt{l.axis, t};
}
} // namespace

// A 90-degree wire whose first or last segment lies on a straight wire:
// absorb the overlapping collinear run into the straight wire and trim that
// run off the corner wire. If the corner is the straight wire's endpoint the
// caller's endpoint join then makes one polyline; if the corner is in the
// middle, the trimmed wire stays a branch (a T-junction) and the overlap is
// gone from the drawing.
bool Document::trim_corner_overlap(int i, int j) {
    if (i == j || i < 0 || j < 0 || i >= int(wires.size()) ||
        j >= int(wires.size()))
        return false;
    WireLine li = wire_line(wires[i]);
    WireLine lj = wire_line(wires[j]);
    if (li.ok == lj.ok) return false; // both straight or both bent
    const int ai = li.ok ? i : j;
    const int bi = li.ok ? j : i;
    WireLine la = li.ok ? li : lj;
    Wire& A = wires[ai];
    Wire& B = wires[bi];
    if (B.pts.size() < 3) return false; // a straight B is merge_collinear's job

    auto recompute_labels = [&]() {
        for (auto& l : labels) {
            if (l.bind_wire == ai || l.bind_wire == bi)
                l.bind_frac = wire_frac_of(l.bind_wire, l.anchor);
        }
    };

    auto extend_A = [&](double u0, double u1) -> bool {
        if (A.pts.size() < 2) return false;
        const int lo_i =
            line_proj(la, A.pts.front()) <= line_proj(la, A.pts.back())
                ? 0
                : int(A.pts.size()) - 1;
        const int hi_i = (lo_i == 0) ? int(A.pts.size()) - 1 : 0;
        WireEnd& lo_e = (lo_i == 0) ? A.a : A.b;
        WireEnd& hi_e = (hi_i == 0) ? A.a : A.b;
        if (u0 < la.lo - 1e-6) {
            // Extending a pin end would move the terminal and lose the
            // binding; leave that pair separate rather than break the part.
            if (lo_e.kind == WireEnd::Kind::Pin) return false;
            A.pts[lo_i] = line_pt(la, u0);
            lo_e = WireEnd{};
            la.lo = u0;
        }
        if (u1 > la.hi + 1e-6) {
            if (hi_e.kind == WireEnd::Kind::Pin) return false;
            A.pts[hi_i] = line_pt(la, u1);
            hi_e = WireEnd{};
            la.hi = u1;
        }
        return true;
    };

    auto trim_front = [&]() -> bool {
        int c = 0;
        while (c + 1 < int(B.pts.size()) && line_on(la, B.pts[c + 1])) ++c;
        if (c < 1) return false;                    // no collinear segment
        if (c + 1 >= int(B.pts.size())) return false; // B is straight
        double p0 = line_proj(la, B.pts[0]);
        double p1 = line_proj(la, B.pts[c]);
        double lo = std::min(p0, p1), hi = std::max(p0, p1);
        if (std::max(lo, la.lo) > std::min(hi, la.hi) + 1.0) return false;
        if (!extend_A(std::min(la.lo, lo), std::max(la.hi, hi))) return false;
        B.pts.erase(B.pts.begin(), B.pts.begin() + c);
        B.a = WireEnd{};
        if (B.pts.size() > 2)
            B.waypoints.assign(B.pts.begin() + 1, B.pts.end() - 1);
        else
            B.waypoints.clear();
        B.pin_points.clear();
        recompute_labels();
        dirty = true;
        return true;
    };

    auto trim_back = [&]() -> bool {
        int c = int(B.pts.size()) - 1;
        while (c - 1 >= 0 && line_on(la, B.pts[c - 1])) --c;
        if (c > int(B.pts.size()) - 2) return false; // no collinear segment
        if (c <= 0) return false;                    // B is straight
        double p0 = line_proj(la, B.pts[c]);
        double p1 = line_proj(la, B.pts.back());
        double lo = std::min(p0, p1), hi = std::max(p0, p1);
        if (std::max(lo, la.lo) > std::min(hi, la.hi) + 1.0) return false;
        if (!extend_A(std::min(la.lo, lo), std::max(la.hi, hi))) return false;
        B.pts.erase(B.pts.begin() + c + 1, B.pts.end());
        B.b = WireEnd{};
        if (B.pts.size() > 2)
            B.waypoints.assign(B.pts.begin() + 1, B.pts.end() - 1);
        else
            B.waypoints.clear();
        B.pin_points.clear();
        recompute_labels();
        dirty = true;
        return true;
    };

    bool changed = false;
    if (trim_front()) changed = true;
    if (trim_back()) changed = true;
    if (changed) {
        // The new corner/extension can land on a pin, so bind the endpoints
        // before the caller's endpoint join.
        bind_wire_ends(A);
        bind_wire_ends(B);
    }
    return changed;
}

int Document::join_with_neighbours(int wire_index) {
    int wi = wire_index;
    const int max_joins = int(wires.size());
    for (int guard = 0; guard < max_joins; ++guard) {
        bool joined = false;
        for (size_t k = 0; k < wires.size(); ++k) {
            if (int(k) == wi) continue;
            // A 90-degree wire whose end segment lies on a straight wire has
            // its overlapping run absorbed first; then a partial overlap is a
            // 1D union, and a corner or non-collinear T is an endpoint join.
            trim_corner_overlap(wi, int(k));
            if (!merge_collinear(wi, int(k)) && !join_wires(wi, int(k)))
                continue;
            // The merged wire took the place of the other one, so its index
            // may have moved.
            if (int(k) < wi) --wi;
            joined = true;
            break;
        }
        if (!joined) break;
    }
    return wi;
}

// Remove every net label whose anchor no longer lies on a wire or a pin (e.g.
// after the wire it named was deleted). Called after any wire removal.
void Document::prune_orphan_labels() {
    for (int li = int(labels.size()) - 1; li >= 0; --li) {
        Pt a = labels[li].anchor;
        bool on_pin = false;
        for (const auto& c : circuit.comps) {
            auto pl = placements.find(c.ref);
            if (pl == placements.end()) continue;
            int np = int(pin_offsets(c.kind).size());
            for (int pi = 0; pi < np; ++pi) {
                Pt pw = pin_world(c, pl->second, pi);
                if (std::hypot(pw.first - a.first, pw.second - a.second) <=
                    kJoinTol) {
                    on_pin = true;
                    break;
                }
            }
            if (on_pin) break;
        }
        bool on_wire = false;
        for (const auto& w : wires)
            for (size_t k = 1; k < w.pts.size(); ++k)
                if (point_on_seg(a, w.pts[k - 1], w.pts[k], kJoinTol)) {
                    on_wire = true;
                    break;
                }
        if (!on_pin && !on_wire) labels.erase(labels.begin() + li);
    }
}

// ---------------------------------------------------------------------------
// net resolution
// ---------------------------------------------------------------------------
namespace {
// Is point q on segment ab (within tolerance)?
bool point_on_seg(Pt p, Pt a, Pt b, double tol) {
    double vx = b.first - a.first, vy = b.second - a.second;
    double wx = p.first - a.first, wy = p.second - a.second;
    double L2 = vx * vx + vy * vy;
    if (L2 < 1e-12) return std::hypot(wx, wy) <= tol;
    double t = (wx * vx + wy * vy) / L2;
    if (t < 0.0 || t > 1.0) return false;
    double px = a.first + t * vx, py = a.second + t * vy;
    return std::hypot(p.first - px, p.second - py) <= tol;
}
bool is_gnd_name(std::string s) {
    for (auto& ch : s) ch = char(std::tolower(ch));
    return s == "0" || s == "gnd" || s == "ground";
}
bool is_vdd_name(std::string s) {
    for (auto& ch : s) ch = char(std::tolower(ch));
    return s == "vdd" || s == "vcc" || s == "v+";
}
} // namespace

int NetMap::root_of_pin(int comp, int pin) const {
    for (size_t i = 0; i < pin_comp.size(); ++i)
        if (pin_comp[i] == comp && pin_index[i] == pin) return pin_root[i];
    return -1;
}

NetMap Document::net_map() const {
    NetMap nm;

    auto pin_pt = [&](int ci, int pi) {
        const auto& c = circuit.comps[ci];
        auto pl = placements.find(c.ref);
        double ox = pl == placements.end() ? 0.0 : pl->second.x;
        double oy = pl == placements.end() ? 0.0 : pl->second.y;
        int rot = pl == placements.end() ? 0 : pl->second.rot;
        bool fh = pl != placements.end() && pl->second.flip_h;
        bool fv = pl != placements.end() && pl->second.flip_v;
        Pt p = pin_offsets(c.kind)[pi];
        if (fh) p.first = -p.first;
        if (fv) p.second = -p.second;
        Pt rp = rotate_pt(p, rot);
        return Pt{ox + rp.first, oy + rp.second};
    };

    // Record every pin.
    for (size_t ci = 0; ci < circuit.comps.size(); ++ci) {
        int np = int(pin_offsets(circuit.comps[ci].kind).size());
        for (int pi = 0; pi < np; ++pi) {
            nm.pin_comp.push_back(int(ci));
            nm.pin_index.push_back(pi);
        }
    }

    // Flat point list: [0 .. nPins) pins, then wire pts, then label anchors.
    // A wire's first/last point is *resolved from its endpoint binding* --
    // a Pin-bound end follows the pin's current world position, so the
    // connectivity graph stays correct even before sync_wire_endpoints()
    // has physically moved the stored coordinate.
    std::vector<Pt> pts;
    for (size_t i = 0; i < nm.pin_comp.size(); ++i)
        pts.push_back(pin_pt(nm.pin_comp[i], nm.pin_index[i]));
    size_t wire_base = pts.size();
    for (const auto& w : wires) {
        for (size_t k = 0; k < w.pts.size(); ++k) {
            if (k == 0) pts.push_back(wire_end_pt(w, true));
            else if (k + 1 == w.pts.size()) pts.push_back(wire_end_pt(w, false));
            else pts.push_back(w.pts[k]);
        }
    }
    size_t label_base = pts.size();
    for (const auto& l : labels) pts.push_back(l.anchor);

    int n = int(pts.size());
    UnionFind uf(n);

    // Points of one wire are connected through the wire.
    {
        size_t idx = wire_base;
        for (const auto& w : wires) {
            for (size_t k = 1; k < w.pts.size(); ++k)
                uf.join(int(idx), int(idx + k));
            idx += w.pts.size();
        }
    }

    // Coincident points join. Use a spatial hash keyed at the join tolerance so
    // this is near-linear instead of O(n^2) (which dominated large schematics:
    // ~20 ms per net_map at 800 components, recomputed after every edit).
    auto cell_key = [](long long cx, long long cy) -> long long {
        return (cx << 32) ^ (cy & 0xffffffffLL);
    };
    auto key_of = [](double v) -> long long {
        return (long long)std::floor(v / kJoinTol);
    };
    {
        std::unordered_map<long long, std::vector<int>> buckets;
        buckets.reserve(n * 2);
        for (int i = 0; i < n; ++i)
            buckets[cell_key(key_of(pts[i].first), key_of(pts[i].second))]
                .push_back(i);
        for (int i = 0; i < n; ++i) {
            long long cx = key_of(pts[i].first), cy = key_of(pts[i].second);
            for (long long dx = -1; dx <= 1; ++dx)
                for (long long dy = -1; dy <= 1; ++dy) {
                    auto it = buckets.find(cell_key(cx + dx, cy + dy));
                    if (it == buckets.end()) continue;
                    for (int j : it->second) {
                        if (j <= i) continue;
                        double ddx = pts[i].first - pts[j].first;
                        double ddy = pts[i].second - pts[j].second;
                        if (ddx * ddx + ddy * ddy <= kJoinTol * kJoinTol)
                            uf.join(i, j);
                    }
                }
        }
    }

    // A point lying on a wire segment joins that wire. This covers pins,
    // label anchors, *and* other wires' vertices -- so a wire drawn
    // perpendicular into the middle of another (a T-junction) merges the two
    // nets, which is what makes the connection real in SPICE terms. Segments
    // are bucketed by their bounding cells (all axis-aligned) so the test is
    // near-linear rather than points x segments.
    {
        std::vector<std::pair<size_t, size_t>> spans;
        size_t idx = wire_base;
        for (const auto& w : wires) {
            spans.push_back({idx, w.pts.size()});
            idx += w.pts.size();
        }
        struct SegRef { int a, b; };  // global point indices
        std::unordered_map<long long, std::vector<SegRef>> segb;
        for (const auto& sp : spans)
            for (size_t k = 1; k < sp.second; ++k) {
                int a = int(sp.first + k - 1), b = int(sp.first + k);
                long long x0 = key_of(std::min(pts[a].first, pts[b].first));
                long long x1 = key_of(std::max(pts[a].first, pts[b].first));
                long long y0 = key_of(std::min(pts[a].second, pts[b].second));
                long long y1 = key_of(std::max(pts[a].second, pts[b].second));
                for (long long cx = x0; cx <= x1; ++cx)
                    for (long long cy = y0; cy <= y1; ++cy)
                        segb[cell_key(cx, cy)].push_back({a, b});
            }
        auto attach_point = [&](int pi) {
            auto it = segb.find(cell_key(key_of(pts[pi].first),
                                         key_of(pts[pi].second)));
            if (it == segb.end()) return;
            for (const auto& s : it->second)
                if (point_on_seg(pts[pi], pts[s.a], pts[s.b], kJoinTol)) {
                    uf.join(pi, s.a);
                    uf.join(pi, s.b);
                }
        };
        for (size_t i = 0; i < nm.pin_comp.size(); ++i) attach_point(int(i));
        for (size_t k = 0; k < labels.size(); ++k)
            attach_point(int(label_base + k));
        for (size_t wi = 0; wi < wires.size(); ++wi) {
            size_t base = wire_base;
            for (size_t j = 0; j < wi; ++j) base += wires[j].pts.size();
            for (size_t v = 0; v < wires[wi].pts.size(); ++v)
                attach_point(int(base + v));
        }
    }

    nm.pin_root.resize(nm.pin_comp.size(), -1);
    for (size_t i = 0; i < nm.pin_comp.size(); ++i)
        nm.pin_root[i] = uf.find(int(i));
    nm.wire_root.resize(wires.size(), -1);
    {
        size_t idx = wire_base;
        for (size_t i = 0; i < wires.size(); ++i) {
            nm.wire_root[i] = uf.find(int(idx));
            idx += wires[i].pts.size();
        }
    }

    // Collect roots.
    std::vector<int> roots;
    for (int i = 0; i < n; ++i) {
        int r = uf.find(i);
        if (std::find(roots.begin(), roots.end(), r) == roots.end())
            roots.push_back(r);
    }

    // Naming: ground wins, then VDD, then the first user label on the net.
    for (int r : roots) {
        std::string name;
        for (size_t i = 0; i < nm.pin_comp.size(); ++i) {
            if (nm.pin_root[i] != r) continue;
            auto k = circuit.comps[nm.pin_comp[i]].kind;
            if (k == syms::Kind::GND) { name = "0"; break; }
            if (k == syms::Kind::VDD) { name = "VDD"; break; }
        }
        if (name.empty()) {
            // Priority: a GND-style label ("0", "gnd", "ground") wins over
            // anything else, then a VDD-style label ("VDD", "VCC"), then any
            // other user label. Earlier iterations might have set `name` to a
            // less-canonical label that happened to be on the wire first.
            if (name.empty() || (!is_gnd_name(name) && !is_vdd_name(name))) {
                for (size_t k = 0; k < labels.size(); ++k) {
                    if (uf.find(int(label_base + int(k))) != r) continue;
                    if (labels[k].name.empty()) continue;
                    if (is_gnd_name(labels[k].name)) {
                        name = "0";
                        break;
                    }
                    if (is_vdd_name(labels[k].name)) {
                        if (!is_gnd_name(name)) {
                            name = "VDD";
                            continue; // keep scanning: a GND label on this
                                     // same net would still win
                        }
                    }
                    if (name.empty()) {
                        name = labels[k].name;
                    }
                }
            }
        }
        nm.name[r] = name;
    }
    // Auto-name the still-unnamed nets, deterministically.
    int auto_n = 1;
    for (int r : roots) {
        if (!nm.name[r].empty()) continue;
        std::string name;
        do {
            name = "n" + std::to_string(auto_n++);
        } while (name == "0");
        nm.name[r] = name;
    }
    return nm;
}

syms::Circuit Document::resolved(std::string& err) const {
    err.clear();
    NetMap nm = net_map();

    syms::Circuit out = circuit;
    for (size_t i = 0; i < nm.pin_comp.size(); ++i) {
        const std::string& name = nm.name[nm.pin_root[i]];
        out.comps[nm.pin_comp[i]].nodes[nm.pin_index[i]] = name;
    }

    // Materialise mirror copies (scaled device parameters from their unit).
    try {
        syms::resolve_mirrors(out);
    } catch (const std::exception& e) {
        err = e.what();
        syms::Circuit empty;
        return empty;
    }

    // Duplicate references can't happen (next_ref), but validate anyway.
    if (!out.validate(err)) {
        syms::Circuit empty;
        return empty;
    }
    return out;
}

// ---------------------------------------------------------------------------
// wire endpoint bindings
// ---------------------------------------------------------------------------
namespace {
// Orthogonalise a polyline: replace every diagonal segment with two axis-
// aligned legs joined at a corner. Same rule as the canvas uses, so the
// model-level sync and the interactive drag agree.
void ortho_fix_pts(std::vector<Pt>& pts) {
    if (pts.size() < 2) return;
    std::vector<Pt> out;
    out.push_back(pts[0]);
    bool prev_h = false;
    auto eq = [](Pt a, Pt b) {
        return std::fabs(a.first - b.first) < 1e-9 &&
               std::fabs(a.second - b.second) < 1e-9;
    };
    for (size_t i = 0; i + 1 < pts.size(); ++i) {
        Pt a = pts[i], b = pts[i + 1];
        bool horiz = std::fabs(b.second - a.second) < 1e-9;
        bool vert = std::fabs(b.first - a.first) < 1e-9;
        if (horiz || vert) {
            out.push_back(b);
            prev_h = horiz;
            continue;
        }
        Pt c1{b.first, a.second};
        Pt c2{a.first, b.second};
        Pt next = (i + 2 < pts.size()) ? pts[i + 2] : b;
        auto bad = [&](Pt c) {
            return eq(c, next) || eq(c, a) || eq(c, b);
        };
        bool c1_bad = bad(c1), c2_bad = bad(c2);
        if (c1_bad && c2_bad) { out.push_back(b); prev_h = false; continue; }
        bool h_first = c2_bad ? true : (c1_bad ? false : !prev_h);
        Pt corner = h_first ? c1 : c2;
        if (!eq(out.back(), corner)) out.push_back(corner);
        out.push_back(b);
        prev_h = !h_first;
    }
    pts = std::move(out);
}
} // namespace

Pt Document::wire_end_pt(const Wire& w, bool start) const {
    const WireEnd& e = start ? w.a : w.b;
    if (e.kind == WireEnd::Kind::Pin) {
        const syms::Component* c = circuit.find(e.ref);
        auto pl = placements.find(e.ref);
        if (c && pl != placements.end())
            return pin_world(*c, pl->second, e.pin);
    }
    if (w.pts.empty()) return {0, 0};
    return start ? w.pts.front() : w.pts.back();
}

void Document::bind_wire_ends(Wire& w) const {
    if (w.pts.size() < 2) return;
    // Bind each end to a pin whose world position coincides with it.
    // A pin match wins over a free coordinate because it's the stronger
    // declaration of intent (the user drew from / to a pin).
    //
    // The tolerance is the *same* one the net merge uses (kJoinTol): anything
    // the resolver treats as touching a pin must also be bound to it, or a wire
    // that looks (and electrically is) connected to a pin stays put when the
    // component is dragged and the connection silently breaks.
    auto bind_one = [&](bool start) {
        Pt p = start ? w.pts.front() : w.pts.back();
        for (const auto& c : circuit.comps) {
            auto pl = placements.find(c.ref);
            if (pl == placements.end()) continue;
            int np = int(pin_offsets(c.kind).size());
            for (int i = 0; i < np; ++i) {
                Pt pw = pin_world(c, pl->second, i);
                if (std::hypot(pw.first - p.first, pw.second - p.second) <=
                    kJoinTol) {
                    WireEnd e;
                    e.kind = WireEnd::Kind::Pin;
                    e.ref = c.ref;
                    e.pin = i;
                    if (start) w.a = e; else w.b = e;
                    return;
                }
            }
        }
        WireEnd e;
        e.kind = WireEnd::Kind::Free;
        if (start) w.a = e; else w.b = e;
    };
    bind_one(true);
    bind_one(false);
}

void Document::sync_wire_endpoints() {
    for (auto& w : wires) {
        if (w.pts.size() < 2) continue;
        // Interior pin points: a merged wire can pass through a pin between
        // its two ends. Move those waypoints with the component. A waypoint
        // that sits on a pin but has no binding yet (e.g. a file saved before
        // interior pin bindings existed) is captured the same way endpoints
        // are: within the net-merge tolerance, so this never invents a
        // connection the resolver would not already make.
        for (auto& pp : w.pin_points) {
            if (pp.waypoint < 0 || pp.waypoint >= int(w.waypoints.size()))
                continue;
            const syms::Component* c = circuit.find(pp.end.ref);
            auto pl = placements.find(pp.end.ref);
            if (!c || pl == placements.end()) {
                pp.waypoint = -1;
                continue;
            }
            w.waypoints[pp.waypoint] = pin_world(*c, pl->second, pp.end.pin);
        }
        for (int k = 0; k < int(w.waypoints.size()); ++k) {
            bool bound = false;
            for (const auto& pp : w.pin_points)
                if (pp.waypoint == k) { bound = true; break; }
            if (bound) continue;
            for (const auto& c : circuit.comps) {
                auto pl = placements.find(c.ref);
                if (pl == placements.end()) continue;
                int np = int(pin_offsets(c.kind).size());
                for (int pi = 0; pi < np; ++pi) {
                    Pt pw = pin_world(c, pl->second, pi);
                    if (std::hypot(pw.first - w.waypoints[k].first,
                                   pw.second - w.waypoints[k].second) <=
                        kJoinTol) {
                        Wire::PinPoint pp;
                        pp.waypoint = k;
                        pp.end.kind = WireEnd::Kind::Pin;
                        pp.end.ref = c.ref;
                        pp.end.pin = pi;
                        w.pin_points.push_back(pp);
                        bound = true;
                        break;
                    }
                }
                if (bound) break;
            }
        }
        w.pin_points.erase(
            std::remove_if(w.pin_points.begin(), w.pin_points.end(),
                           [](const Wire::PinPoint& pp) {
                               return pp.waypoint < 0 ||
                                      pp.end.kind != WireEnd::Kind::Pin;
                           }),
            w.pin_points.end());

        // Electrical capture: a *free* endpoint that has ended up on (or within
        // the merge tolerance of) a pin is made a pin end, so the next component
        // move carries the wire instead of leaving it behind. Already-bound ends
        // are left alone (their stored coordinate is stale by construction).
        // The tolerance is the net-merge one, so this can never introduce a
        // connection the resolver would not already make.
        auto capture = [&](bool start) {
            WireEnd& e = start ? w.a : w.b;
            if (e.kind == WireEnd::Kind::Pin) return;
            Pt p = start ? w.pts.front() : w.pts.back();
            for (const auto& c : circuit.comps) {
                auto pl = placements.find(c.ref);
                if (pl == placements.end()) continue;
                int np = int(pin_offsets(c.kind).size());
                for (int i = 0; i < np; ++i) {
                    Pt pw = pin_world(c, pl->second, i);
                    if (std::hypot(pw.first - p.first, pw.second - p.second) <=
                        kJoinTol) {
                        e.kind = WireEnd::Kind::Pin;
                        e.ref = c.ref;
                        e.pin = i;
                        return;
                    }
                }
            }
        };
        capture(true);
        capture(false);
        Pt a = wire_end_pt(w, true);
        Pt b = wire_end_pt(w, false);
        // Rebuild the polyline from the authored waypoints, never by mutating
        // the stored `pts`. This is what keeps a stretching route from
        // accumulating stale elbows: each move re-derives the geometry from
        // the pinned vertices (analog-canvas's stable-waypoint model).
        std::vector<Pt> rebuilt;
        rebuilt.push_back(a);
        for (const auto& wp : w.waypoints) rebuilt.push_back(wp);
        rebuilt.push_back(b);
        // The pinned waypoints must survive both the orthogonaliser and the
        // collinearity cleanup; remember them by coordinate.
        std::vector<Pt> pinned_pos;
        for (const auto& pp : w.pin_points)
            if (pp.waypoint >= 0 && pp.waypoint < int(w.waypoints.size()))
                pinned_pos.push_back(w.waypoints[pp.waypoint]);
        auto mark_pinned = [&](const std::vector<Pt>& pts) {
            std::vector<bool> mark(pts.size(), false);
            for (const auto& q : pinned_pos) {
                for (size_t i = 0; i < pts.size(); ++i) {
                    if (std::hypot(pts[i].first - q.first,
                                   pts[i].second - q.second) < 1e-6) {
                        mark[i] = true;
                        break;
                    }
                }
            }
            return mark;
        };
        // Orthogonalise the end legs (waypoints are grid points, so the
        // interior legs they form are usually already axis-aligned).
        ortho_fix_pts(rebuilt);
        std::vector<bool> pinned = mark_pinned(rebuilt);
        // Drop bends that became collinear with their neighbours, except the
        // pins.
        if (rebuilt.size() >= 3) {
            std::vector<Pt> keep{rebuilt.front()};
            for (size_t i = 1; i + 1 < rebuilt.size(); ++i) {
                const Pt& p0 = keep.back();
                const Pt& p1 = rebuilt[i];
                const Pt& p2 = rebuilt[i + 1];
                bool collinear =
                    (std::fabs(p0.first - p1.first) < 1e-9 &&
                     std::fabs(p1.first - p2.first) < 1e-9) ||
                    (std::fabs(p0.second - p1.second) < 1e-9 &&
                     std::fabs(p1.second - p2.second) < 1e-9);
                if (!collinear || pinned[i]) keep.push_back(p1);
            }
            keep.push_back(rebuilt.back());
            rebuilt = std::move(keep);
        }
        w.pts = std::move(rebuilt);
    }
}

void Document::remap_pin_points(Wire& w,
                                const std::vector<Pt>& old_waypoints) {
    for (auto& pp : w.pin_points) {
        if (pp.waypoint < 0 || pp.waypoint >= int(old_waypoints.size())) {
            pp.waypoint = -1;
            continue;
        }
        Pt q = old_waypoints[pp.waypoint];
        int best = -1;
        double bd = kJoinTol;
        for (size_t k = 0; k < w.waypoints.size(); ++k) {
            double d = std::hypot(w.waypoints[k].first - q.first,
                                  w.waypoints[k].second - q.second);
            if (d <= bd) {
                bd = d;
                best = int(k);
            }
        }
        pp.waypoint = best;
    }
    w.pin_points.erase(
        std::remove_if(w.pin_points.begin(), w.pin_points.end(),
                       [](const Wire::PinPoint& pp) {
                           return pp.waypoint < 0 ||
                                  pp.end.kind != WireEnd::Kind::Pin;
                       }),
        w.pin_points.end());
}

void Document::escape_pin_ends(Wire& w) {
    if (w.pts.size() < 2) return;
    // analog-canvas: the outward axis is *advisory*. A wire may reach a pin
    // from any direction -- "reaching a downward pin from the side is an
    // ordinary drawing". So we do NOT force a lead along the pin axis. The
    // only thing we keep is idempotency: if the wire already has a short
    // on-axis lead vertex (e.g. from an older file), snap it to the pin so a
    // drag does not accumulate vertices. No new vertices are ever inserted.
    auto do_end = [&](bool start) {
        const WireEnd& e = start ? w.a : w.b;
        if (e.kind != WireEnd::Kind::Pin) return;
        const syms::Component* c = circuit.find(e.ref);
        auto pl = placements.find(e.ref);
        if (!c || pl == placements.end()) return;
        Pt dir = pin_outward(*c, pl->second, e.pin);
        if (dir.first == 0 && dir.second == 0) return;
        Pt pin = pin_world(*c, pl->second, e.pin);
        size_t ni = start ? 1 : w.pts.size() - 2;
        Pt next = w.pts[ni];
        double perp = (next.first - pin.first) * (-dir.second) +
                      (next.second - pin.second) * dir.first;
        double along = (next.first - pin.first) * dir.first +
                       (next.second - pin.second) * dir.second;
        // Only normalise an existing on-axis lead vertex; leave off-axis
        // neighbours exactly as drawn (the orthogonaliser handles diagonals).
        if (std::fabs(perp) < 1e-6 && along > 1e-6 && along <= 20.0) {
            Pt lead_pt{pin.first + dir.first * 10.0,
                       pin.second + dir.second * 10.0};
            w.pts[ni] = lead_pt;
        }
    };
    do_end(true);
    do_end(false);
}

// ---------------------------------------------------------------------------
// interactive net helpers
// ---------------------------------------------------------------------------
namespace {
double pt_dist(Pt a, Pt b) {
    return std::hypot(a.first - b.first, a.second - b.second);
}
// Nearest point to `q` on segment ab.
Pt nearest_on_seg(Pt q, Pt a, Pt b) {
    double vx = b.first - a.first, vy = b.second - a.second;
    double wx = q.first - a.first, wy = q.second - a.second;
    double L2 = vx * vx + vy * vy;
    if (L2 < 1e-12) return a;
    double t = (wx * vx + wy * vy) / L2;
    t = std::max(0.0, std::min(1.0, t));
    return {a.first + t * vx, a.second + t * vy};
}
} // namespace

std::string Document::net_name_of_wire(int wire_index) const {
    if (wire_index < 0 || wire_index >= int(wires.size())) return std::string();
    NetMap nm = net_map();
    int r = nm.wire_root[wire_index];
    auto it = nm.name.find(r);
    return it == nm.name.end() ? std::string() : it->second;
}

std::string Document::net_name_of_pin(const std::string& ref, int pin) const {
    int ci = -1;
    for (size_t i = 0; i < circuit.comps.size(); ++i)
        if (circuit.comps[i].ref == ref) { ci = int(i); break; }
    if (ci < 0) return std::string();
    NetMap nm = net_map();
    int r = nm.root_of_pin(ci, pin);
    auto it = nm.name.find(r);
    return it == nm.name.end() ? std::string() : it->second;
}

Pt Document::net_anchor_near(const std::string& ref, int pin, Pt near) const {
    // The anchor for a pin is simply the pin's world position; snap it onto the
    // nearest point of any wire on the same net, if such a wire exists.
    int ci = -1;
    for (size_t i = 0; i < circuit.comps.size(); ++i)
        if (circuit.comps[i].ref == ref) { ci = int(i); break; }
    if (ci < 0) return near;
    Pt pin_world;
    {
        const auto& c = circuit.comps[ci];
        auto pl = placements.find(c.ref);
        double ox = pl == placements.end() ? 0.0 : pl->second.x;
        double oy = pl == placements.end() ? 0.0 : pl->second.y;
        int rot = pl == placements.end() ? 0 : pl->second.rot;
        bool fh = pl != placements.end() && pl->second.flip_h;
        bool fv = pl != placements.end() && pl->second.flip_v;
        Pt p = pin_offsets(c.kind)[pin];
        if (fh) p.first = -p.first;
        if (fv) p.second = -p.second;
        Pt rp = rotate_pt(p, rot);
        pin_world = {ox + rp.first, oy + rp.second};
    }
    NetMap nm = net_map();
    int r = nm.root_of_pin(ci, pin);
    if (r < 0) return pin_world;
    Pt best = pin_world;
    double bestd = 1e300;
    for (size_t wi = 0; wi < wires.size(); ++wi) {
        if (nm.wire_root[wi] != r) continue;
        Pt q = net_anchor_near_wire(int(wi), pin_world);
        double d = pt_dist(q, pin_world);
        if (d < bestd) { bestd = d; best = q; }
    }
    (void)near;
    return best;
}

Pt Document::net_anchor_near_wire(int wire_index, Pt near) const {
    if (wire_index < 0 || wire_index >= int(wires.size())) return near;
    const auto& w = wires[wire_index];
    Pt best = near;
    double bestd = 1e300;
    for (size_t k = 1; k < w.pts.size(); ++k) {
        Pt q = nearest_on_seg(near, w.pts[k - 1], w.pts[k]);
        double d = pt_dist(q, near);
        if (d < bestd) { bestd = d; best = q; }
    }
    return best;
}

// Point on wire `wi`'s polyline at arc-length fraction f (0..1).
Pt Document::wire_pt_at(int wi, double f) const {
    if (wi < 0 || wi >= int(wires.size())) return {0, 0};
    const auto& w = wires[wi];
    if (w.pts.size() < 2) return w.pts.empty() ? Pt{0, 0} : w.pts.front();
    double total = 0.0;
    for (size_t k = 1; k < w.pts.size(); ++k)
        total += std::hypot(w.pts[k].first - w.pts[k - 1].first,
                            w.pts[k].second - w.pts[k - 1].second);
    if (total <= 1e-9) return w.pts.front();
    double want = std::max(0.0, std::min(1.0, f)) * total;
    double acc = 0.0;
    for (size_t k = 1; k < w.pts.size(); ++k) {
        Pt a = w.pts[k - 1], b = w.pts[k];
        double seg = std::hypot(b.first - a.first, b.second - a.second);
        if (acc + seg >= want) {
            double t = seg > 0 ? (want - acc) / seg : 0.0;
            return {a.first + t * (b.first - a.first),
                    a.second + t * (b.second - a.second)};
        }
        acc += seg;
    }
    return w.pts.back();
}

// Arc-length fraction of point q along wire `wi`'s polyline (clamped 0..1) --
// the inverse of wire_pt_at, used to (re)bind a label to a wire.
double Document::wire_frac_of(int wi, Pt q) const {
    if (wi < 0 || wi >= int(wires.size())) return 0.0;
    const auto& w = wires[wi];
    if (w.pts.size() < 2) return 0.0;
    double total = 0.0;
    for (size_t k = 1; k < w.pts.size(); ++k)
        total += std::hypot(w.pts[k].first - w.pts[k - 1].first,
                            w.pts[k].second - w.pts[k - 1].second);
    if (total <= 1e-9) return 0.0;
    double acc = 0.0, best = 0.0, bestd = 1e300;
    for (size_t k = 1; k < w.pts.size(); ++k) {
        Pt a = w.pts[k - 1], b = w.pts[k];
        double seg = std::hypot(b.first - a.first, b.second - a.second);
        Pt n = nearest_on_seg(q, a, b);
        double d = pt_dist(n, q);
        if (d < bestd) {
            bestd = d;
            double t = seg > 0 ? pt_dist(n, a) / seg : 0.0;
            best = acc + t * seg;
        }
        acc += seg;
    }
    return std::max(0.0, std::min(1.0, best / total));
}

// Re-resolve every wire-bound label's anchor/display point from its wire. Call
// after any wire edit (drag, re-route, component move).
void Document::sync_label_anchors() {
    for (auto& l : labels) {
        if (l.bind_wire < 0 || l.bind_wire >= int(wires.size())) continue;
        Pt a = wire_pt_at(l.bind_wire, l.bind_frac);
        Pt d = {l.pt.first - l.anchor.first, l.pt.second - l.anchor.second};
        l.anchor = a;
        l.pt = {a.first + d.first, a.second + d.second};
    }
}

Pt Document::label_display_pt(int wire_index, Pt anchor, int name_len) const {
    // Decide the offset from the local wire direction: a label beside a
    // vertical wire (to the right), above a horizontal one, and up-right at a
    // corner. This keeps the text clear of the wire instead of straddling it.
    bool vertical = false, horizontal = false;
    if (wire_index >= 0 && wire_index < int(wires.size())) {
        const auto& w = wires[wire_index];
        for (size_t k = 1; k < w.pts.size(); ++k) {
            Pt a = w.pts[k - 1], b = w.pts[k];
            if (std::fabs(b.first - a.first) < 1e-6 &&
                std::fabs(b.second - a.second) > 1e-6)
                vertical = true;
            if (std::fabs(b.second - a.second) < 1e-6 &&
                std::fabs(b.first - a.first) > 1e-6)
                horizontal = true;
        }
    }
    // The renderer draws the text *centred* on `pt`, so compute the centre of
    // the box we want the text to occupy.
    const double fs = 9.0;                 // default label font size (pts)
    const double line_h = fs * 1.5;        // rough text line height
    const double text_w = std::max(1, name_len) * fs * 0.62; // rough width
    const double gap = 4.0;                // clearance from the wire
    if (vertical && !horizontal) {
        // to the right of the wire, vertically centred on the anchor
        return {anchor.first + gap + text_w / 2, anchor.second};
    }
    if (horizontal && !vertical) {
        // above the wire, horizontally centred on the anchor
        return {anchor.first, anchor.second - gap - line_h / 2};
    }
    // corner / mixed wire: up and to the right, clear of both segments
    return {anchor.first + gap + text_w / 2, anchor.second - gap - line_h / 2};
}

int Document::ensure_label_on_wire(int wire_index, Pt at) {
    if (wire_index < 0 || wire_index >= int(wires.size())) return -1;
    // Reuse a label already anchored on this net: find every wire sharing the
    // target net's union-find root, then scan labels for one whose anchor lies
    // on any of those wires.
    NetMap nm = net_map();
    int r = nm.wire_root[wire_index];
    for (size_t li = 0; li < labels.size(); ++li) {
        const Pt& a = labels[li].anchor;
        for (size_t wi = 0; wi < wires.size(); ++wi) {
            if (nm.wire_root[wi] != r) continue;
            for (size_t k = 1; k < wires[wi].pts.size(); ++k)
                if (point_on_seg(a, wires[wi].pts[k - 1], wires[wi].pts[k],
                                 kJoinTol)) {
                    return int(li);
                }
        }
    }
    const auto& w = wires[wire_index];
    if (w.pts.size() < 2) return -1;
    NetLabel l;
    l.anchor = net_anchor_near_wire(wire_index, at);
    // Bind the label to this wire so it follows drags / re-routes.
    l.bind_wire = wire_index;
    l.bind_frac = wire_frac_of(wire_index, l.anchor);
    // default display point: offset to the readable side of the wire
    l.pt = label_display_pt(wire_index, l.anchor, 4);
    l.name = ""; // unnamed until the user types one
    l.font_size = 9;
    l.rot = 0;
    labels.push_back(l);
    return int(labels.size()) - 1;
}

bool Document::label_attached(int label_index, const NetMap& nm) const {
    if (label_index < 0 || label_index >= int(labels.size())) return false;
    // A label is attached if its anchor coincides with (or lies on) some wire
    // or pin. Recomputed on demand from geometry.
    Pt a = labels[label_index].anchor;
    for (const auto& w : wires)
        for (size_t k = 1; k < w.pts.size(); ++k)
            if (point_on_seg(a, w.pts[k - 1], w.pts[k], kJoinTol)) return true;
    (void)nm;
    return false;
}

// ---------------------------------------------------------------------------
// serialization
// ---------------------------------------------------------------------------
std::string Document::serialize() const {
    std::ostringstream o;
    o << "envlp 1\n";
    o << "req " << quote(req.input_ref) << " " << quote(req.output) << " "
      << req.sweep.f_start_hz << " " << req.sweep.f_stop_hz << " "
      << int(req.sweep.type) << " " << req.sweep.points_per_interval << " "
      << (req.prune ? 1 : 0) << " " << (req.use_parallel ? 1 : 0) << " "
      << (req.approx_factor ? 1 : 0)
      << "\n";
    // DC tech settings (large-signal model): mode, Vthn, Vthp, Is, uCox,
    // assume-saturation, supply. Absent in older files (defaults apply).
    o << "tech " << int(tech.dc_mode) << " " << tech.vthn << " " << tech.vthp
      << " " << tech.is << " " << tech.uncox << " " << tech.upcox << " "
      << (tech.assume_saturation ? 1 : 0) << " "
      << (tech.include_clm ? 1 : 0) << " " << tech.vdd << "\n";
    for (const auto& c : circuit.comps) {
        auto pl = placements.find(c.ref);
        double x = pl == placements.end() ? 0.0 : pl->second.x;
        double y = pl == placements.end() ? 0.0 : pl->second.y;
        int rot = pl == placements.end() ? 0 : pl->second.rot;
        int fh = pl != placements.end() && pl->second.flip_h ? 1 : 0;
        int fv = pl != placements.end() && pl->second.flip_v ? 1 : 0;
        o << "comp " << quote(c.ref) << " " << quote(syms::kind_token(c.kind))
          << " " << x << " " << y << " " << rot << " " << fh << " " << fv << " "
          << c.size_db << " " << quote(c.value_text);
        for (const auto& lk : c.links) o << " " << quote(lk);
        // Independent sources may carry non-default DC / AC values.
        if (syms::is_independent_source(c.kind)) {
            if (c.dc_text != "0" && !c.dc_text.empty())
                o << " dc=" << c.dc_text;
            if (c.ac_text != "1" && !c.ac_text.empty())
                o << " ac=" << c.ac_text;
        }
        // Mirror copies carry "mirror=<unit> mult=<m>" after the links.
        if (!c.mirror_ref.empty())
            o << " mirror=" << c.mirror_ref << " mult=" << c.mirror_num()
              << " den=" << c.mirror_dn();
        o << "\n";
        // Emit every parameter the user has touched (a parasitic toggle, an
        // estimate, or a size offset) -- not only the ones with a toggle, so a
        // plain parameter such as a transformer's Lp/Ls/k survives a round-trip.
        std::set<std::string> pnames;
        for (const auto& kv : c.param_on) pnames.insert(kv.first);
        for (const auto& kv : c.param_text) pnames.insert(kv.first);
        for (const auto& kv : c.param_db) pnames.insert(kv.first);
        for (const auto& p : pnames)
            o << "param " << quote(c.ref) << " " << quote(p) << " "
              << (c.param_enabled(p) ? 1 : 0) << " "
              << quote(c.param_text.count(p) ? c.param_text.at(p)
                                             : std::string())
              << " " << (c.param_db.count(p) ? c.param_db.at(p) : 0) << "\n";
    }
    for (const auto& w : wires) {
        // Format: "wire x,y x,y ... [a=<pin|free>] [b=<pin|free>]"
        // The endpoint binding is appended so reloading keeps the wire
        // attached to its pins. Older files without it deserialize as free
        // ends and are re-bound on load.
        o << "wire";
        for (auto& p : w.pts)
            o << " " << p.first << "," << p.second;
        auto emit_end = [&](const WireEnd& e) {
            if (e.kind == WireEnd::Kind::Pin)
                o << " pin:" << e.ref << ":" << e.pin;
            else
                o << " free";
        };
        emit_end(w.a);
        emit_end(w.b);
        // Authored interior waypoints (so a loaded route re-syncs from the
        // same pinned vertices). Omitted when there are none (a plain route).
        for (const auto& wp : w.waypoints)
            o << " wp=" << wp.first << "," << wp.second;
        // A merged wire can pass through a pin between its ends; remember
        // which waypoint that is so a component move keeps carrying the wire.
        for (const auto& pp : w.pin_points)
            if (pp.end.kind == WireEnd::Kind::Pin)
                o << " pinwp=" << pp.waypoint << ":" << pp.end.ref << ":"
                  << pp.end.pin;
        o << "\n";
    }
    for (const auto& l : labels) {
        // Format: "netlabel ax,ay [x,y] \"name\" [fs] [rot]"
        // The display point is omitted when equal to the anchor (typical case).
        o << "netlabel " << l.anchor.first << "," << l.anchor.second;
        if (l.pt.first != l.anchor.first || l.pt.second != l.anchor.second)
            o << " " << l.pt.first << "," << l.pt.second;
        o << " " << quote(l.name) << " " << l.font_size << " " << l.rot;
        if (l.bind_wire >= 0)
            o << " bind=" << l.bind_wire << ":" << l.bind_frac;
        o << "\n";
    }
    if (!analysis_cards.empty()) o << analysis_cards;
    return o.str();
}

bool Document::deserialize(const std::string& data, std::string& err) {
    circuit.comps.clear();
    placements.clear();
    wires.clear();
    labels.clear();
    analysis_cards.clear();
    req = syms::AnalysisRequest{};

    std::istringstream in(data);
    std::string line;
    bool saw_header = false;
    bool tech_vdd_seen = false;
    int lineno = 0;
    while (std::getline(in, line)) {
        ++lineno;
        if (line.empty()) continue;
        size_t i = 0;
        std::string kw;
        if (!next_token(line, i, kw)) continue;
        auto fail = [&](const std::string& m) {
            err = "line " + std::to_string(lineno) + ": " + m;
            return false;
        };
        auto need = [&](std::string& t) { return next_token(line, i, t); };

        if (kw == "envlp" || kw == "symcirc") {
            // "envlp 1" is the current header; the legacy "symcirc 1" is
            // accepted so circuits saved before the rename still load.
            saw_header = true;
        } else if (kw == "req") {
            std::string a, b;
            if (!need(a) || !need(b)) return fail("bad req");
            req.input_ref = a;
            req.output = b;
            // Collect the remaining numeric tokens; interpret by count so
            // both the old (f0/threshold/global) and the new sweep forms load.
            std::vector<std::string> rest;
            std::string t;
            while (next_token(line, i, t)) rest.push_back(t);
            if (rest.size() >= 8) {
                // new, with the legacy gm_ro field: fstart fstop stype npts
                // prune par gro approx  (the gro field is ignored).
                req.sweep.f_start_hz = std::atof(rest[0].c_str());
                req.sweep.f_stop_hz = std::atof(rest[1].c_str());
                int ty = std::atoi(rest[2].c_str());
                req.sweep.type = ty == 1 ? syms::SweepType::Octave
                                         : ty == 2 ? syms::SweepType::Linear
                                                   : syms::SweepType::Decade;
                req.sweep.points_per_interval = std::atoi(rest[3].c_str());
                req.prune = rest[4] != "0";
                req.use_parallel = rest[5] != "0";
                req.approx_factor = rest[7] != "0";
            } else if (rest.size() == 7) {
                // new, no gm_ro field: fstart fstop stype npts prune par approx
                req.sweep.f_start_hz = std::atof(rest[0].c_str());
                req.sweep.f_stop_hz = std::atof(rest[1].c_str());
                int ty = std::atoi(rest[2].c_str());
                req.sweep.type = ty == 1 ? syms::SweepType::Octave
                                         : ty == 2 ? syms::SweepType::Linear
                                                   : syms::SweepType::Decade;
                req.sweep.points_per_interval = std::atoi(rest[3].c_str());
                req.prune = rest[4] != "0";
                req.use_parallel = rest[5] != "0";
                req.approx_factor = rest[6] != "0";
            } else if (rest.size() >= 5) {
                // old: f0 threshold global prune par [approx]
                req.f0_hz = std::atof(rest[0].c_str());
                req.threshold_db = std::atof(rest[1].c_str());
                req.global_ref = rest[2] != "0";
                if (rest.size() > 3) req.prune = rest[3] != "0";
                if (rest.size() > 4) req.use_parallel = rest[4] != "0";
                if (rest.size() > 5) req.approx_factor = rest[5] != "0";
                req.sweep.f_start_hz = req.f0_hz > 0 ? req.f0_hz : 1.0;
                req.sweep.f_stop_hz = req.sweep.f_start_hz * 1e6;
            }
        } else if (kw == "tech") {
            // Collect every token so the current layout (mode Vthn Vthp Is uCox
            // assumeSat Vdd) and the older layouts load.
            std::vector<std::string> t;
            std::string tok;
            while (next_token(line, i, tok)) t.push_back(tok);
            if (t.size() < 8) return fail("bad tech");
            int md = std::atoi(t[0].c_str());
            tech.dc_mode = md == 0 ? syms::DcMode::GmOverId
                                   : syms::DcMode::SquareLaw;
            size_t k = 1; // index of the first process value
            auto is01 = [](const std::string& s) {
                return s == "0" || s == "1";
            };
            const bool new_layout =
                (t.size() == 9 && is01(t[6]) && is01(t[7])) || t.size() == 8;
            if (t.size() >= 11 || new_layout) {
                tech.vthn = std::atof(t[k++].c_str());
                tech.vthp = std::atof(t[k++].c_str());
            } else {
                // Older files carried a single Vth; use it for both polarities.
                double v = std::atof(t[k++].c_str());
                tech.vthn = v;
                tech.vthp = v;
            }
            tech.is = std::atof(t[k++].c_str());
            tech.uncox = std::atof(t[k++].c_str());
            tech.upcox = std::atof(t[k++].c_str());
            if (new_layout) {
                tech.assume_saturation = t[k++] != "0";
                if (t.size() >= 9) tech.include_clm = t[k++] != "0";
                tech.vdd = std::atof(t[k++].c_str());
                tech_vdd_seen = true;
            } else {
                // Legacy: model_file, nmos_model, pmos_model, override[, vdd].
                if (t.size() >= 11) {
                    k += 3; // skip model file + model names
                    k += 1; // skip override
                } else {
                    k += 3;
                    if (k < t.size()) k += 1;
                }
                if (k < t.size()) {
                    tech.vdd = std::atof(t[k].c_str());
                    tech_vdd_seen = true;
                }
            }
        } else if (kw == "comp") {
            std::string ref, tok, sx, sy, srot, sfh, sfv, sdb, val;
            if (!need(ref) || !need(tok) || !need(sx) || !need(sy) ||
                !need(srot) || !need(sfh) || !need(sfv) || !need(sdb) ||
                !need(val))
                return fail("bad comp");
            syms::Kind k;
            if (!syms::kind_from_token(tok, k)) return fail("unknown kind " + tok);
            syms::Component c;
            c.ref = ref;
            c.kind = k;
            c.value_text = val;
            c.size_db = std::atoi(sdb.c_str());
            c.nodes.assign(syms::pin_count(k), "");
            // K stores its two coupled-inductor refs at the end of the line;
            // mirror copies append "mirror=<unit> mult=<m>".
            std::string extra;
            while (next_token(line, i, extra)) {
                if (extra.rfind("mirror=", 0) == 0) {
                    c.mirror_ref = extra.substr(7);
                } else if (extra.rfind("mult=", 0) == 0) {
                    c.mirror_mult = std::atoi(extra.c_str() + 5);
                    if (c.mirror_mult < 1) c.mirror_mult = 1;
                } else if (extra.rfind("den=", 0) == 0) {
                    c.mirror_den = std::atoi(extra.c_str() + 4);
                    if (c.mirror_den < 1) c.mirror_den = 1;
                } else if (extra.rfind("dc=", 0) == 0) {
                    c.dc_text = extra.substr(3);
                } else if (extra.rfind("ac=", 0) == 0) {
                    c.ac_text = extra.substr(3);
                } else {
                    c.links.push_back(extra);
                }
            }
            circuit.comps.push_back(c);
            Placement pl;
            pl.x = std::atof(sx.c_str());
            pl.y = std::atof(sy.c_str());
            pl.rot = std::atoi(srot.c_str());
            pl.flip_h = sfh != "0";
            pl.flip_v = sfv != "0";
            placements[ref] = pl;
        } else if (kw == "param") {
            std::string ref, name, son, text, sdb;
            if (!need(ref) || !need(name) || !need(son) || !need(text) ||
                !need(sdb))
                return fail("bad param");
            auto* c = const_cast<syms::Component*>(circuit.find(ref));
            if (!c) return fail("param for unknown ref " + ref);
            c->param_on[name] = son != "0";
            c->param_text[name] = text;
            c->param_db[name] = std::atoi(sdb.c_str());
        } else if (kw == "wire") {
            Wire w;
            std::string t;
            bool saw_binding = false;
            while (next_token(line, i, t)) {
                if (t.rfind("wp=", 0) == 0) {
                    // Authored interior waypoint "wp=x,y".
                    double x, y;
                    if (std::sscanf(t.c_str() + 3, "%lf,%lf", &x, &y) != 2)
                        return fail("bad wire waypoint " + t);
                    w.waypoints.push_back({x, y});
                    continue;
                }
                if (t.rfind("pinwp=", 0) == 0) {
                    // Interior pin binding "pinwp=<waypoint>:<REF>:<N>".
                    std::string body = t.substr(6);
                    size_t first = body.find(':');
                    size_t last = body.rfind(':');
                    if (first == std::string::npos || last == std::string::npos ||
                        first == last)
                        return fail("bad wire interior pin binding " + t);
                    Wire::PinPoint pp;
                    pp.waypoint = std::atoi(body.substr(0, first).c_str());
                    pp.end.kind = WireEnd::Kind::Pin;
                    pp.end.ref = body.substr(first + 1, last - first - 1);
                    pp.end.pin = std::atoi(body.c_str() + last + 1);
                    w.pin_points.push_back(pp);
                    continue;
                }
                if (t.rfind("pin:", 0) == 0 || t == "free") {
                    // Endpoint binding: "pin:REF:N" or "free".
                    WireEnd e;
                    if (t == "pin:") return fail("bad wire binding " + t);
                    if (t.rfind("pin:", 0) == 0) {
                        // pin:REF:N  (REF may itself contain ':'; split on
                        // the last colon)
                        std::string body = t.substr(4);
                        size_t colon = body.rfind(':');
                        if (colon == std::string::npos)
                            return fail("bad wire pin binding " + t);
                        e.kind = WireEnd::Kind::Pin;
                        e.ref = body.substr(0, colon);
                        e.pin = std::atoi(body.c_str() + colon + 1);
                    }
                    if (!saw_binding) { w.a = e; saw_binding = true; }
                    else w.b = e;
                    continue;
                }
                double x, y;
                if (std::sscanf(t.c_str(), "%lf,%lf", &x, &y) != 2)
                    return fail("bad wire point " + t);
                w.pts.push_back({x, y});
            }
            if (w.pts.size() < 2) return fail("wire needs 2+ points");
            wires.push_back(w);
        } else if (kw == "netlabel") {
            std::string sa, sp, name;
            if (!need(sa)) return fail("bad netlabel");
            NetLabel l;
            if (std::sscanf(sa.c_str(), "%lf,%lf", &l.anchor.first,
                            &l.anchor.second) != 2)
                return fail("bad netlabel anchor");
            l.pt = l.anchor;
            // The next token may be the display point "x,y" or the name. A
            // display point contains a comma and no spaces; a name may contain
            // any characters but is unlikely to have a comma without a space.
            // Try to sscanf as x,y; on failure it must be the name.
            std::string nxt;
            if (!next_token(line, i, nxt)) return fail("netlabel missing name");
            double dx, dy;
            if (std::sscanf(nxt.c_str(), "%lf,%lf", &dx, &dy) == 2 &&
                nxt.find(' ') == std::string::npos) {
                l.pt = {dx, dy};
                if (!need(name)) return fail("netlabel missing name");
                l.name = name;
            } else {
                l.name = nxt;
            }
            // Optional font size; older files omit it.
            std::string fs;
            if (next_token(line, i, fs)) {
                int n = std::atoi(fs.c_str());
                if (n > 0) l.font_size = n;
            }
            // Optional rotation (degrees, multiples of 90), then an optional
            // wire binding "bind=<index>:<frac>".
            std::string srot;
            if (next_token(line, i, srot)) {
                if (srot.rfind("bind=", 0) == 0) {
                    std::string body = srot.substr(5);
                    size_t colon = body.find(':');
                    l.bind_wire = std::atoi(body.c_str());
                    if (colon != std::string::npos)
                        l.bind_frac = std::atof(body.c_str() + colon + 1);
                } else {
                    l.rot = std::atoi(srot.c_str());
                    std::string sbind;
                    if (next_token(line, i, sbind) &&
                        sbind.rfind("bind=", 0) == 0) {
                        std::string body = sbind.substr(5);
                        size_t colon = body.find(':');
                        l.bind_wire = std::atoi(body.c_str());
                        if (colon != std::string::npos)
                            l.bind_frac = std::atof(body.c_str() + colon + 1);
                    }
                }
            }
            labels.push_back(l);
        } else if (kw == "card") {
            // analysis card line: keep it verbatim in analysis_cards
            analysis_cards += line + "\n";
        } else {
            return fail("unknown keyword " + kw);
        }
    }
    if (!saw_header) {
        err = "not an ENVLP file (missing header)";
        return false;
    }
    // Older files carried the supply voltage on each VDD symbol instead of in
    // the tech settings. Migrate the first one so a saved schematic keeps its
    // rail voltage.
    if (!tech_vdd_seen) {
        bool got = false;
        for (auto& c : circuit.comps) {
            if (c.kind != syms::Kind::VDD) continue;
            if (!got && !c.value_text.empty()) {
                double v = 0.0;
                if (syms::eng::parse_value(c.value_text, v)) tech.vdd = v;
                got = true;
            }
            c.value_text.clear(); // now carried by tech, not the symbol
        }
    }
    // Re-bind any wire end that wasn't explicitly tagged. Old .scx files
    // predate the binding, and hand-written files often omit it; binding by
    // coincidence with a current pin position gets them attached.
    for (auto& w : wires) {
        if (!w.pts.empty()) {
            bool a_tagged = w.a.kind == WireEnd::Kind::Pin;
            bool b_tagged = w.b.kind == WireEnd::Kind::Pin;
            if (!a_tagged || !b_tagged) {
                Wire tmp = w;
                bind_wire_ends(tmp);
                if (!a_tagged) w.a = tmp.a;
                if (!b_tagged) w.b = tmp.b;
            }
        }
    }
    dirty = false;
    return true;
}

bool Document::save(const std::string& p, std::string& err) {
    std::ofstream f(p, std::ios::binary);
    if (!f) {
        err = "cannot open '" + p + "' for writing";
        return false;
    }
    f << serialize();
    if (!f) {
        err = "write failed";
        return false;
    }
    path = p;
    dirty = false;
    return true;
}

bool Document::load(const std::string& p, std::string& err) {
    std::ifstream f(p, std::ios::binary);
    if (!f) {
        err = "cannot open '" + p + "'";
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    if (!deserialize(ss.str(), err)) return false;
    path = p;
    dirty = false;
    undo_.clear();
    redo_.clear();
    return true;
}

// ---------------------------------------------------------------------------
// undo / redo
// ---------------------------------------------------------------------------
namespace {
constexpr size_t kMaxUndo = 100;
} // namespace

void Document::push_undo() {
    undo_.push_back(serialize());
    if (undo_.size() > kMaxUndo) undo_.erase(undo_.begin());
    redo_.clear();
}

bool Document::undo() {
    if (undo_.empty()) return false;
    redo_.push_back(serialize());
    std::string snap = undo_.back();
    undo_.pop_back();
    std::string err;
    if (!deserialize(snap, err)) return false;
    dirty = true;
    return true;
}

bool Document::redo() {
    if (redo_.empty()) return false;
    undo_.push_back(serialize());
    std::string snap = redo_.back();
    redo_.pop_back();
    std::string err;
    if (!deserialize(snap, err)) return false;
    dirty = true;
    return true;
}

} // namespace envlp
