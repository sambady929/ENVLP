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
        js << ",\"mirror\":" << q(c.mirror_ref);
        js << ",\"mult\":" << c.mirror_num() << ",\"den\":" << c.mirror_dn();
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
    js << "]}";
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
// reference prefix the document will hand out.
const char* envlp_doc_palette() {
    std::ostringstream js;
    js << "{\"ok\":true,\"kinds\":[";
    bool first = true;
    for (syms::Kind k : {syms::Kind::R, syms::Kind::C, syms::Kind::L,
                         syms::Kind::V, syms::Kind::I, syms::Kind::D,
                         syms::Kind::NMOS, syms::Kind::PMOS, syms::Kind::NPN,
                         syms::Kind::PNP, syms::Kind::GND, syms::Kind::VDD,
                         syms::Kind::E, syms::Kind::G, syms::Kind::OPAMP,
                         syms::Kind::FDOPAMP, syms::Kind::AMP, syms::Kind::T,
                         syms::Kind::K, syms::Kind::NULLOR, syms::Kind::IS,
                         syms::Kind::SBLK, syms::Kind::CCCS, syms::Kind::CCVS}) {
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
            g_doc.add(c, getn(o, "x", 0), getn(o, "y", 0));
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
            if (c) c->value_text = get(o, "text");
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
            g_doc.bind_wire_ends(w);
            g_doc.wires.push_back(w);
            g_doc.sync_wire_endpoints();
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

