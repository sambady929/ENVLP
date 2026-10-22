// The schematic-editor backend, exposed to the browser.
//
// The web editor is a canvas front-end over the *same* document model the
// desktop app uses: components, wire bindings, waypoints, net naming and .scx
// serialisation all stay in C++ (envlp::Document), so there is exactly one
// implementation of the electrical rules. The page only renders the geometry
// it is handed and sends batched edits back.
//
// A single document is held (a browser tab edits one schematic at a time).
#include "app/Document.h"
#include "app/SymbolGeom.h"
#include "core/Analysis.h"
#include "core/Netlist.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

envlp::Document g_doc;
envlp::NetMap g_nets;
// True between an editor "begin"/"end" pair (a drag): the intermediate edits of
// one gesture must collapse into a single undo step, not one per mouse move.
bool g_in_batch = false;

// --- the in-app clipboard (copy / paste) -----------------------------------
//
// The desktop keeps the copied components and the wires between them in the
// canvas; the browser build keeps them here, next to the document.
struct ClipComp {
    syms::Component comp;
    envlp::Placement place;
};
struct ClipWire {
    std::vector<envlp::Pt> pts;
    envlp::WireEnd a, b;
};
std::vector<ClipComp> g_clip_comps;
std::vector<ClipWire> g_clip_wires;
// The components the engine wants selected (a paste selects what it pasted).
std::vector<std::string> g_selected;
// The wire the last wire operation produced (after any joining), so the
// front-end can keep extending the right wire.
int g_last_wire = -1;

// --- tiny JSON helpers (flat objects / arrays of flat objects) ------------
std::string unesc(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char c = s[++i];
            switch (c) {
                case 'n': o += '\n'; break;
                case 't': o += '\t'; break;
                case 'r': o += '\r'; break;
                default: o += c; break;
            }
        } else {
            o += s[i];
        }
    }
    return o;
}

// Parses an array of flat objects: [{"a":1,"b":"x"}, ...]. Values are strings,
// numbers or booleans; enough for the editor's op batches.
std::vector<std::map<std::string, std::string>> parse_ops(const std::string& j) {
    std::vector<std::map<std::string, std::string>> ops;
    size_t i = 0;
    auto ws = [&] { while (i < j.size() && std::isspace((unsigned char)j[i])) ++i; };
    ws();
    if (i >= j.size() || j[i] != '[') return ops;
    ++i;
    while (i < j.size()) {
        ws();
        if (i < j.size() && j[i] == ']') break;
        if (i >= j.size() || j[i] != '{') { ++i; continue; }
        ++i;
        std::map<std::string, std::string> o;
        while (i < j.size()) {
            ws();
            if (i < j.size() && j[i] == '}') { ++i; break; }
            if (i >= j.size() || j[i] != '"') { ++i; continue; }
            std::string k;
            ++i;
            while (i < j.size() && j[i] != '"') {
                if (j[i] == '\\' && i + 1 < j.size()) { k += j[i]; k += j[i + 1]; i += 2; continue; }
                k += j[i++];
            }
            ++i;
            ws();
            if (i < j.size() && j[i] == ':') ++i;
            ws();
            std::string v;
            if (i < j.size() && j[i] == '"') {
                ++i;
                while (i < j.size() && j[i] != '"') {
                    if (j[i] == '\\' && i + 1 < j.size()) { v += j[i]; v += j[i + 1]; i += 2; continue; }
                    v += j[i++];
                }
                ++i;
                v = unesc(v);
            } else {
                while (i < j.size() && j[i] != ',' && j[i] != '}') v += j[i++];
                while (!v.empty() && std::isspace((unsigned char)v.back())) v.pop_back();
            }
            o[unesc(k)] = v;
            ws();
            if (i < j.size() && j[i] == ',') ++i;
        }
        ops.push_back(o);
        ws();
        if (i < j.size() && j[i] == ',') ++i;
    }
    return ops;
}

std::string q(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if ((unsigned char)c < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof(b), "\\u%04x", c);
                    o += b;
                } else {
                    o += c;
                }
        }
    }
    return o + "\"";
}
std::string num(double v) {
    if (!std::isfinite(v)) return "0";
    char b[40];
    std::snprintf(b, sizeof(b), "%.6g", v);
    return b;
}
std::string get(const std::map<std::string, std::string>& m, const char* k,
                const std::string& d = "") {
    auto it = m.find(k);
    return it == m.end() ? d : it->second;
}
double getn(const std::map<std::string, std::string>& m, const char* k, double d) {
    auto it = m.find(k);
    if (it == m.end() || it->second.empty()) return d;
    try { return std::stod(it->second); } catch (...) { return d; }
}
bool getb(const std::map<std::string, std::string>& m, const char* k, bool d) {
    auto it = m.find(k);
    if (it == m.end()) return d;
    return it->second == "true" || it->second == "1";
}

const char* dup(const std::string& s) {
    char* p = (char*)std::malloc(s.size() + 1);
    std::memcpy(p, s.c_str(), s.size() + 1);
    return p;
}
const char* err_json(const std::string& msg) {
    return dup(std::string("{\"ok\":false,\"error\":") + q(msg) + "}");
}

syms::Kind kind_of(const std::string& t, bool& found) {
    syms::Kind k = syms::Kind::R;
    found = syms::kind_from_token(t, k);
    return k;
}

// --- geometry -------------------------------------------------------------
//
// Everything the renderer and the hit-tester need, in one object: components
// (with their pins and nets), wires (resolved polylines + bindings) and labels.
std::string geometry() {
    g_doc.sync_wire_endpoints();
    g_doc.sync_label_anchors();
    std::string derr;
    syms::Circuit rc = g_doc.resolved(derr);
    g_nets = g_doc.net_map();

    std::ostringstream js;
    js << "{\"ok\":true,\"dirty\":" << (g_doc.dirty ? "true" : "false");
    js << ",\"rev\":" << g_doc.undo_depth();
    js << ",\"can_undo\":" << (g_doc.can_undo() ? "true" : "false");
    js << ",\"can_redo\":" << (g_doc.can_redo() ? "true" : "false");
    js << ",\"error\":" << q(derr);

    // --- components
    js << ",\"components\":[";
    bool first = true;
    for (size_t ci = 0; ci < g_doc.circuit.comps.size(); ++ci) {
        const syms::Component& c = g_doc.circuit.comps[ci];
        auto it = g_doc.placements.find(c.ref);
        if (it == g_doc.placements.end()) continue;
        const envlp::Placement& pl = it->second;
        const syms::Component* rcp = rc.find(c.ref);
        if (!first) js << ",";
        first = false;
        js << "{\"ref\":" << q(c.ref);
        js << ",\"kind\":" << q(syms::kind_token(c.kind));
        js << ",\"label\":" << q(syms::kind_display(c.kind));
        js << ",\"x\":" << num(pl.x) << ",\"y\":" << num(pl.y);
        js << ",\"rot\":" << pl.rot;
        js << ",\"flip_h\":" << (pl.flip_h ? "true" : "false");
        js << ",\"flip_v\":" << (pl.flip_v ? "true" : "false");
        js << ",\"value\":" << q(c.value_text);
        js << ",\"dc\":" << q(c.dc_text);
        js << ",\"ac\":" << q(c.ac_text);
        // The coupled inductors of a coupling marker (K).
        js << ",\"links\":[";
        for (size_t li = 0; li < c.links.size(); ++li) {
            if (li) js << ",";
            js << q(c.links[li]);
        }
        js << "]";
        js << ",\"mirror\":" << q(c.mirror_ref);
        js << ",\"mult\":" << c.mirror_num() << ",\"den\":" << c.mirror_dn();
        // The model parameters of this kind (gm, ro, Cgs, the passives'
        // parasitics...), with the value in force and whether it is enabled.
        {
            const auto& defs = syms::param_defs(c.kind);
            js << ",\"params\":[";
            bool f2 = true;
            for (const auto& pd : defs) {
                if (!f2) js << ",";
                f2 = false;
                auto tit = c.param_text.find(pd.name);
                const std::string txt =
                    (tit != c.param_text.end() && !tit->second.empty())
                        ? tit->second
                        : pd.default_text;
                js << "{\"name\":" << q(pd.name) << ",\"text\":" << q(txt)
                   << ",\"unit\":" << q(pd.unit) << ",\"parasitic\":"
                   << (pd.parasitic ? "true" : "false") << ",\"on\":"
                   << (c.param_enabled(pd.name) ? "true" : "false") << ",\"desc\":"
                   << q(pd.desc) << "}";
            }
            js << "]";
        }
        double x0, y0, x1, y1;
        envlp::symbol_bbox(c, pl, x0, y0, x1, y1, 0.0);
        js << ",\"bbox\":[" << num(x0) << "," << num(y0) << "," << num(x1) << ","
           << num(y1) << "]";
        js << ",\"pins\":[";
        for (int pi = 0; pi < syms::pin_count(c.kind); ++pi) {
            envlp::Pt w = envlp::pin_world(c, pl, pi);
            if (pi) js << ",";
            js << "{\"i\":" << pi << ",\"x\":" << num(w.first)
               << ",\"y\":" << num(w.second) << ",\"name\":"
               << q(syms::pin_names(c.kind)[pi]) << ",\"net\":"
               << q(rcp && pi < int(rcp->nodes.size()) ? rcp->nodes[pi] : "")
               << "}";
        }
        js << "]}";
    }
    js << "]";

    // --- wires
    js << ",\"wires\":[";
    for (size_t wi = 0; wi < g_doc.wires.size(); ++wi) {
        const envlp::Wire& w = g_doc.wires[wi];
        if (wi) js << ",";
        js << "{\"i\":" << wi << ",\"pts\":[";
        for (size_t k = 0; k < w.pts.size(); ++k) {
            if (k) js << ",";
            js << "[" << num(w.pts[k].first) << "," << num(w.pts[k].second) << "]";
        }
        js << "]";
        auto end = [&](const envlp::WireEnd& e) {
            if (e.kind == envlp::WireEnd::Kind::Pin)
                js << "{\"pin\":true,\"ref\":" << q(e.ref) << ",\"index\":" << e.pin
                   << "}";
            else
                js << "{\"pin\":false}";
        };
        js << ",\"a\":";
        end(w.a);
        js << ",\"b\":";
        end(w.b);
        js << ",\"net\":" << q(g_doc.net_name_of_wire(int(wi))) << "}";
    }
    js << "]";

    // --- labels
    js << ",\"labels\":[";
    for (size_t li = 0; li < g_doc.labels.size(); ++li) {
        const envlp::NetLabel& l = g_doc.labels[li];
        if (li) js << ",";
        js << "{\"i\":" << li << ",\"name\":" << q(l.name) << ",\"x\":"
           << num(l.pt.first) << ",\"y\":" << num(l.pt.second) << ",\"ax\":"
           << num(l.anchor.first) << ",\"ay\":" << num(l.anchor.second) << "}";
    }
    js << "]";

    // --- net list (for the highlight + the label UI)
    js << ",\"nets\":[";
    {
        bool f = true;
        for (const auto& kv : g_nets.name) {
            if (!f) js << ",";
            f = false;
            js << "{\"name\":" << q(kv.second) << "}";
        }
    }
    js << "]";

    // --- the components the engine wants selected (a paste)
    js << ",\"selected\":[";
    for (size_t i = 0; i < g_selected.size(); ++i) {
        if (i) js << ",";
        js << q(g_selected[i]);
    }
    js << "]";
    // --- the wire the last wire operation produced (after any joining), so
    // the front-end can keep extending the right one.
    js << ",\"last_wire\":" << g_last_wire;
    js << "}";
    return js.str();
}

} // namespace

extern "C" {

// Open (or clear) the document from .scx text and return the geometry.
const char* envlp_doc_open(const char* scx) {
    g_doc = envlp::Document();
    if (scx && *scx) {
        std::string err;
        if (!g_doc.deserialize(std::string(scx), err) && !err.empty())
            return err_json(err);
    }
    return dup(geometry());
}

const char* envlp_doc_geometry() { return dup(geometry()); }

const char* envlp_doc_serialize() {
    return dup("{\"ok\":true,\"scx\":" + q(g_doc.serialize()) + "}");
}

    // The palette: one entry per placeable kind, with its display name and the
    // reference prefix the document will hand out. The order is the one the
    // palette shows: the passives and the transistors, then the amplifiers,
    // then the controlled sources together, then the ideal blocks.
const char* envlp_doc_palette() {
    std::ostringstream js;
    js << "{\"ok\":true,\"kinds\":[";
    bool first = true;
    for (syms::Kind k : {syms::Kind::R, syms::Kind::C, syms::Kind::L,
                         syms::Kind::V, syms::Kind::I, syms::Kind::D,
                         syms::Kind::NMOS, syms::Kind::PMOS, syms::Kind::NPN,
                         syms::Kind::PNP, syms::Kind::GND, syms::Kind::VDD,
                         syms::Kind::OPAMP, syms::Kind::OTA,
                         syms::Kind::FDOPAMP, syms::Kind::AMP, syms::Kind::GM,
                         syms::Kind::T, syms::Kind::K, syms::Kind::NULLOR,
                         syms::Kind::E, syms::Kind::G,
                         syms::Kind::CCCS, syms::Kind::CCVS,
                         syms::Kind::IS, syms::Kind::SBLK}) {
        if (!first) js << ",";
        first = false;
        js << "{\"token\":" << q(syms::kind_token(k)) << ",\"label\":"
           << q(syms::kind_display(k)) << ",\"prefix\":"
           << q(syms::ref_prefix(k)) << ",\"pins\":" << syms::pin_count(k) << "}";
    }
    js << "]}";
    return dup(js.str());
}

void envlp_doc_undo() { g_doc.undo(); }
void envlp_doc_redo() { g_doc.redo(); }

// Apply a batch of edits. One undo step per batch.
const char* envlp_doc_set(const char* ops_json) {
    std::vector<std::map<std::string, std::string>> ops =
        parse_ops(ops_json ? ops_json : "[]");
    if (ops.empty()) return dup(geometry());

    // A gesture (a drag) brackets many small edits; they are one undo step.
    bool ended = false;
    for (const auto& o : ops) {
        const std::string op = get(o, "op");
        if (op == "begin") {
            if (!g_in_batch) {
                g_doc.push_undo();
                g_in_batch = true;
            }
        } else if (op == "end") {
            g_in_batch = false;
            ended = true;   // the closing call must not push another snapshot
        }
    }
    if (!g_in_batch && !ended) g_doc.push_undo();
    std::string error;
    for (const auto& o : ops) {
        const std::string op = get(o, "op");
        if (op == "add") {
            bool ok = false;
            syms::Kind k = kind_of(get(o, "kind"), ok);
            if (!ok) { error = "unknown component kind"; continue; }
            syms::Component c;
            c.kind = k;
            c.nodes.assign(syms::pin_count(k), "");
            c.value_text = get(o, "value");
            const std::string ref = g_doc.add(c, getn(o, "x", 0), getn(o, "y", 0));
            // The orientation of the placed part (the ghost's rotation and
            // flips), so the browser can place a part the way the desktop's
            // ghost does.
            auto it = g_doc.placements.find(ref);
            if (it != g_doc.placements.end()) {
                it->second.rot = int(getn(o, "rot", 0)) % 360;
                if (it->second.rot < 0) it->second.rot += 360;
                it->second.flip_h = getb(o, "flip_h", false);
                it->second.flip_v = getb(o, "flip_v", false);
            }
        } else if (op == "remove") {
            g_doc.remove(get(o, "ref"));
        } else if (op == "move") {
            auto it = g_doc.placements.find(get(o, "ref"));
            if (it != g_doc.placements.end()) {
                it->second.x = std::round(getn(o, "x", it->second.x) / 10.0) * 10.0;
                it->second.y = std::round(getn(o, "y", it->second.y) / 10.0) * 10.0;
            }
            g_doc.sync_wire_endpoints();
        } else if (op == "nudge") {
            // relative move, used while dragging a component
            auto it = g_doc.placements.find(get(o, "ref"));
            if (it != g_doc.placements.end()) {
                it->second.x += getn(o, "dx", 0);
                it->second.y += getn(o, "dy", 0);
                it->second.x = std::round(it->second.x / 10.0) * 10.0;
                it->second.y = std::round(it->second.y / 10.0) * 10.0;
            }
            g_doc.sync_wire_endpoints();
        } else if (op == "rotate") {
            auto it = g_doc.placements.find(get(o, "ref"));
            if (it != g_doc.placements.end()) {
                int d = int(getn(o, "dir", 1));
                it->second.rot = ((it->second.rot + (d > 0 ? 90 : -90)) % 360 + 360) % 360;
            }
            g_doc.sync_wire_endpoints();
        } else if (op == "flip") {
            auto it = g_doc.placements.find(get(o, "ref"));
            if (it != g_doc.placements.end()) {
                if (get(o, "axis") == "v") it->second.flip_v = !it->second.flip_v;
                else it->second.flip_h = !it->second.flip_h;
            }
            g_doc.sync_wire_endpoints();
        } else if (op == "value") {
            syms::Component* c = nullptr;
            for (auto& cc : g_doc.circuit.comps)
                if (cc.ref == get(o, "ref")) c = &cc;
            if (c) {
                // The field: the value itself, or a source's DC / AC level.
                const std::string field = get(o, "field", "value");
                if (field == "dc") c->dc_text = get(o, "text");
                else if (field == "ac") c->ac_text = get(o, "text");
                else c->value_text = get(o, "text");
            }
        } else if (op == "mirror") {
            syms::Component* c = nullptr;
            for (auto& cc : g_doc.circuit.comps)
                if (cc.ref == get(o, "ref")) c = &cc;
            if (c) {
                c->mirror_ref = get(o, "unit");
                c->mirror_mult = std::max(1, int(getn(o, "mult", 1)));
                c->mirror_den = std::max(1, int(getn(o, "den", 1)));
            }
        } else if (op == "wire") {
            envlp::Wire w;
            envlp::Pt a{getn(o, "x0", 0), getn(o, "y0", 0)};
            envlp::Pt b{getn(o, "x1", 0), getn(o, "y1", 0)};
            // The route is orthogonal, like the desktop's: the corner is the
            // horizontal-first or the vertical-first leg, and Space swaps it.
            // The corner sits *between* the two ends.
            const bool corner = std::fabs(b.first - a.first) > 1e-9 &&
                                std::fabs(b.second - a.second) > 1e-9;
            if (corner && getb(o, "h_first", true))
                w.pts = {a, envlp::Pt{b.first, a.second}, b};
            else if (corner)
                w.pts = {a, envlp::Pt{a.first, b.second}, b};
            else
                w.pts = {a, b};
            if (get(o, "start_pin") == "true") {
                w.a.kind = envlp::WireEnd::Kind::Pin;
                w.a.ref = get(o, "start_ref");
                w.a.pin = int(getn(o, "start_index", 0));
            } else {
                w.a.kind = envlp::WireEnd::Kind::Free;
            }
            if (get(o, "end_pin") == "true") {
                w.b.kind = envlp::WireEnd::Kind::Pin;
                w.b.ref = get(o, "end_ref");
                w.b.pin = int(getn(o, "end_index", 0));
            } else {
                w.b.kind = envlp::WireEnd::Kind::Free;
            }
            if (w.pts.size() > 2)
                w.waypoints.assign(w.pts.begin() + 1, w.pts.end() - 1);
            g_doc.bind_wire_ends(w);
            g_doc.wires.push_back(w);
            g_doc.sync_wire_endpoints();
            // A wire that meets another one end to end becomes a single wire.
            g_last_wire = g_doc.join_with_neighbours(int(g_doc.wires.size()) - 1);
        } else if (op == "wire_extend") {
            // Continue the wire that is being drawn: append the routed
            // segment (the desktop extends one wire, it does not start a new
            // one for every click).
            const int wi = int(getn(o, "wire", -1));
            if (wi < 0 || wi >= int(g_doc.wires.size())) {
                error = "no such wire";
                continue;
            }
            envlp::Wire& w = g_doc.wires[wi];
            if (w.pts.empty()) continue;
            const envlp::Pt last = w.pts.back();
            const envlp::Pt b{getn(o, "x", 0), getn(o, "y", 0)};
            if (std::fabs(b.first - last.first) > 1e-9 &&
                std::fabs(b.second - last.second) > 1e-9) {
                if (getb(o, "h_first", true))
                    w.pts.push_back(envlp::Pt{b.first, last.second});
                else
                    w.pts.push_back(envlp::Pt{last.first, b.second});
            }
            w.pts.push_back(b);
            if (w.pts.size() > 2)
                w.waypoints.assign(w.pts.begin() + 1, w.pts.end() - 1);
            if (get(o, "end_pin") == "true") {
                w.b.kind = envlp::WireEnd::Kind::Pin;
                w.b.ref = get(o, "end_ref");
                w.b.pin = int(getn(o, "end_index", 0));
            }
            g_doc.bind_wire_ends(w);
            g_doc.sync_wire_endpoints();
            // Same here: the extended wire may meet another one end to end.
            g_last_wire = g_doc.join_with_neighbours(wi);
        } else if (op == "move_wire") {
            // Drag a wire (or one of its segments). A segment moves
            // perpendicular to itself, so its neighbours stretch instead of
            // the whole route going diagonal, and a pin-bound end gets a jog
            // inserted beside the pin -- the desktop's behaviour.
            const int wi = int(getn(o, "wire", -1));
            if (wi < 0 || wi >= int(g_doc.wires.size())) {
                error = "no such wire";
                continue;
            }
            const int seg = int(getn(o, "seg", -1));
            double dx = getn(o, "dx", 0);
            double dy = getn(o, "dy", 0);
            envlp::Wire& w = g_doc.wires[wi];
            const size_t npts = w.pts.size();
            if (npts < 2) continue;
            const bool segment = (seg >= 0 && seg + 1 < int(npts));
            int seg_a = -1, seg_b = -1;
            if (segment) {
                seg_a = seg;
                seg_b = seg + 1;
                const envlp::Pt& sa = w.pts[seg_a];
                const envlp::Pt& sb = w.pts[seg_b];
                const bool horizontal =
                    std::fabs(sa.second - sb.second) < std::fabs(sa.first - sb.first);
                if (horizontal) dx = 0.0;
                else dy = 0.0;
            }
            const bool a_pin = (w.a.kind == envlp::WireEnd::Kind::Pin);
            const bool b_pin = (w.b.kind == envlp::WireEnd::Kind::Pin);
            const std::vector<envlp::Pt> backup = w.pts;
            std::vector<envlp::Pt> nw;
            nw.reserve(backup.size() + 2);
            for (size_t k = 0; k < npts; ++k) {
                envlp::Pt p = backup[k];
                const bool moved_k = !segment || int(k) == seg_a || int(k) == seg_b;
                const bool pin_k = (k == 0 && a_pin) || (k + 1 == npts && b_pin);
                if (moved_k && pin_k) {
                    envlp::Pt m{p.first + dx, p.second + dy};
                    if (k == 0) {
                        nw.push_back(p);
                        nw.push_back(m);
                    } else {
                        nw.push_back(m);
                        nw.push_back(p);
                    }
                } else {
                    if (moved_k) {
                        p.first += dx;
                        p.second += dy;
                    }
                    nw.push_back(p);
                }
            }
            // Other wires that share a moved vertex follow it.
            std::map<std::pair<double, double>, envlp::Pt> moved;
            for (size_t k = 0; k < npts; ++k) {
                if (segment && int(k) != seg_a && int(k) != seg_b) continue;
                const bool pin_k = (k == 0 && a_pin) || (k + 1 == npts && b_pin);
                if (pin_k) continue;
                const envlp::Pt o = backup[k];
                if (dx != 0.0 || dy != 0.0)
                    moved[{o.first, o.second}] = envlp::Pt{o.first + dx, o.second + dy};
            }
            w.pts = nw;
            if (nw.size() > 2) w.waypoints.assign(nw.begin() + 1, nw.end() - 1);
            else w.waypoints.clear();
            for (size_t other = 0; other < g_doc.wires.size(); ++other) {
                if (int(other) == wi) continue;
                envlp::Wire& ow = g_doc.wires[other];
                bool any = false;
                for (auto& v : ow.pts) {
                    auto it = moved.find({v.first, v.second});
                    if (it != moved.end()) {
                        v = it->second;
                        any = true;
                    }
                }
                if (any) {
                    if (ow.pts.size() > 2)
                        ow.waypoints.assign(ow.pts.begin() + 1, ow.pts.end() - 1);
                    else
                        ow.waypoints.clear();
                }
            }
            g_doc.sync_label_anchors();
        } else if (op == "links") {
            // The two inductors a coupling marker ties together.
            const std::string ref = get(o, "ref");
            for (auto& c : g_doc.circuit.comps) {
                if (c.ref != ref) continue;
                c.links.clear();
                const std::string a = get(o, "a"), b = get(o, "b");
                if (!a.empty()) c.links.push_back(a);
                if (!b.empty()) c.links.push_back(b);
            }
            g_doc.dirty = true;
        } else if (op == "param") {
            // A model parameter's value (a parasitic, gm, ro, ...).
            const std::string ref = get(o, "ref");
            for (auto& c : g_doc.circuit.comps) {
                if (c.ref != ref) continue;
                c.param_text[get(o, "name")] = get(o, "text");
            }
            g_doc.dirty = true;
        } else if (op == "param_on") {
            const std::string ref = get(o, "ref");
            for (auto& c : g_doc.circuit.comps) {
                if (c.ref != ref) continue;
                c.param_on[get(o, "name")] = getb(o, "on", true);
            }
            g_doc.dirty = true;
        } else if (op == "copy") {
            // Copy the selected components (and the wires that connect only
            // them) into the clipboard.
            g_clip_comps.clear();
            g_clip_wires.clear();
            std::vector<std::string> refs;
            const std::string list = get(o, "refs");
            size_t pos = 0;
            while (pos < list.size()) {
                size_t comma = list.find(',', pos);
                if (comma == std::string::npos) comma = list.size();
                const std::string r = list.substr(pos, comma - pos);
                if (!r.empty()) refs.push_back(r);
                pos = comma + 1;
            }
            for (const auto& r : refs) {
                const syms::Component* c = g_doc.circuit.find(r);
                auto pl = g_doc.placements.find(r);
                if (!c || pl == g_doc.placements.end()) continue;
                g_clip_comps.push_back({*c, pl->second});
            }
            for (const auto& w : g_doc.wires) {
                auto pin_in = [&](const envlp::WireEnd& e) {
                    if (e.kind != envlp::WireEnd::Kind::Pin) return false;
                    for (const auto& r : refs) if (r == e.ref) return true;
                    return false;
                };
                if (pin_in(w.a) && pin_in(w.b))
                    g_clip_wires.push_back({w.pts, w.a, w.b});
            }
        } else if (op == "paste") {
            // Paste the clipboard at the cursor: new references, the wires
            // rebound to the new pins, and the mirror fields carried over.
            if (g_clip_comps.empty()) continue;
            const envlp::Pt at{getn(o, "x", 0), getn(o, "y", 0)};
            double ox = 1e300, oy = 1e300;
            for (const auto& cc : g_clip_comps) {
                ox = std::min(ox, cc.place.x);
                oy = std::min(oy, cc.place.y);
            }
            const double dx = at.first - ox, dy = at.second - oy;
            std::map<std::string, std::string> remap;
            std::vector<std::string> new_refs;
            for (const auto& cc : g_clip_comps) {
                const std::string nr =
                    g_doc.add(cc.comp, cc.place.x + dx, cc.place.y + dy);
                auto pl = g_doc.placements.find(nr);
                if (pl != g_doc.placements.end()) {
                    pl->second.rot = cc.place.rot;
                    pl->second.flip_h = cc.place.flip_h;
                    pl->second.flip_v = cc.place.flip_v;
                }
                remap[cc.comp.ref] = nr;
                new_refs.push_back(nr);
            }
            // A pasted copy of a unit points at the pasted unit, not the old one.
            for (const auto& r : new_refs) {
                for (auto& dst : g_doc.circuit.comps) {
                    if (dst.ref != r) continue;
                    if (!dst.mirror_ref.empty() && remap.count(dst.mirror_ref))
                        dst.mirror_ref = remap[dst.mirror_ref];
                }
            }
            for (const auto& cw : g_clip_wires) {
                envlp::Wire w;
                w.pts = cw.pts;
                for (auto& p : w.pts) {
                    p.first += dx;
                    p.second += dy;
                }
                w.a = cw.a;
                w.b = cw.b;
                if (w.a.kind == envlp::WireEnd::Kind::Pin && remap.count(w.a.ref))
                    w.a.ref = remap[w.a.ref];
                if (w.b.kind == envlp::WireEnd::Kind::Pin && remap.count(w.b.ref))
                    w.b.ref = remap[w.b.ref];
                g_doc.bind_wire_ends(w);
                g_doc.wires.push_back(w);
            }
            // The new components are selected, so they can be moved at once.
            g_selected.clear();
            for (const auto& r : new_refs) g_selected.push_back(r);
        } else if (op == "delete_wire") {
            g_doc.remove_wire(int(getn(o, "i", -1)));
        } else if (op == "label") {
            int wi = int(getn(o, "i", -1));
            if (wi < 0 || wi >= int(g_doc.wires.size())) {
                error = "no such wire";
                continue;
            }
            int li = g_doc.ensure_label_on_wire(wi, envlp::Pt{getn(o, "x", 0),
                                                             getn(o, "y", 0)});
            if (li >= 0 && li < int(g_doc.labels.size()))
                g_doc.labels[li].name = get(o, "name");
        } else if (op == "delete_label") {
            int li = int(getn(o, "i", -1));
            if (li >= 0 && li < int(g_doc.labels.size()))
                g_doc.labels.erase(g_doc.labels.begin() + li);
        } else if (op == "prune") {
            g_doc.prune_orphan_labels();
        } else if (op == "dirty") {
            g_doc.dirty = getb(o, "value", true);
        }
    }
    if (!error.empty()) return err_json(error);
    return dup(geometry());
}

} // extern "C"

