#include "Document.h"
#include "Symbols.h"

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
syms::Circuit Document::resolved(std::string& err) const {
    err.clear();

    // Collect anchors: pins (id -> comp/pin), wire points, label points.
    struct PinRef { int comp; int pin; Pt pt; };
    std::vector<PinRef> pins;
    for (size_t ci = 0; ci < circuit.comps.size(); ++ci) {
        const auto& c = circuit.comps[ci];
        auto pl = placements.find(c.ref);
        double ox = pl == placements.end() ? 0.0 : pl->second.x;
        double oy = pl == placements.end() ? 0.0 : pl->second.y;
        int rot = pl == placements.end() ? 0 : pl->second.rot;
        bool fh = pl != placements.end() && pl->second.flip_h;
        bool fv = pl != placements.end() && pl->second.flip_v;
        auto offs = pin_offsets(c.kind);
        for (size_t pi = 0; pi < offs.size(); ++pi) {
            Pt p = offs[pi];
            if (fh) p.first = -p.first;
            if (fv) p.second = -p.second;
            Pt rp = rotate_pt(p, rot);
            pins.push_back({int(ci), int(pi), {ox + rp.first, oy + rp.second}});
        }
    }

    // Flat point list: [0 .. nPins) pins, then wire pts, then labels.
    std::vector<Pt> pts;
    for (auto& pr : pins) pts.push_back(pr.pt);
    size_t wire_base = pts.size();
    for (const auto& w : wires)
        for (auto& p : w.pts) pts.push_back(p);
    size_t label_base = pts.size();
    for (const auto& l : labels) pts.push_back(l.pt);

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

    // Coincident points join (O(n^2), fine for schematic sizes).
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j) {
            double dx = pts[i].first - pts[j].first;
            double dy = pts[i].second - pts[j].second;
            if (dx * dx + dy * dy <= kJoinTol * kJoinTol) uf.join(i, j);
        }

    // Net naming: ground wins, then user labels, else auto n1, n2, ...
    std::map<int, std::string> net_name;
    std::vector<int> roots;
    for (int i = 0; i < n; ++i) {
        int r = uf.find(i);
        if (std::find(roots.begin(), roots.end(), r) == roots.end())
            roots.push_back(r);
    }

    auto is_gnd_name = [](std::string s) {
        for (auto& ch : s) ch = char(tolower(ch));
        return s == "0" || s == "gnd" || s == "ground";
    };
    auto is_vdd_name = [](std::string s) {
        for (auto& ch : s) ch = char(tolower(ch));
        return s == "vdd" || s == "vcc" || s == "v+";
    };

    int auto_n = 1;
    for (int r : roots) {
        std::string name;
        // any GND/VDD component pin here?
        for (size_t i = 0; i < pins.size(); ++i) {
            if (uf.find(int(i)) != r) continue;
            if (circuit.comps[pins[i].comp].kind == syms::Kind::GND) {
                name = "0";
                break;
            }
            if (circuit.comps[pins[i].comp].kind == syms::Kind::VDD) {
                name = "VDD";
                break;
            }
        }
        if (name.empty()) {
            for (size_t k = 0; k < labels.size(); ++k) {
                if (uf.find(int(label_base + int(k))) != r) {
                    if (is_gnd_name(labels[k].name)) name = "0";
                    else if (is_vdd_name(labels[k].name)) name = "VDD";
                    else name = labels[k].name;
                    break;
                }
            }
        }
        if (name.empty()) {
            do {
                name = "n" + std::to_string(auto_n++);
            } while (name == "0");
        }
        net_name[r] = name;
    }

    syms::Circuit out = circuit;
    for (auto& pr : pins) {
        int r = uf.find(int(&pr - &pins[0]));
        out.comps[pr.comp].nodes[pr.pin] = net_name[r];
    }

    // Duplicate references can't happen (next_ref), but validate anyway.
    if (!out.validate(err)) {
        syms::Circuit empty;
        return empty;
    }
    return out;
}

// ---------------------------------------------------------------------------
// serialization
// ---------------------------------------------------------------------------
std::string Document::serialize() const {
    std::ostringstream o;
    o << "symcirc 1\n";
    o << "req " << quote(req.input_ref) << " " << quote(req.output) << " "
      << req.f0_hz << " " << req.threshold_db << " "
      << (req.global_ref ? 1 : 0) << "\n";
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
    for (const auto& l : labels)
        o << "netlabel " << l.pt.first << "," << l.pt.second << " "
          << quote(l.name) << "\n";
    return o.str();
}

bool Document::deserialize(const std::string& data, std::string& err) {
    circuit.comps.clear();
    placements.clear();
    wires.clear();
    labels.clear();
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
            std::string a, b, c, d, e;
            if (!need(a) || !need(b) || !need(c) || !need(d) || !need(e))
                return fail("bad req");
            req.input_ref = a;
            req.output = b;
            req.f0_hz = std::atof(c.c_str());
            req.threshold_db = std::atof(d.c_str());
            req.global_ref = e != "0";
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
            std::string sp, name;
            if (!need(sp) || !need(name)) return fail("bad netlabel");
            NetLabel l;
            if (std::sscanf(sp.c_str(), "%lf,%lf", &l.pt.first, &l.pt.second) != 2)
                return fail("bad netlabel point");
            l.name = name;
            labels.push_back(l);
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
