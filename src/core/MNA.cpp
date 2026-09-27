#include "core/MNA.h"
#include "core/Eng.h"
#include "core/Par.h"
#include "core/Solver.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace syms {
namespace {

using GiNaC::ex;
using GiNaC::matrix;

ex reg_param(ParamTable& pt, const Component& c, const std::string& p) {
    std::string name = param_symbol(c, p);
    ex sym = pt.get(name);
    pt.set(name, c.param_estimate(p), param_unit_class(c.kind, p));
    return sym;
}

// Ideal controlled-source gains are dimensionless numbers: stamp them
// numerically so H(s) stays in terms of the real design variables. Integral
// values (including "1e5") are stamped as exact integers so downstream
// rational simplification and gcd cancellation stay exact.
ex gain_ex(const Component& c) {
    const std::string& t = c.value_text;
    bool plain = !t.empty();
    for (char ch : t)
        if (!((ch >= '0' && ch <= '9') || ch == '.' || ch == '+' ||
              ch == '-' || ch == 'e' || ch == 'E'))
            plain = false;
    double v = 0.0;
    if (plain) {
        try {
            v = std::strtod(t.c_str(), nullptr);
        } catch (...) {
            v = c.estimate();
        }
    } else {
        v = c.estimate();
    }
    if (!std::isfinite(v)) v = c.estimate();
    double ri = std::round(v);
    if (std::fabs(v - ri) < 1e-12 * (1.0 + std::fabs(v)) &&
        std::fabs(ri) < 1e15)
        return ex(static_cast<long>(ri));
    return ex(v);
}

// Amplifier gain as a *symbolic* design variable A_<ref>, with the user's
// estimate registered so the magnitude pruner can simplify A/(A+1) -> 1 when
// A = 1e9. Used for the amplifier-like blocks (op-amp, fully-differential
// op-amp, generic gain block) so a feedback expression keeps A visible and
// reduces symbolically. A negative value_text (e.g. "-100") makes the symbol
// negative while the registered estimate stays positive, so the magnitude
// pruning keeps working: the returned expression is sign * A_<ref>.
ex amp_gain(ParamTable& pt, const Component& c) {
    std::string name = "A_" + c.ref;
    ex sym = pt.get(name);
    double a = c.estimate();
    double mag = std::fabs(a);
    if (!(mag > 0.0) || !std::isfinite(mag)) mag = 1.0;
    pt.set(name, mag, UnitClass::Plain);
    return (a < 0.0) ? -sym : sym;
}

// The op-amp's gain-bandwidth product as a symbol GBW_<ref>, in *rad/s* (the
// user enters Hz, so we convert: the symbol is the unity-gain angular
// frequency). This keeps the symbolic pole A/GBW free of explicit 2*pi terms.
ex amp_gbw(ParamTable& pt, const Component& c) {
    std::string name = param_symbol(c, "GBW");
    ex sym = pt.get(name);
    pt.set(name, 2.0 * M_PI * c.param_estimate("GBW"), UnitClass::Plain);
    return sym;
}

// Add the single dominant pole implied by an op-amp's gain-bandwidth product.
// A first-order op-amp has A(s) = A0/(1 + s/w0) with w0 = GBW/A0, where GBW is
// the unity-gain angular frequency in rad/s. The VCVS KVL row `k` (whose
// output sits at node `out`) gains an extra (s/w0)*v(out) = s*A/GBW*v(out) term
// -- the same shape as a capacitor to ground. GBW is optional: when it is
// switched off (or non-positive) this is a no-op and the amplifier is ideal
// (infinite bandwidth).
void add_gbw_pole(MnaSystem& sys, const Component& c, const ex& gain,
                  const ex& gbw, int k, int out, const ex& s) {
    if (!c.param_enabled("GBW")) return;
    if (c.param_estimate("GBW") <= 0.0) return;
    if (out >= 0) sys.Y(k, out) += s * gain / gbw;
}

// --- structural series folding (low-entropy by construction) --------------
//
// Two components are in SERIES when they share a node that no other component
// touches. That is the whole test: a clean, degree-2 internal node. Whether the
// pair is then collapsed depends only on whether that intermediate node is
// USED:
//   * if nothing reads it, fold it -- the pair is one branch and becomes one
//     held atom (`R2+R3`), so a parallel partner reads `R1||(R2+R3)`;
//   * if it is used (e.g. it is the analysis output node, or a DC sweep prints
//     every node, or a tapped divider measures through it), it is NOT in series
//     for this analysis, so it is left alone and stays probeable.
//
// A T-coil's two inductors do share a node, but the bridge capacitor also
// touches it, so the node is not degree-2 and they are not in series. A
// tapped voltage divider's midpoint is degree-2 but is used -> not collapsed.
//
// Nodes are folded into chains: joining the elements at a node rebuilds the
// branch, so a three-deep chain R1+R2+R3 folds to one held sum as well.
struct HeldPassive {
    GiNaC::ex value;    // held atom: a symbol, or a ser()/par() combination
    double est = 1.0;   // numeric estimate (ranking / pruning)
    UnitClass cls = UnitClass::Ohm;
};

// Fold series pairs of same-class passives into held atoms. `used_nodes` are
// nodes the caller will probe: they are never folded. Every symbol folded away
// is registered in `params` so the magnitude pruner still knows it. Returns
// synthetic-ref -> held value; the synthetic components are appended to
// `circ.comps`.
std::map<std::string, HeldPassive> fold_series_passives(
    Circuit& circ, ParamTable& params, const std::set<std::string>& used_nodes) {
    std::map<std::string, HeldPassive> held;
    auto cls_of = [](Kind k) {
        return k == Kind::R   ? UnitClass::Ohm
               : k == Kind::C ? UnitClass::Farad
                              : UnitClass::Henry;
    };
    auto value_of = [&](const Component& c) -> GiNaC::ex {
        auto it = held.find(c.ref);
        return it != held.end() ? it->second.value : params.get(c.ref);
    };
    auto est_of = [&](const Component& c) -> double {
        auto it = held.find(c.ref);
        return it != held.end() ? it->second.est : c.estimate();
    };
    // A node the caller reads: never fold it away.
    auto is_used = [&](const std::string& n) {
        return used_nodes.count(n) != 0;
    };

    int synth = 0;
    for (bool changed = true; changed;) {
        changed = false;
        // Which R/C/L branches touch each node; and the total degree of each
        // node (every component terminal, so a device pin or a source counts).
        std::map<std::string, std::vector<int>> at;
        std::map<std::string, int> degree;
        for (size_t i = 0; i < circ.comps.size(); ++i) {
            const Component& c = circ.comps[i];
            if (c.kind == Kind::R || c.kind == Kind::C || c.kind == Kind::L) {
                at[c.nodes[0]].push_back(int(i));
                if (c.nodes[1] != c.nodes[0]) at[c.nodes[1]].push_back(int(i));
            }
            for (const auto& n : c.nodes) ++degree[n];
            if ((c.kind == Kind::NPN || c.kind == Kind::PNP) &&
                c.param_enabled("rb"))
                ++degree[bjt_internal_node(c)];
        }
        for (const auto& kv : at) {
            const std::string& m = kv.first;
            if (m == "0" || m == "GND") continue;   // ground is a terminal
            if (is_used(m)) continue;               // probed: keep it real
            if (degree.count(m) && degree[m] != 2) continue; // not a clean node
            const auto& inc = kv.second;
            if (inc.size() != 2) continue;
            int i = inc[0], j = inc[1];
            if (i == j) continue;
            Component a = circ.comps[i];
            Component b = circ.comps[j];
            if (a.kind != b.kind) continue;         // same class only
            std::string oa = (a.nodes[0] == m) ? a.nodes[1] : a.nodes[0];
            std::string ob = (b.nodes[0] == m) ? b.nodes[1] : b.nodes[0];
            if (oa == ob) continue;                 // would be a shunt

            UnitClass cls = cls_of(a.kind);
            GiNaC::ex x = value_of(a), y = value_of(b);
            double ex_ = est_of(a), ey = est_of(b);
            GiNaC::ex hv;
            double hest;
            if (cls == UnitClass::Farad) {
                // capacitors in series combine as a parallel product
                hv = par_ex(x, y);
                hest = (ex_ * ey) / (ex_ + ey);
            } else {
                // resistors / inductors in series add: a held sum
                hv = make_series({x, y});
                hest = ex_ + ey;
            }
            // Register every folded-away original so pruning still sees it.
            params.set(a.ref, ex_, cls_of(a.kind));
            params.set(b.ref, ey, cls_of(b.kind));

            Component s;
            s.kind = a.kind;
            s.ref = "__FOLD" + std::to_string(synth++);
            s.nodes = {oa, ob};
            circ.comps.push_back(s);
            held[s.ref] = HeldPassive{hv, hest, cls};
            circ.comps.erase(circ.comps.begin() + std::max(i, j));
            circ.comps.erase(circ.comps.begin() + std::min(i, j));
            changed = true;
            break; // indices changed: rescan
        }
    }
    return held;
}

} // namespace

MnaSystem build_mna(const Circuit& cin, const std::string& input_ref) {
    return build_mna(cin, input_ref, std::set<std::string>{});
}

MnaSystem build_mna(const Circuit& cin, const std::string& input_ref,
                    const std::set<std::string>& used_nodes) {
    return build_mna(cin, input_ref, used_nodes, nullptr);
}

MnaSystem build_mna(const Circuit& cin, const std::string& input_ref,
                    const std::set<std::string>& used_nodes,
                    const MnaDcSupply* dc_supply) {
    // Resolve mirror copies into concrete scaled parameters first, so the MNA
    // stamp and the pruner both see the materialised device model.
    Circuit circ = cin;
    resolve_mirrors(circ);

    std::string err;
    if (!circ.validate(err)) throw std::runtime_error(err);

    const Component* in = circ.find(input_ref);
    if (!in && !input_ref.empty())
        throw std::runtime_error("input source '" + input_ref + "' not found");
    if (in && !is_independent_source(in->kind))
        throw std::runtime_error("input must be an ideal source (V or I), got " +
                                 in->ref + " (" + kind_display(in->kind) + ")");

    MnaSystem sys;
    ex s = sys.params.get("s");
    const bool dc_mode = dc_supply != nullptr;
    if (dc_mode) {
        // The threshold voltage is a universal DC tech value, registered as a
        // plain symbol so it appears symbolically in the operating point.
        sys.params.set("Vth", dc_supply->vth, UnitClass::Volt);
    }
    // DC source value as a SYMBOL (the source's own name: V1, VDD, I1), so the
    // operating point reads as a design equation. The numeric DC value the user
    // entered is registered as the symbol's estimate, used for pruning and for
    // numeric evaluation. (In AC/TF mode the selected input is a numeric unit
    // drive instead, so this is DC-only.)
    auto dc_source_symbol = [&](const Component& c, const std::string& est_text) {
        double est = 0.0;
        if (!eng::parse_value(est_text, est)) est = 0.0;
        sys.params.set(c.ref, est, UnitClass::Volt);
        return sys.params.get(c.ref);
    };

    // Fold genuine series groups of same-class passives into held atoms BEFORE
    // anything is stamped (low-entropy by construction: R2+R3 becomes a held
    // sum, so the parallel deferral below combines R1 with it and prints
    // R1||(R2+R3) without ever expanding the pair). Nodes the caller probes are
    // protected so they stay real, resolvable unknowns.
    std::map<std::string, HeldPassive> held =
        fold_series_passives(circ, sys.params, used_nodes);
    auto held_of = [&](const Component& c) -> const HeldPassive* {
        auto it = held.find(c.ref);
        return it == held.end() ? nullptr : &it->second;
    };

    // --- collect node names (ground = "0"; "GND" is normalized to "0") ---
    std::vector<std::string> nodes;
    std::set<std::string> seen;
    auto add_node = [&](const std::string& raw) {
        std::string nd = (raw == "GND") ? "0" : (raw == "VDD") ? "VDD" : raw;
        if (seen.insert(nd).second) nodes.push_back(nd);
    };
    for (const auto& c : circ.comps) {
        for (const auto& n : c.nodes) add_node(n);
        if ((c.kind == Kind::NPN || c.kind == Kind::PNP) && c.param_enabled("rb"))
            add_node(bjt_internal_node(c));
    }
    if (!seen.count("0"))
        throw std::runtime_error(
            "no ground reference (place a Ground symbol or name a node \"0\")");

    // A VDD rail that is not connected to anything else is electrically
    // dangling; tie it to ground so analyses other than PSR still work.
    {
        std::map<std::string, int> uses;
        for (const auto& c : circ.comps)
            for (const auto& n : c.nodes)
                if (n == "VDD") ++uses["VDD"];
        // one use = the VDD symbol itself
        if (uses["VDD"] <= 1) {
            std::vector<std::string> keep;
            for (const auto& nd : nodes)
                if (nd != "VDD") keep.push_back(nd);
            nodes.swap(keep);
        }
    }

    // --- unknown ordering: node voltages, then branch currents ----------
    for (const auto& nd : nodes) {
        if (nd == "0") continue;
        sys.node_idx[nd] = sys.n;
        sys.var_names.push_back("v(" + nd + ")");
        ++sys.n;
    }
    for (const auto& c : circ.comps) {
        if (has_branch_current(c.kind)) {
            std::string base = (c.kind == Kind::L) ? "L:" : "";
            for (const auto& sfx : branch_suffixes(c.kind)) {
                std::string key = branch_key(c.ref, sfx);
                sys.branch_idx[key] = sys.n;
                sys.var_names.push_back("i(" + base + key + ")");
                ++sys.n;
            }
        }
        // Large-signal DC (Mode 1): each MOSFET gets an extra unknown -- its
        // drain current Id -- plus the row that ties Vgs to that current. In
        // the square-law mode Id is an explicit symbolic current, so no unknown
        // is needed.
        if (dc_mode && !dc_supply->square_law &&
            (c.kind == Kind::NMOS || c.kind == Kind::PMOS)) {
            sys.branch_idx["Id:" + c.ref] = sys.n;
            sys.var_names.push_back("Id(" + c.ref + ")");
            ++sys.n;
        }
    }
    if (sys.n == 0) throw std::runtime_error("circuit has no unknowns");

    sys.Y = matrix(sys.n, sys.n);
    sys.b = matrix(sys.n, 1);

    auto idx = [&](const std::string& raw) -> int {
        std::string key = (raw == "GND") ? "0" : (raw == "VDD") ? "VDD" : raw;
        if (key == "0") return -1;
        auto it = sys.node_idx.find(key);
        return it == sys.node_idx.end() ? -1 : it->second;
    };

    // ---- structural parallel-conductance combination (low-entropy) --------
    // Every passive *conductance* between the same pair of nodes is deferred
    // and combined into one held parallel atom before solving, so the MNA
    // result is already in terms of (R1||ro) and never has to recover it from
    // an expanded polynomial afterwards. A conductance 1/R is recognised by
    // the `R` symbol's class, so this covers both standalone resistors and a
    // device's output resistance (ro) / rpi / rd alike.
    //
    // Nodes are canonicalised so a rail held at AC ground by an ideal source
    // (other than the driven input) counts as node "0": that is what makes a
    // resistor to VDD and an output resistance to ground genuinely parallel.
    std::set<std::string> acg;
    acg.insert("0");
    for (bool ch = true; ch;) {
        ch = false;
        auto add = [&](const std::string& n) {
            std::string k = (n == "GND") ? "0" : n;
            if (acg.insert(k).second) ch = true;
        };
        for (const auto& c : circ.comps) {
            if (c.kind == Kind::V && c.ref != input_ref) {
                bool a = acg.count(c.nodes[0] == "GND" ? "0" : c.nodes[0]) != 0;
                bool b = acg.count(c.nodes[1] == "GND" ? "0" : c.nodes[1]) != 0;
                if (a) add(c.nodes[1]);
                if (b) add(c.nodes[0]);
            } else if (c.kind == Kind::VDD && c.ref != input_ref) {
                add(c.nodes[0]);
            }
        }
    }
    auto acg_rank = [&](int i) -> long long {
        // rank an idx: ground (-1) is 0; nodes are 1..n
        return i < 0 ? 0 : (long long)i + 1;
    };
    struct Deferred {
        ex sum;                 // accumulated admittance (held, not expanded)
        std::vector<ex> res_syms; // resistor symbols, if every term is 1/R
        bool all_res = true;    // every contribution so far is a pure 1/R
    };
    std::map<std::pair<long long, long long>, Deferred> def;
    // Try to read `y` as 1/R for an Ohm-class *resistance expression*: a bare
    // Ohm symbol, or a held series sum of Ohm symbols, `1/(R2+R3)`, produced by
    // the structural series fold. On success set `rsym` to that resistance.
    auto ohm_resistance = [&](ex base) -> bool {
        auto is_ohm_sym = [&](const ex& e) {
            if (!GiNaC::is_a<GiNaC::symbol>(e)) return false;
            for (const auto& kv : sys.params.syms)
                if (e.is_equal(kv.second)) {
                    auto c = sys.params.cls.find(kv.first);
                    return c != sys.params.cls.end() &&
                           c->second == UnitClass::Ohm;
                }
            return false;
        };
        if (is_ohm_sym(base)) return true;
        if (is_series(base)) {
            for (const ex& a : series_args(base))
                if (!is_ohm_sym(a)) return false;
            return true;
        }
        return false;
    };
    auto as_conductance_of = [&](const ex& y, ex& rsym) -> bool {
        ex base = y;
        if (GiNaC::is_a<GiNaC::power>(base)) {
            const ex& e = base.op(1);
            if (!(GiNaC::is_a<GiNaC::numeric>(e) &&
                  GiNaC::ex_to<GiNaC::numeric>(e).is_equal(
                      GiNaC::numeric(-1))))
                return false;
            base = base.op(0);
        }
        if (!ohm_resistance(base)) return false;
        rsym = base;
        return true;
    };
    auto defer_adm = [&](int a, int b2, const ex& y) {
        long long ra = acg_rank(a), rb = acg_rank(b2);
        if (ra == rb) return;              // shunt to the same node: ignore
        auto key = ra < rb ? std::make_pair(ra, rb)
                           : std::make_pair(rb, ra);
        auto& d = def[key];
        d.sum += y;
        ex rsym;
        if (d.all_res && as_conductance_of(y, rsym))
            d.res_syms.push_back(rsym);
        else
            d.all_res = false;
        (void)rsym;
    };

    // --- generic stamps --------------------------------------------------
    // A *pure* admittance (1/R: no explicit s) is deferred so that every
    // resistance between the same node pair is combined into one low-entropy
    // atom; anything with s (a capacitance) is stamped directly.
    auto adm_has_s = [&](const ex& y) { return y.has(s); };
    auto stamp_adm = [&](int a, int bb, const ex& y) {
        if (!adm_has_s(y)) { defer_adm(a, bb, y); return; }
        if (a >= 0) {
            sys.Y(a, a) += y;
            if (bb >= 0) sys.Y(a, bb) -= y;
        }
        if (bb >= 0) {
            sys.Y(bb, bb) += y;
            if (a >= 0) sys.Y(bb, a) -= y;
        }
    };
    auto stamp_cap = [&](int a, int bb, const ex& cap) { stamp_adm(a, bb, s * cap); };
    // current g*(v(cp)-v(cn)) flows internally from op into on
    auto stamp_vccs = [&](int op, int on, int cp, int cn, const ex& g) {
        if (op >= 0) {
            if (cp >= 0) sys.Y(op, cp) += g;
            if (cn >= 0) sys.Y(op, cn) -= g;
        }
        if (on >= 0) {
            if (cp >= 0) sys.Y(on, cp) -= g;
            if (cn >= 0) sys.Y(on, cn) += g;
        }
    };
    // branch k with current i flowing from node a into the branch and out at
    // node bb: stamp the KCL column and the KVL row (op is the branch output).
    auto stamp_branch = [&](int a, int bb, int k) {
        if (a >= 0) sys.Y(a, k) += 1;
        if (bb >= 0) sys.Y(bb, k) -= 1;
        if (a >= 0) sys.Y(k, a) += 1;
        if (bb >= 0) sys.Y(k, bb) -= 1;
    };

    // --- component stamps ------------------------------------------------
    for (const auto& c : circ.comps) {
        const auto& nd = c.nodes;
        switch (c.kind) {
        case Kind::R: {
            if (const HeldPassive* hp = held_of(c)) {
                // A synthetic fold: stamp its held resistance/admittance.
                sys.params.set(c.ref, hp->est, hp->cls);
                ex r = hp->value;
                stamp_adm(idx(nd[0]), idx(nd[1]), ex(1) / r);
            } else {
                sys.params.set(c.ref, c.estimate(), UnitClass::Ohm);
                ex r = sys.params.get(c.ref);
                stamp_adm(idx(nd[0]), idx(nd[1]), ex(1) / r);
            }
            break;
        }
        case Kind::C: {
            if (dc_mode) {
                // At DC a capacitor is an open circuit: stamp nothing, but
                // still register its symbol so reports can name it.
                sys.params.set(c.ref, c.estimate(), UnitClass::Farad);
                break;
            }
            if (const HeldPassive* hp = held_of(c)) {
                // A synthetic series-capacitor fold: held capacitance value.
                sys.params.set(c.ref, hp->est, hp->cls);
                stamp_cap(idx(nd[0]), idx(nd[1]), hp->value);
            } else {
                sys.params.set(c.ref, c.estimate(), UnitClass::Farad);
                ex cap = sys.params.get(c.ref);
                stamp_cap(idx(nd[0]), idx(nd[1]), cap);
            }
            break;
        }
        case Kind::L: {
            const HeldPassive* hp = held_of(c);
            ex l = hp ? hp->value : sys.params.get(c.ref);
            if (hp) sys.params.set(c.ref, hp->est, hp->cls);
            else sys.params.set(c.ref, c.estimate(), UnitClass::Henry);
            int a = idx(nd[0]), bb = idx(nd[1]);
            int k = sys.branch_idx.at(c.ref);
            if (a >= 0) {
                sys.Y(a, k) += 1;
                sys.Y(k, a) += 1;
            }
            if (bb >= 0) {
                sys.Y(bb, k) -= 1;
                sys.Y(k, bb) -= 1;
            }
            // At DC an inductor is a short: no s*L self term (a 0 V branch).
            if (!dc_mode) sys.Y(k, k) -= s * l;
            break;
        }
        case Kind::V: {
            int a = idx(nd[0]), bb = idx(nd[1]);
            int k = sys.branch_idx.at(c.ref);
            if (a >= 0) {
                sys.Y(a, k) += 1;
                sys.Y(k, a) += 1;
            }
            if (bb >= 0) {
                sys.Y(bb, k) -= 1;
                sys.Y(k, bb) -= 1;
            }
            // DC bias solve: the branch voltage is the source's DC symbol
            // (V1, VDD, ...). Otherwise: the selected input is driven per-unit
            // (transfer function) and every other source is zeroed (AC).
            sys.b(k, 0) = dc_mode ? dc_source_symbol(c, c.dc_text)
                                  : ((c.ref == input_ref) ? ex(1) : ex(0));
            break;
        }
        case Kind::I: {
            ex drive = dc_mode ? dc_source_symbol(c, c.dc_text)
                               : ((c.ref == input_ref) ? ex(1) : ex(0));
            int a = idx(nd[0]), bb = idx(nd[1]);
            // SPICE independent-current-source convention: positive current
            // flows from n+ (pin 0, drawn at the top) through the source to n-
            // (pin 1, the tail). Node a therefore *loses* `drive` and node bb
            // *gains* it.
            if (a >= 0) sys.b(a, 0) -= drive;
            if (bb >= 0) sys.b(bb, 0) += drive;
            break;
        }
        case Kind::E: {
            // voltage-controlled voltage source: v(op)-v(on) = gain*(v(cp)-v(cn))
            ex gain = gain_ex(c);
            int op = idx(nd[0]), on = idx(nd[1]), cp = idx(nd[2]), cn = idx(nd[3]);
            int k = sys.branch_idx.at(c.ref);
            if (op >= 0) {
                sys.Y(op, k) += 1;
                sys.Y(k, op) += 1;
            }
            if (on >= 0) {
                sys.Y(on, k) -= 1;
                sys.Y(k, on) -= 1;
            }
            if (cp >= 0) sys.Y(k, cp) -= gain;
            if (cn >= 0) sys.Y(k, cn) += gain;
            break;
        }
        case Kind::G: {
            ex g = gain_ex(c);
            stamp_vccs(idx(nd[0]), idx(nd[1]), idx(nd[2]), idx(nd[3]), g);
            break;
        }
        case Kind::NMOS:
        case Kind::PMOS: {
            int D = idx(nd[0]), G = idx(nd[1]), S = idx(nd[2]);
            // register every model parameter (enabled or not) so the symbol
            // table always exposes the full model; only enabled ones stamp MNA
            ex gm = reg_param(sys.params, c, "gm");
            ex ro = reg_param(sys.params, c, "ro");
            ex cgs = reg_param(sys.params, c, "Cgs");
            ex cgd = reg_param(sys.params, c, "Cgd");
            ex cds = reg_param(sys.params, c, "Cds");
            ex cdb = reg_param(sys.params, c, "Cdb");
            ex csb = reg_param(sys.params, c, "Csb");
            // Vth is a process value shared by every device (the DC tech
            // settings), so it is NOT a per-device parameter symbol.
            ex vth = sys.params.get("Vth");
            if (dc_mode && dc_supply->square_law) {
                // Mode 2: symbolic square law. The device's overdrive is a
                // *symbolic design variable* Vov_<ref> (not solved), so the
                // saturation drain current
                //     Id = 1/2 * uCox * (W/L) * Vov^2
                // is a known symbolic current source D->S, with W_<ref>/L_<ref>
                // kept symbolic. The gate bias is whatever the circuit drives;
                // saturation is assumed (the caller reports consistency).
                const int sgn = (c.kind == Kind::NMOS) ? +1 : -1;
                std::string vov_name = "Vov_" + c.ref;
                std::string w_name = "W_" + c.ref;
                std::string l_name = "L_" + c.ref;
                sys.params.set(vov_name, 0.2, UnitClass::Volt);
                sys.params.set(w_name, c.param_estimate("W"), UnitClass::Plain);
                sys.params.set(l_name, c.param_estimate("L"), UnitClass::Plain);
                std::string ucox_name = (c.kind == Kind::NMOS) ? "unCox" : "upCox";
                sys.params.set(ucox_name,
                               (c.kind == Kind::NMOS) ? dc_supply->uncox
                                                      : dc_supply->upcox,
                               UnitClass::Plain);
                ex vov = sys.params.get(vov_name);
                ex wsym = sys.params.get(w_name);
                ex lsym = sys.params.get(l_name);
                ex ucox = sys.params.get(ucox_name);
                ex id = (ucox * wsym * vov * vov / (2 * lsym)).normal();
                // Inject Id from D to S (sign flipped for PMOS).
                if (D >= 0) sys.b(D, 0) += sgn * id;
                if (S >= 0) sys.b(S, 0) -= sgn * id;
                if (c.param_enabled("ro")) stamp_adm(D, S, ex(1) / ro);
            } else if (dc_mode) {
                // Mode 1: Id is the unknown drain-to-source current (positive
                // into D, out of S; negative for a PMOS in normal operation).
                // With |Vdsat| = 2|Id|/gm and |Vgs| = |Vdsat| + |Vth|, the gate
                // relation is the same row for both polarities:
                //     (v(G) - v(S)) - (2/gm)*Id = sgn*|Vth|
                // with sgn = +1 (NMOS) / -1 (PMOS). Id flows D->S as a plain
                // current source; channel-length modulation adds 1/ro D->S.
                const int sgn = (c.kind == Kind::NMOS) ? +1 : -1;
                int k = sys.branch_idx.at("Id:" + c.ref);
                if (D >= 0) sys.Y(D, k) += 1;
                if (S >= 0) sys.Y(S, k) -= 1;
                if (G >= 0) sys.Y(k, G) += 1;
                if (S >= 0) sys.Y(k, S) -= 1;
                sys.Y(k, k) -= 2 / gm;
                sys.b(k, 0) = sgn * vth;
                if (c.param_enabled("ro")) stamp_adm(D, S, ex(1) / ro);
            } else {
                // no body terminal: the body is tied to the source
                stamp_vccs(D, S, G, S, gm);
                if (c.param_enabled("ro")) stamp_adm(D, S, ex(1) / ro);
                // Device parasitic capacitances are open at DC (they are
                // stamped only in the small-signal/AC model).
                if (!dc_mode) {
                    if (c.param_enabled("Cgs")) stamp_cap(G, S, cgs);
                    if (c.param_enabled("Cgd")) stamp_cap(G, D, cgd);
                    if (c.param_enabled("Cds")) stamp_cap(D, S, cds);
                    if (c.param_enabled("Cdb")) stamp_cap(D, S, cdb);
                    if (c.param_enabled("Csb")) stamp_cap(S, S, csb);
                }
            }
            break;
        }
        case Kind::NPN:
        case Kind::PNP: {
            int C = idx(nd[0]), B = idx(nd[1]), E = idx(nd[2]);
            // register every model parameter (enabled or not); only enabled
            // ones are stamped into the MNA matrix
            ex gm = reg_param(sys.params, c, "gm");
            ex rpi = reg_param(sys.params, c, "rpi");
            ex rb = reg_param(sys.params, c, "rb");
            ex ro = reg_param(sys.params, c, "ro");
            ex cpi = reg_param(sys.params, c, "Cpi");
            ex cmu = reg_param(sys.params, c, "Cmu");
            int Bi = B;
            if (c.param_enabled("rb")) {
                Bi = sys.node_idx.at(bjt_internal_node(c));
                stamp_adm(B, Bi, ex(1) / rb);
            }
            stamp_vccs(C, E, Bi, E, gm);
            if (c.param_enabled("rpi")) stamp_adm(Bi, E, ex(1) / rpi);
            if (c.param_enabled("ro")) stamp_adm(C, E, ex(1) / ro);
            // BJT junction capacitances are open at DC.
            if (!dc_mode) {
                if (c.param_enabled("Cpi")) stamp_cap(Bi, E, cpi);
                if (c.param_enabled("Cmu")) stamp_cap(Bi, C, cmu);
            }
            break;
        }
        case Kind::GND:
            break;
        case Kind::VDD: {
            // VDD is an ideal DC voltage source from the supply rail to
            // ground: the symbol's single pin is the + terminal, the implicit
            // - terminal is node 0. Like Kind::V, this stamps as an
            // independent voltage source with no series impedance.
            int a = idx(nd[0]);
            int bb = idx("0");          // - terminal: always ground
            int k = sys.branch_idx.at(c.ref);
            if (a >= 0) {
                sys.Y(a, k) += 1;
                sys.Y(k, a) += 1;
            }
            if (bb >= 0) {
                sys.Y(bb, k) -= 1;
                sys.Y(k, bb) -= 1;
            }
            // Register the supply value as a parameter so reports can name it.
            // In the large-signal DC solve the rail is driven at its supply
            // voltage symbol (VDD); otherwise it is a per-unit AC excitation
            // only when it is the selected input, and zero otherwise.
            if (dc_mode) {
                sys.b(k, 0) = dc_source_symbol(c, c.value_text);
            } else {
                sys.params.set(c.ref, c.estimate(), UnitClass::Volt);
                sys.b(k, 0) = (c.ref == input_ref) ? ex(1) : ex(0);
            }
            break;
        }
        case Kind::D: {
            // small-signal diode: gm(A->K), optional rd in series, Cd
            int A = idx(nd[0]), Kk = idx(nd[1]);
            ex gm = reg_param(sys.params, c, "gm");
            ex rd = reg_param(sys.params, c, "rd");
            ex cd = reg_param(sys.params, c, "Cd");
            stamp_vccs(A, Kk, A, Kk, gm);
            if (c.param_enabled("rd")) stamp_adm(A, Kk, ex(1) / rd);
            if (!dc_mode && c.param_enabled("Cd")) stamp_cap(A, Kk, cd);
            break;
        }
        case Kind::CCCS: {
            // current-controlled current source: sensing branch k at the
            // shorted input port, output current = gain * i_ctrl
            ex gain = gain_ex(c);
            int cp = idx(nd[0]), cn = idx(nd[1]);
            int op = idx(nd[2]), on = idx(nd[3]);
            int k = sys.branch_idx.at(c.ref);
            // input port is a short: v(ctrl+) - v(ctrl-) = 0
            if (cp >= 0) sys.Y(k, cp) += 1;
            if (cn >= 0) sys.Y(k, cn) -= 1;
            // i_ctrl flows in at ctrl+, out at ctrl-
            if (cp >= 0) sys.Y(cp, k) += 1;
            if (cn >= 0) sys.Y(cn, k) -= 1;
            // output current gain * i_ctrl flows in at out+, out at out-
            if (op >= 0) sys.Y(op, k) += gain;
            if (on >= 0) sys.Y(on, k) -= gain;
            break;
        }
        case Kind::CCVS: {
            // current-controlled voltage source: v(out+) - v(out-) = gain*i_ctrl
            ex gain = gain_ex(c);
            int cp = idx(nd[0]), cn = idx(nd[1]);
            int op = idx(nd[2]), on = idx(nd[3]);
            int k = sys.branch_idx.at(c.ref);
            if (cp >= 0) sys.Y(k, cp) += 1; // input short
            if (cn >= 0) sys.Y(k, cn) -= 1;
            if (op >= 0) {
                sys.Y(op, k) += 1;
                sys.Y(k, op) += 1;
            }
            if (on >= 0) {
                sys.Y(on, k) -= 1;
                sys.Y(k, on) -= 1;
            }
            if (cp >= 0) sys.Y(k, k) -= gain; // -gain * i_ctrl
            break;
        }
        case Kind::IS: {
            // ideal 1/s block: branch current i flows in at `in`, out at `out`,
            // with s*i = v(in) - v(out)  (integrator into a 1 ohm sense port)
            int a = idx(nd[0]), bb = idx(nd[1]);
            int k = sys.branch_idx.at(c.ref);
            stamp_branch(a, bb, k);
            sys.Y(k, k) -= s;
            break;
        }
        case Kind::SBLK: {
            // ideal s block: i = s*(v(in) - v(out))
            int a = idx(nd[0]), bb = idx(nd[1]);
            int k = sys.branch_idx.at(c.ref);
            stamp_branch(a, bb, k);
            sys.Y(k, k) -= s;
            break;
        }
        case Kind::AMP: {
            // single-ended voltage-gain block: v(out) = A*v(in), A symbolic and
            // signed (so the value can be +100 or -100). The GBW pole adds
            // (s*A/GBW)*v(out), giving A(s) = A/(1 + s*A/GBW).
            ex gain = amp_gain(sys.params, c);
            ex gbw = amp_gbw(sys.params, c);
            int a = idx(nd[0]), o = idx(nd[1]);
            int k = sys.branch_idx.at(c.ref);
            stamp_branch(o, -1, k);
            if (a >= 0) sys.Y(k, a) -= gain;
            add_gbw_pole(sys, c, gain, gbw, k, o, s);
            break;
        }
        case Kind::OPAMP: {
            // finite-gain op-amp: v(out) = A*(v(in+) - v(in-)); A symbolic.
            ex gain = amp_gain(sys.params, c);
            ex gbw = amp_gbw(sys.params, c);
            int ip = idx(nd[0]), im = idx(nd[1]), o = idx(nd[2]);
            int k = sys.branch_idx.at(c.ref);
            stamp_branch(o, -1, k);
            if (im >= 0) sys.Y(k, im) += gain;
            if (ip >= 0) sys.Y(k, ip) -= gain;
            add_gbw_pole(sys, c, gain, gbw, k, o, s);
            break;
        }
        case Kind::NULLOR: {
            // Ideal nullor as a two-port: a nullator across the INPUT port
            // (zero voltage, zero current) and a norator across the OUTPUT port
            // (its current is whatever the circuit demands). One extra unknown,
            // the norator current k (into out+, out of out-), and one
            // constraint row v(in+) - v(in-) = 0.
            int ip = idx(nd[0]), im = idx(nd[1]);
            int op = idx(nd[2]), on = idx(nd[3]);
            int k = sys.branch_idx.at(c.ref);
            if (op >= 0) sys.Y(op, k) += 1;
            if (on >= 0) sys.Y(on, k) -= 1;
            if (ip >= 0) sys.Y(k, ip) += 1;
            if (im >= 0) sys.Y(k, im) -= 1;
            break;
        }
        case Kind::FDOPAMP: {
            // fully differential: v(out+)-v(out-) = A*(v(in+)-v(in-)); A symbolic.
            ex gain = amp_gain(sys.params, c);
            ex gbw = amp_gbw(sys.params, c);
            int ip = idx(nd[0]), im = idx(nd[1]);
            int op = idx(nd[2]), on = idx(nd[3]);
            int kp = sys.branch_idx.at(branch_key(c.ref, "p"));
            int kn = sys.branch_idx.at(branch_key(c.ref, "n"));
            stamp_branch(op, -1, kp);
            stamp_branch(on, -1, kn);
            if (im >= 0) {
                sys.Y(kp, im) += gain;
                sys.Y(kn, im) += gain;
            }
            if (ip >= 0) {
                sys.Y(kp, ip) -= gain;
                sys.Y(kn, ip) -= gain;
            }
            // v(out+) - v(out-) constraint (row kp already couples op-on)
            if (on >= 0) sys.Y(kp, on) -= 1;
            // GBW: add the dominant pole (s/w0)*(v(op) - v(on)) to the kp row.
            if (c.param_enabled("GBW") && c.param_estimate("GBW") > 0.0) {
                if (op >= 0) sys.Y(kp, op) += s * gain / (2.0 * M_PI * gbw);
                if (on >= 0) sys.Y(kp, on) -= s * gain / (2.0 * M_PI * gbw);
            }
            break;
        }
        case Kind::T: {
            ex lp = reg_param(sys.params, c, "Lp");
            ex ls = reg_param(sys.params, c, "Ls");
            ex kk = reg_param(sys.params, c, "k");
            int pp = idx(nd[0]), pn = idx(nd[1]);
            int sp = idx(nd[2]), sn = idx(nd[3]);
            int kp = sys.branch_idx.at(branch_key(c.ref, "p"));
            int kn = sys.branch_idx.at(branch_key(c.ref, "n"));
            // KCL columns of both windings
            if (pp >= 0) {
                sys.Y(pp, kp) += 1;
                sys.Y(pp, kn) += 1;
            }
            if (pn >= 0) {
                sys.Y(pn, kp) -= 1;
                sys.Y(pn, kn) -= 1;
            }
            if (sp >= 0) {
                sys.Y(sp, kp) -= 1;
                sys.Y(sp, kn) -= 1;
            }
            if (sn >= 0) {
                sys.Y(sn, kp) += 1;
                sys.Y(sn, kn) += 1;
            }
            // KVL rows for the two windings
            if (pp >= 0) {
                sys.Y(kp, pp) += 1;
                sys.Y(kn, pp) -= 1;
            }
            if (pn >= 0) {
                sys.Y(kp, pn) -= 1;
                sys.Y(kn, pn) += 1;
            }
            if (sp >= 0) {
                sys.Y(kp, sp) -= 1;
                sys.Y(kn, sp) += 1;
            }
            if (sn >= 0) {
                sys.Y(kp, sn) += 1;
                sys.Y(kn, sn) -= 1;
            }
            ex m = kk * GiNaC::sqrt((lp * ls).expand());
            sys.Y(kp, kp) -= s * lp;
            sys.Y(kn, kn) -= s * ls;
            sys.Y(kp, kn) -= s * m;
            sys.Y(kn, kp) -= s * m;
            break;
        }
        case Kind::K: {
            const Component* la = circ.find(c.links[0]);
            const Component* lb = circ.find(c.links[1]);
            if (la && lb && la->kind == Kind::L && lb->kind == Kind::L &&
                c.links[0] != c.links[1]) {
                // coupling coefficient k (plain symbol) times the geometric
                // mean of the two inductances gives the mutual inductance
                ex k_val = reg_param(sys.params, c, "K");
                // make sure both inductors' symbols exist even if the L case
                // has not run yet (component order is arbitrary)
                reg_param(sys.params, *la, "L");
                reg_param(sys.params, *lb, "L");
                ex l1 = sys.params.get(param_symbol(*la, "L"));
                ex l2 = sys.params.get(param_symbol(*lb, "L"));
                sys.params.set(c.ref, c.estimate(), UnitClass::Plain);
                ex m = k_val * GiNaC::sqrt((l1 * l2).expand());
                int aa = sys.node_idx.count(la->nodes[0])
                             ? sys.node_idx.at(la->nodes[0])
                             : -1;
                int ab = sys.node_idx.count(la->nodes[1])
                             ? sys.node_idx.at(la->nodes[1])
                             : -1;
                int ba = sys.node_idx.count(lb->nodes[0])
                             ? sys.node_idx.at(lb->nodes[0])
                             : -1;
                int bb = sys.node_idx.count(lb->nodes[1])
                             ? sys.node_idx.at(lb->nodes[1])
                             : -1;
                int ka = sys.branch_idx.at(la->ref);
                int kb = sys.branch_idx.at(lb->ref);
                // v(a1) - v(a2) += s*M*i(b);  v(b1) - v(b2) += s*M*i(a)
                if (aa >= 0) sys.Y(ka, aa) += s * m;
                if (ab >= 0) sys.Y(ka, ab) -= s * m;
                if (ba >= 0) sys.Y(kb, ba) += s * m;
                if (bb >= 0) sys.Y(kb, bb) -= s * m;
            }
            break;
        }
        }
    }

    // Flush the deferred passive admittances. A group whose every contribution
    // is a pure 1/R becomes a single held parallel atom: with one resistor it
    // is just 1/R; with several it is 1/(R_i||R_j||...). Otherwise (a mix with
    // non-resistor admittances) fall back to stamping the accumulated sum.
    for (const auto& kv : def) {
        long long ra = kv.first.first, rb = kv.first.second;
        int a = ra == 0 ? -1 : int(ra - 1);
        int b2 = rb == 0 ? -1 : int(rb - 1);
        const Deferred& d = kv.second;
        ex y;
        if (d.all_res && d.res_syms.size() >= 2) {
            ex rpar = d.res_syms[0];
            for (size_t i = 1; i < d.res_syms.size(); ++i)
                rpar = par_ex(rpar, d.res_syms[i]);
            y = ex(1) / rpar;
        } else {
            y = d.sum;
        }
        if (a >= 0) {
            sys.Y(a, a) += y;
            if (b2 >= 0) sys.Y(a, b2) -= y;
        }
        if (b2 >= 0) {
            sys.Y(b2, b2) += y;
            if (a >= 0) sys.Y(b2, a) -= y;
        }
    }
    return sys;
}

std::vector<TimeConstant> open_circuit_time_constants(
    const Circuit& cin, const std::string& input_ref, ParamTable& params) {
    return open_circuit_time_constants(cin, input_ref, params,
                                       std::set<std::string>{});
}

std::vector<TimeConstant> open_circuit_time_constants(
    const Circuit& cin, const std::string& input_ref, ParamTable& params,
    const std::set<std::string>& used_nodes) {
    Circuit c = cin;
    resolve_mirrors(c);
    std::vector<TimeConstant> out;

    // The structural series fold must be IDENTICAL here to the one the main
    // solve performs, or the time constant will not symbolically divide the
    // denominator. The main fold only collapses a node that is degree-2 in the
    // full circuit; OCTC *removes* elements (every capacitor), which can drop a
    // higher-degree node to degree 2 and create a fold that the full circuit
    // never had. Protect every node whose full-circuit degree is not exactly 2
    // (plus the caller's used nodes), so a fold only happens at a node that is
    // degree-2 in both views.
    std::map<std::string, int> full_degree;
    for (const auto& cc : c.comps) {
        for (const auto& n : cc.nodes) ++full_degree[n];
        if ((cc.kind == Kind::NPN || cc.kind == Kind::PNP) &&
            cc.param_enabled("rb"))
            ++full_degree[bjt_internal_node(cc)];
    }
    std::set<std::string> protect = used_nodes;
    for (const auto& kv : full_degree)
        if (kv.first != "0" && kv.first != "GND" && kv.second != 2)
            protect.insert(kv.first);

    // With every independent source zeroed, every capacitor opened (omitted),
    // and every inductor except the one under test shorted (a 0 V source),
    // inject 1 A across the element's terminals and read V(a)-V(b): that is the
    // resistance it sees.
    auto port_R = [&](const std::string& test_sym, const std::string& na,
                      const std::string& nb) -> ex {
        // Zero-value reactances: every capacitor (including the test one, since
        // OCTC removes it and measures the resistance across its terminals) is
        // opened; every inductor except the test one is shorted. The test
        // inductor is removed too (we measure the resistance into its open
        // terminals). The test element's symbol also tells us which device
        // parasitic to disable.
        Circuit ct;
        for (const auto& cc : c.comps) {
            if (cc.kind == Kind::C) continue;              // all caps open
            if (cc.kind == Kind::L) {
                if (cc.ref == test_sym) continue;          // remove the test L
                Component sh = cc;                          // short the rest
                sh.kind = Kind::V;
                sh.value_text = "0";
                sh.ref = "__SHORT_" + cc.ref;
                ct.comps.push_back(sh);
                continue;
            }
            Component m = cc;
            if (is_independent_source(cc.kind)) m.value_text = "0";
            // Open every device parasitic capacitance: OCTC measures the
            // zero-value (all-other-caps-open) resistance.
            for (const auto& pd : param_defs(cc.kind)) {
                if (param_unit_class(cc.kind, pd.name) == UnitClass::Farad)
                    m.param_on[pd.name] = false;
            }
            ct.comps.push_back(m);
        }
        Component it;
        it.kind = Kind::I;
        it.ref = "__OCTC__";
        // Inject 1 A *into* node `na` (and out of `nb`). With the SPICE
        // convention ({n+,n-}: current flows n+ -> n- through the source), the
        // source that delivers current into `na` has n- = na, so the nodes are
        // ordered {nb, na}. Then R = (v(na) - v(nb)) / 1 A is positive.
        it.nodes = {nb, na};
        it.value_text = "1";
        ct.comps.push_back(it);
        try {
            AnalysisRequest r;
            r.input_ref = "__OCTC__";
            r.output = "V(" + na + ")";
            r.used_nodes = protect;
            Solved sa = solve(ct, r);
            ex va = (sa.num / sa.den).normal();
            if (nb == "0" || nb == "GND") return va;
            r.output = "V(" + nb + ")";
            Solved sb = solve(ct, r);
            ex vb = (sb.num / sb.den).normal();
            return (va - vb).normal();
        } catch (const std::exception&) {
            return ex(0);
        }
    };

    auto add_tc = [&](const std::string& label, const std::string& na,
                      const std::string& nb, bool is_cap, const ex& sym) {
        ex R = port_R(label, na, nb);
        if (R.is_zero()) return;
        R = to_parallel(R);
        ex tau = is_cap ? (R * sym).normal() : (sym / R).normal();
        TimeConstant tc;
        tc.label = label;
        tc.tau = tau;
        ex tv = params.eval_real(tau);
        if (GiNaC::is_a<GiNaC::numeric>(tv))
            tc.tau_value =
                std::fabs(GiNaC::ex_to<GiNaC::numeric>(tv).to_double());
        out.push_back(tc);
    };

    for (const auto& cc : c.comps) {
        if (cc.kind == Kind::C || cc.kind == Kind::L) {
            add_tc(cc.ref, cc.nodes[0], cc.nodes[1], cc.kind == Kind::C,
                   params.get(cc.ref));
            continue;
        }
        // Device parasitic capacitances are reactive elements too: give each
        // enabled one its own time constant, with the port being the two nodes
        // it bridges in the small-signal model.
        const std::string& n0 = cc.nodes.size() > 0 ? cc.nodes[0] : "0";
        const std::string& n1 = cc.nodes.size() > 1 ? cc.nodes[1] : "0";
        const std::string& n2 = cc.nodes.size() > 2 ? cc.nodes[2] : "0";
        for (const auto& pd : param_defs(cc.kind)) {
            if (param_unit_class(cc.kind, pd.name) != UnitClass::Farad) continue;
            if (!cc.param_enabled(pd.name)) continue;
            std::string a, b;
            // Map the parasitic to its node pair (matches the MNA stamps).
            if (pd.name == "Cgs") { a = n1; b = n2; }        // G-S
            else if (pd.name == "Cgd") { a = n1; b = n0; }   // G-D
            else if (pd.name == "Cds" || pd.name == "Cdb")
                { a = n0; b = n2; }                          // D-S
            else if (pd.name == "Csb") { a = n2; b = n2; }   // S-S (shunt)
            else if (pd.name == "Cpi") { a = n1; b = n2; }   // B-E
            else if (pd.name == "Cmu") { a = n1; b = n0; }   // B-C
            else if (pd.name == "Cd") { a = n0; b = n1; }    // A-K
            else { a = n0; b = n1; }
            if (a == b) continue;
            std::string sym_name = param_symbol(cc, pd.name);
            ex sym = params.get(sym_name);
            params.set(sym_name, cc.param_estimate(pd.name), UnitClass::Farad);
            add_tc(sym_name, a, b, true, sym);
        }
    }
    // Report the smallest (dominant) time constants first, matching the hand
    // method of starting from the dominant element.
    std::sort(out.begin(), out.end(),
              [](const TimeConstant& a, const TimeConstant& b) {
                  return a.tau_value > b.tau_value;
              });
    (void)input_ref;
    return out;
}

} // namespace syms
