#include "core/MNA.h"
#include "core/Eng.h"

#include <cmath>
#include <cstdlib>
#include <set>
#include <stdexcept>

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

} // namespace

MnaSystem build_mna(const Circuit& circ, const std::string& input_ref) {
    std::string err;
    if (!circ.validate(err)) throw std::runtime_error(err);

    const Component* in = circ.find(input_ref);
    if (!in)
        throw std::runtime_error("input source '" + input_ref + "' not found");
    if (!is_independent_source(in->kind))
        throw std::runtime_error("input must be an ideal source (V or I), got " +
                                 in->ref + " (" + kind_display(in->kind) + ")");

    MnaSystem sys;
    ex s = sys.params.get("s");

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

    // --- generic stamps --------------------------------------------------
    auto stamp_adm = [&](int a, int bb, const ex& y) {
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
            sys.params.set(c.ref, c.estimate(), UnitClass::Ohm);
            ex r = sys.params.get(c.ref);
            stamp_adm(idx(nd[0]), idx(nd[1]), ex(1) / r);
            break;
        }
        case Kind::C: {
            sys.params.set(c.ref, c.estimate(), UnitClass::Farad);
            ex cap = sys.params.get(c.ref);
            stamp_cap(idx(nd[0]), idx(nd[1]), cap);
            break;
        }
        case Kind::L: {
            sys.params.set(c.ref, c.estimate(), UnitClass::Henry);
            ex l = sys.params.get(c.ref);
            int a = idx(nd[0]), bb = idx(nd[1]);
            int k = sys.branch_idx.at("L:" + c.ref);
            if (a >= 0) {
                sys.Y(a, k) += 1;
                sys.Y(k, a) += 1;
            }
            if (bb >= 0) {
                sys.Y(bb, k) -= 1;
                sys.Y(k, bb) -= 1;
            }
            sys.Y(k, k) -= s * l;
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
            sys.b(k, 0) = (c.ref == input_ref) ? ex(1) : ex(0);
            break;
        }
        case Kind::I: {
            ex drive = (c.ref == input_ref) ? ex(1) : ex(0);
            int a = idx(nd[0]), bb = idx(nd[1]);
            if (a >= 0) sys.b(a, 0) += drive;
            if (bb >= 0) sys.b(bb, 0) -= drive;
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
            // no body terminal: the body is tied to the source
            stamp_vccs(D, S, G, S, gm);
            if (c.param_enabled("ro")) stamp_adm(D, S, ex(1) / ro);
            if (c.param_enabled("Cgs")) stamp_cap(G, S, cgs);
            if (c.param_enabled("Cgd")) stamp_cap(G, D, cgd);
            if (c.param_enabled("Cds")) stamp_cap(D, S, cds);
            if (c.param_enabled("Cdb")) stamp_cap(D, S, cdb);
            if (c.param_enabled("Csb")) stamp_cap(S, S, csb);
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
            if (c.param_enabled("Cpi")) stamp_cap(Bi, E, cpi);
            if (c.param_enabled("Cmu")) stamp_cap(Bi, C, cmu);
            break;
        }
        case Kind::GND:
        case Kind::VDD:
            break;
        case Kind::D: {
            // small-signal diode: gm(A->K), optional rd in series, Cd
            int A = idx(nd[0]), Kk = idx(nd[1]);
            ex gm = reg_param(sys.params, c, "gm");
            ex rd = reg_param(sys.params, c, "rd");
            ex cd = reg_param(sys.params, c, "Cd");
            stamp_vccs(A, Kk, A, Kk, gm);
            if (c.param_enabled("rd")) stamp_adm(A, Kk, ex(1) / rd);
            if (c.param_enabled("Cd")) stamp_cap(A, Kk, cd);
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
            // gain block: v(out) = A*(v(in) - v(out)), inverting sides grounded
            ex gain = gain_ex(c);
            int a = idx(nd[0]), bb = idx(nd[1]);
            int k = sys.branch_idx.at(c.ref);
            stamp_branch(a, bb, k);
            if (bb >= 0) sys.Y(k, bb) -= gain;
            break;
        }
        case Kind::OPAMP: {
            // finite-gain op-amp: v(out) = A*(v(in+) - v(in-))
            ex gain = gain_ex(c);
            int ip = idx(nd[0]), im = idx(nd[1]), o = idx(nd[2]);
            int k = sys.branch_idx.at(c.ref);
            stamp_branch(o, -1, k);
            if (im >= 0) sys.Y(k, im) += gain;
            if (ip >= 0) sys.Y(k, ip) -= gain;
            break;
        }
        case Kind::NULLOR: {
            // Ideal nullor: a nullator across the input port (zero voltage,
            // zero current) and a norator at the output (its current is
            // whatever the circuit demands). Model with one extra unknown, the
            // norator current k (flowing from `out` into the norator, which
            // returns to ground), and one constraint row v(in+) = v(in-).
            int ip = idx(nd[0]), im = idx(nd[1]), o = idx(nd[2]);
            int k = sys.branch_idx.at(c.ref);
            if (ip >= 0) sys.Y(k, ip) += 1;
            if (im >= 0) sys.Y(k, im) -= 1;
            if (o >= 0) sys.Y(o, k) += 1;
            break;
        }
        case Kind::FDOPAMP: {
            // fully differential: v(out+)-v(out-) = A*(v(in+)-v(in-))
            ex gain = gain_ex(c);
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
                int ka = sys.branch_idx.at("L:" + la->ref);
                int kb = sys.branch_idx.at("L:" + lb->ref);
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
    return sys;
}

} // namespace syms
