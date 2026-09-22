#include "core/MNA.h"
#include "core/Eng.h"

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
// numerically so H(s) stays in terms of the real design variables.
ex gain_ex(const Component& c) {
    const std::string& t = c.value_text;
    bool plain = !t.empty();
    for (char ch : t)
        if (!((ch >= '0' && ch <= '9') || ch == '.' || ch == '+' ||
              ch == '-' || ch == 'e' || ch == 'E'))
            plain = false;
    if (plain) {
        try {
            return ex(GiNaC::numeric(t.c_str()));
        } catch (...) {
        }
    }
    double v = c.estimate();
    long i = static_cast<long>(v);
    if (static_cast<double>(i) == v && v >= -1e12 && v <= 1e12)
        return ex(i);
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
        std::string nd = (raw == "GND") ? "0" : raw;
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

    // --- unknown ordering: node voltages, then branch currents ----------
    for (const auto& nd : nodes) {
        if (nd == "0") continue;
        sys.node_idx[nd] = sys.n;
        sys.var_names.push_back("v(" + nd + ")");
        ++sys.n;
    }
    for (const auto& c : circ.comps) {
        if (has_branch_current(c.kind)) {
            sys.branch_idx[c.ref] = sys.n;
            sys.var_names.push_back("i(" + c.ref + ")");
            ++sys.n;
        }
    }
    if (sys.n == 0) throw std::runtime_error("circuit has no unknowns");

    sys.Y = matrix(sys.n, sys.n);
    sys.b = matrix(sys.n, 1);

    auto idx = [&](const std::string& raw) -> int {
        std::string key = (raw == "GND") ? "0" : raw;
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
            int k = sys.branch_idx.at(c.ref);
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
            sys.params.set(c.ref, c.estimate(), UnitClass::Siemens);
            ex g = sys.params.get(c.ref);
            stamp_vccs(idx(nd[0]), idx(nd[1]), idx(nd[2]), idx(nd[3]), g);
            break;
        }
        case Kind::NMOS:
        case Kind::PMOS: {
            int D = idx(nd[0]), G = idx(nd[1]), S = idx(nd[2]), B = idx(nd[3]);
            // register every model parameter (enabled or not) so the symbol
            // table always exposes the full model; only enabled ones stamp MNA
            ex gm = reg_param(sys.params, c, "gm");
            ex gmb = reg_param(sys.params, c, "gmb");
            ex ro = reg_param(sys.params, c, "ro");
            ex cgs = reg_param(sys.params, c, "Cgs");
            ex cgd = reg_param(sys.params, c, "Cgd");
            ex cdb = reg_param(sys.params, c, "Cdb");
            ex csb = reg_param(sys.params, c, "Csb");
            stamp_vccs(D, S, G, S, gm);
            if (c.param_enabled("gmb")) stamp_vccs(D, S, B, S, gmb);
            if (c.param_enabled("ro")) stamp_adm(D, S, ex(1) / ro);
            if (c.param_enabled("Cgs")) stamp_cap(G, S, cgs);
            if (c.param_enabled("Cgd")) stamp_cap(G, D, cgd);
            if (c.param_enabled("Cdb")) stamp_cap(D, B, cdb);
            if (c.param_enabled("Csb")) stamp_cap(S, B, csb);
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
            break;
        }
    }
    return sys;
}

} // namespace syms
