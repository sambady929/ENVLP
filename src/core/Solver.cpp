#include "core/Solver.h"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>

namespace syms {
namespace {

using GiNaC::ex;
using GiNaC::matrix;
using GiNaC::numeric;
using GiNaC::exmap;

// Prefer pivots that keep fraction-free elimination exact and cheap:
// +-1 integers first, then integers, numbers, symbols, powers, muls, sums.
int pivot_score(const ex& e) {
    if (GiNaC::is_a<numeric>(e)) {
        const numeric& n = GiNaC::ex_to<numeric>(e);
        if (n.is_integer()) {
            if (n.is_equal(numeric(1)) || n.is_equal(numeric(-1))) return 0;
            return 1;
        }
        return 2;
    }
    if (GiNaC::is_a<GiNaC::symbol>(e)) return 3;
    if (GiNaC::is_a<GiNaC::power>(e)) return 4;
    if (GiNaC::is_a<GiNaC::mul>(e)) return 5;
    if (GiNaC::is_a<GiNaC::add>(e)) return 6 + static_cast<int>(std::min<size_t>(e.nops(), 20));
    return 9;
}

// Fraction-free (Bareiss) determinant. Exact division keeps entries in the
// polynomial ring; .normal() guarantees cancellation even if a swap breaks
// the fraction-free property (result stays mathematically correct either way).
ex bareiss_det(matrix M) {
    int n = static_cast<int>(M.rows());
    if (n == 0) return ex(1);
    if (n == 1) return M(0, 0);

    ex sign(1);
    ex prev(1);
    for (int k = 0; k < n - 1; ++k) {
        int piv = -1;
        int best = 1 << 30;
        for (int i = k; i < n; ++i) {
            if (M(i, k).is_zero()) continue;
            int sc = pivot_score(M(i, k));
            if (sc < best) {
                best = sc;
                piv = i;
            }
        }
        if (piv < 0) return ex(0);
        if (piv != k) {
            for (int j = 0; j < n; ++j) std::swap(M(k, j), M(piv, j));
            sign = -sign;
        }
        ex pk = M(k, k);
        for (int i = k + 1; i < n; ++i) {
            for (int j = k + 1; j < n; ++j) {
                ex v = M(i, j) * pk - M(i, k) * M(k, j);
                if (prev.is_equal(ex(1)))
                    M(i, j) = v;
                else
                    M(i, j) = (v / prev).normal();
            }
        }
        prev = pk;
    }
    return sign * M(n - 1, n - 1);
}

// Determinant of Y with column k replaced by the input column b.
ex det_with_column(const matrix& Y, int k, const matrix& bcol) {
    matrix M(Y);
    for (int i = 0; i < static_cast<int>(M.rows()); ++i) M(i, k) = bcol(i, 0);
    return bareiss_det(std::move(M));
}

} // namespace

Solved solve(const Circuit& circ, const AnalysisRequest& req) {
    // The output node is always read, and the caller may add more (DC enumerates
    // every node): those nodes must not be folded into an internal series node.
    std::set<std::string> used = req.used_nodes;
    if (req.output.size() > 3 && req.output.front() == 'V' && req.output.back() == ')')
        used.insert(req.output.substr(2, req.output.size() - 3));
    // A branch-current output I(ref) reads both terminals of `ref`: those nodes
    // must stay real unknowns so the current is well defined (folding a series
    // group through them would change which branch the current refers to).
    if (req.output.size() > 3 && req.output.front() == 'I' && req.output.back() == ')') {
        std::string ref = req.output.substr(2, req.output.size() - 3);
        if (ref.rfind("L:", 0) == 0) ref = ref.substr(2);
        if (const Component* cp = circ.find(ref)) {
            used.insert(cp->nodes.begin(), cp->nodes.end());
        }
    }
    MnaSystem sys = build_mna(circ, req.input_ref, used);

    Solved out;
    out.input_desc = req.input_ref;
    out.params = std::move(sys.params);
    ex s = out.params.get("s");

    ex det = bareiss_det(sys.Y);
    if (det.expand().is_zero())
        throw std::runtime_error(
            "MNA matrix is singular -- check for floating nodes or missing ground");

    // Lazily computed per-unknown numerators (numerators over common det).
    std::map<int, ex> sol;
    auto solution = [&](int k) -> const ex& {
        auto it = sol.find(k);
        if (it == sol.end())
            it = sol.emplace(k, det_with_column(sys.Y, k, sys.b)).first;
        return it->second;
    };
    auto node_solution = [&](const std::string& raw) -> ex {
        std::string key = (raw == "GND") ? "0" : raw;
        if (key == "0") return ex(0);
        auto it = sys.node_idx.find(key);
        if (it == sys.node_idx.end()) {
            // List the nets that DO exist; auto-assigned nets are "n1"/"n2"/..
            // when no net label names them, so "V(out)" with no "out" label is
            // the common failure mode.
            std::string avail;
            for (const auto& kv : sys.node_idx) {
                if (!avail.empty()) avail += ", ";
                avail += kv.first;
            }
            throw std::runtime_error(
                "unknown node: " + key + " -- no net has that name. Existing "
                "nets: " + (avail.empty() ? std::string("(none)") : avail) +
                ". Place a net label (N) on the wire you want to measure.");
        }
        return solution(it->second);
    };

    const std::string& spec = req.output;
    ex num;

    if (spec.size() > 3 && spec.front() == 'V' && spec.back() == ')') {
        std::string node = spec.substr(2, spec.size() - 3);
        if (node == "GND") node = "0";
        num = node_solution(node);
        out.output_desc = "V(" + node + ")";
    } else if (spec.size() > 3 && spec.front() == 'I' && spec.back() == ')') {
        std::string ref = spec.substr(2, spec.size() - 3);
        // strip an explicit L: prefix used to disambiguate the inductor
        // branch from the inductor's symbol name
        std::string look = ref;
        if (look.rfind("L:", 0) == 0) look = look.substr(2);
        auto bit = sys.branch_idx.find(look);
        if (bit == sys.branch_idx.end() && look != ref)
            bit = sys.branch_idx.find(ref);
        if (bit != sys.branch_idx.end()) {
            num = solution(bit->second);
        } else {
            const Component* cp = circ.find(ref);
            if (!cp) throw std::runtime_error("unknown component: " + ref);
            if (cp->kind == Kind::I || pin_count(cp->kind) != 2)
                throw std::runtime_error(
                    "branch current not available for " + ref +
                    " (supported: R, C, L, V, E)");
            ex va = node_solution(cp->nodes[0]);
            ex vb = node_solution(cp->nodes[1]);
            ex z;
            switch (cp->kind) {
                case Kind::R: z = out.params.get(cp->ref); break;
                case Kind::C: z = ex(1) / (s * out.params.get(cp->ref)); break;
                case Kind::L: z = s * out.params.get(cp->ref); break;
                default:
                    throw std::runtime_error("unsupported I() target: " + ref);
            }
            num = (va - vb) / z;
        }
        out.output_desc = "I(" + ref + ")";
    } else {
        throw std::runtime_error(
            "output must look like V(node) or I(ref), got: " + spec);
    }

    out.num = num;
    out.den = det;
    return out;
}

DcSolution solve_dc(const Circuit& c, const TechParams& tech) {
    std::set<std::string> used;
    for (const auto& cc : c.comps)
        for (const auto& n : cc.nodes) {
            std::string nd = (n == "GND") ? "0" : n;
            if (nd != "0") used.insert(nd);
        }
    MnaDcSupply dc;
    dc.vth = tech.vth;
    dc.square_law = tech.dc_mode == DcMode::SquareLaw;
    dc.uncox = tech.uncox;
    dc.upcox = tech.upcox;
    MnaSystem sys = build_mna(c, std::string(), used, &dc);

    DcSolution out;
    out.params = sys.params;

    ex det = bareiss_det(sys.Y);
    if (det.expand().is_zero())
        throw std::runtime_error(
            "DC: MNA matrix is singular -- check for floating nodes or missing "
            "ground");

    std::map<int, ex> sol;
    auto solution = [&](int k) -> ex {
        auto it = sol.find(k);
        if (it == sol.end())
            it = sol.emplace(k, det_with_column(sys.Y, k, sys.b)).first;
        return (it->second / det).normal();
    };

    for (const auto& kv : sys.node_idx)
        out.node_v[kv.first] = solution(kv.second);
    auto node_of = [&](const std::string& raw) -> ex {
        std::string nd = (raw == "GND") ? "0" : raw;
        if (nd == "0") return ex(0);
        auto it = out.node_v.find(nd);
        return it == out.node_v.end() ? ex(0) : it->second;
    };
    // --- per-device drain current, then (Mode 2) the overdrive ---
    // Id is the raw KCL unknown: for a current mirror leg that is exactly the
    // reference current, so I(M1) = I1 is reported directly.
    for (const auto& cc : c.comps) {
        if (cc.kind != Kind::NMOS && cc.kind != Kind::PMOS) continue;
        auto it = sys.branch_idx.find("Id:" + cc.ref);
        if (it == sys.branch_idx.end()) continue;
        out.mosfets.push_back(cc.ref);
        out.id[cc.ref] = solution(it->second).normal();
    }

    // Mode 2 (square law): solve each overdrive from the drain current and the
    // device geometry, Vov = sqrt(2*Id*L/(uCox*W)), with W/L mirror-aware (a
    // copy's W is mult*W_unit). Substituting Vov (and gm = uCox*(W/L)*Vov) into
    // the solved expressions gives the textbook large-signal forms -- e.g.
    // Vov = sqrt(2*I1*L_M1/(uCox*W_M1)) and Vgs = Vth + that.
    // Two-pass substitution so identical sqrt bases combine: first replace each
    // gm by its overdrive form gm = uCox*(W/L)*Vov (keeping Vov a symbol), then
    // replace Vov by sqrt(2*Id*L/(uCox*W)). With the same Vov symbol in both,
    // products of the sqrt and its reciprocal collapse.
    exmap gm_to_vov, vov_to_val;
    if (tech.dc_mode == DcMode::SquareLaw) {
        for (const auto& cc : c.comps) {
            if (cc.kind != Kind::NMOS && cc.kind != Kind::PMOS) continue;
            auto it = sys.branch_idx.find("Id:" + cc.ref);
            if (it == sys.branch_idx.end()) continue;
            std::string wref = cc.mirror_ref.empty() ? cc.ref : cc.mirror_ref;
            int mult = cc.multiplicity();
            ex wsym = cc.mirror_ref.empty()
                          ? out.params.get("W_" + wref)
                          : ex(mult) * out.params.get("W_" + wref);
            ex lsym = out.params.get("L_" + wref);
            ex ucox = out.params.get((cc.kind == Kind::NMOS) ? "unCox"
                                                            : "upCox");
            int sgn = (cc.kind == Kind::NMOS) ? 1 : -1;
            out.params.set("Vov_" + cc.ref, 0.2, UnitClass::Volt);
            ex vov = out.params.get("Vov_" + cc.ref);
            gm_to_vov[out.params.get(param_symbol(cc, "gm"))] =
                (ucox * wsym * vov / lsym).normal();
            // Vov = sqrt(2*Id*L/(uCox*W)) using the KCL current with gm already
            // replaced (so no gm survives). For a current-driven device (a
            // mirror leg) this is clean; for a gate-driven device the current
            // itself depends on Vov, so the sqrt would be self-referential --
            // there Vov stays the symbol and Vgs - Vth is the meaningful form.
            ex id_sub = out.id[cc.ref];
            ex gm_sym = out.params.get(param_symbol(cc, "gm"));
            id_sub = id_sub.subs(gm_sym == (ucox * wsym * vov / lsym));
            ex vov_val =
                sqrt((2 * sgn * id_sub * lsym / (ucox * wsym)).normal());
            if (vov_val.has(vov)) {
                out.vov[cc.ref] = vov; // gate-driven: keep the overdrive symbol
            } else {
                vov_to_val[vov] = vov_val;
                out.vov[cc.ref] = vov_val.normal();
            }
        }
    }
    auto simplify = [&](ex e) -> ex {
        if (!gm_to_vov.empty()) e = e.subs(gm_to_vov);
        if (!vov_to_val.empty()) e = e.subs(vov_to_val);
        return e.normal();
    };
    if (!gm_to_vov.empty())
        for (auto& kv : out.node_v) kv.second = simplify(kv.second);

    for (const auto& cc : c.comps) {
        if (cc.kind != Kind::NMOS && cc.kind != Kind::PMOS) continue;
        auto it = sys.branch_idx.find("Id:" + cc.ref);
        if (it == sys.branch_idx.end()) continue;
        // Express the drain current through the overdrive as well.
        if (tech.dc_mode == DcMode::SquareLaw && !gm_to_vov.empty())
            out.id[cc.ref] = simplify(out.id[cc.ref]);
        ex vgs = (node_of(cc.nodes[1]) - node_of(cc.nodes[2]));
        ex vds = (node_of(cc.nodes[0]) - node_of(cc.nodes[2]));
        if (tech.dc_mode == DcMode::SquareLaw) {
            // Vgs = Vov + Vth stays in the low-entropy overdrive form; Vds and
            // the node voltages take the solved sqrt(...) values.
            out.vgs[cc.ref] =
                (out.params.get("Vth") + out.params.get("Vov_" + cc.ref))
                    .normal();
            out.vds[cc.ref] = simplify(vds);
        } else {
            out.vgs[cc.ref] = vgs.normal();
            out.vds[cc.ref] = vds.normal();
            ex gm = reg_param(out.params, cc, "gm");
            out.vov[cc.ref] = (2 * out.id[cc.ref] / gm).normal();
        }
    }
    return out;
}

} // namespace syms
