#include "Document.h"
#include "SymbolGeom.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace symcirc {

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
namespace {

constexpr double kJoinTol = 9.0; // px: pin/wire endpoints within this touch

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
    circuit.comps.erase(it);
    placements.erase(ref);
    dirty = true;
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
    std::vector<Pt> pts;
    for (size_t i = 0; i < nm.pin_comp.size(); ++i)
        pts.push_back(pin_pt(nm.pin_comp[i], nm.pin_index[i]));
    size_t wire_base = pts.size();
    for (const auto& w : wires)
        for (auto& p : w.pts) pts.push_back(p);
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

    // Coincident points join.
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j) {
            double dx = pts[i].first - pts[j].first;
            double dy = pts[i].second - pts[j].second;
            if (dx * dx + dy * dy <= kJoinTol * kJoinTol) uf.join(i, j);
        }

    // A point lying on a wire segment joins that wire. This covers pins,
    // label anchors, *and* other wires' vertices -- so a wire drawn
    // perpendicular into the middle of another (a T-junction) merges the two
    // nets, which is what makes the connection real in SPICE terms.
    {
        std::vector<std::pair<size_t, size_t>> spans;
        size_t idx = wire_base;
        for (const auto& w : wires) {
            spans.push_back({idx, w.pts.size()});
            idx += w.pts.size();
        }
        auto attach_point = [&](int pi) {
            for (const auto& sp : spans)
                for (size_t k = 1; k < sp.second; ++k)
                    if (point_on_seg(pts[pi], pts[sp.first + k - 1],
                                     pts[sp.first + k], kJoinTol)) {
                        uf.join(pi, int(sp.first + k));
                        uf.join(pi, int(sp.first + k - 1));
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

    // Duplicate references can't happen (next_ref), but validate anyway.
    if (!out.validate(err)) {
        syms::Circuit empty;
        return empty;
    }
    return out;
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
    o << "symcirc 1\n";
    o << "req " << quote(req.input_ref) << " " << quote(req.output) << " "
      << req.sweep.f_start_hz << " " << req.sweep.f_stop_hz << " "
      << int(req.sweep.type) << " " << req.sweep.points_per_interval << " "
      << (req.prune ? 1 : 0) << " " << (req.use_parallel ? 1 : 0) << " "
      << (req.gm_ro_assume ? 1 : 0) << " " << (req.approx_factor ? 1 : 0)
      << "\n";
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
        o << "\n";
        for (const auto& kv : c.param_on)
            o << "param " << quote(c.ref) << " " << quote(kv.first) << " "
              << (kv.second ? 1 : 0) << " " << quote(c.param_text.count(kv.first)
                                                         ? c.param_text.at(kv.first)
                                                         : std::string())
              << " "
              << (c.param_db.count(kv.first) ? c.param_db.at(kv.first) : 0)
              << "\n";
    }
    for (const auto& w : wires) {
        o << "wire";
        for (auto& p : w.pts)
            o << " " << p.first << "," << p.second;
        o << "\n";
    }
    for (const auto& l : labels) {
        // Format: "netlabel ax,ay [x,y] \"name\" [fs] [rot]"
        // The display point is omitted when equal to the anchor (typical case).
        o << "netlabel " << l.anchor.first << "," << l.anchor.second;
        if (l.pt.first != l.anchor.first || l.pt.second != l.anchor.second)
            o << " " << l.pt.first << "," << l.pt.second;
        o << " " << quote(l.name) << " " << l.font_size;
        if (l.rot != 0) o << " " << l.rot;
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

        if (kw == "symcirc") {
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
                // new: fstart fstop stype npts prune par gro approx
                req.sweep.f_start_hz = std::atof(rest[0].c_str());
                req.sweep.f_stop_hz = std::atof(rest[1].c_str());
                int ty = std::atoi(rest[2].c_str());
                req.sweep.type = ty == 1 ? syms::SweepType::Octave
                                         : ty == 2 ? syms::SweepType::Linear
                                                   : syms::SweepType::Decade;
                req.sweep.points_per_interval = std::atoi(rest[3].c_str());
                req.prune = rest[4] != "0";
                req.use_parallel = rest[5] != "0";
                req.gm_ro_assume = rest[6] != "0";
                req.approx_factor = rest[7] != "0";
            } else if (rest.size() >= 5) {
                // old: f0 threshold global prune par gro approx
                req.f0_hz = std::atof(rest[0].c_str());
                req.threshold_db = std::atof(rest[1].c_str());
                req.global_ref = rest[2] != "0";
                if (rest.size() > 3) req.prune = rest[3] != "0";
                if (rest.size() > 4) req.use_parallel = rest[4] != "0";
                if (rest.size() > 5) req.gm_ro_assume = rest[5] != "0";
                if (rest.size() > 6) req.approx_factor = rest[6] != "0";
                req.sweep.f_start_hz = req.f0_hz > 0 ? req.f0_hz : 1.0;
                req.sweep.f_stop_hz = req.sweep.f_start_hz * 1e6;
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
            // K stores its two coupled-inductor refs at the end of the line
            std::string extra;
            while (next_token(line, i, extra)) c.links.push_back(extra);
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
            while (next_token(line, i, t)) {
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
            // Optional rotation (degrees, multiples of 90).
            std::string srot;
            if (next_token(line, i, srot)) l.rot = std::atoi(srot.c_str());
            labels.push_back(l);
        } else if (kw == "card") {
            // analysis card line: keep it verbatim in analysis_cards
            analysis_cards += line + "\n";
        } else {
            return fail("unknown keyword " + kw);
        }
    }
    if (!saw_header) {
        err = "not a SymCirc file (missing header)";
        return false;
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

} // namespace symcirc
