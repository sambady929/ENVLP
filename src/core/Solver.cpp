#include "core/Solver.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <functional>
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

// Evaluate a probe sum such as "V(out)", "V(a)-V(b)", "2*V(a)-V(b)",
// "V(a)+V(b)". `one_term(type, name)` evaluates a single probe over the common
// denominator. Used for differential ports (a differential output is just a
// weighted sum of node voltages).
ex parse_probe_sum(const std::string& spec,
                   const std::function<ex(char, const std::string&)>& one_term) {
    ex acc = 0;
    size_t i = 0, n = spec.size();
    int sign = 1;
    bool any = false;
    while (i < n) {
        while (i < n && std::isspace((unsigned char)spec[i])) ++i;
        if (i >= n) break;
        if (spec[i] == '+') { sign = 1; ++i; continue; }
        if (spec[i] == '-') { sign = -1; ++i; continue; }
        // Optional numeric coefficient like "2*". Keep it an EXACT integer
        // (not a double): a floating 1.0 coefficient would block the exact
        // cancellation the low-entropy pruner relies on.
        long coeff = 1;
        size_t j = i;
        bool have_num = false;
        while (j < n && std::isdigit((unsigned char)spec[j])) {
            have_num = true;
            ++j;
        }
        if (have_num && j < n && spec[j] == '*') {
            coeff = std::strtol(spec.substr(i, j - i).c_str(), nullptr, 10);
            i = j + 1;
        }
        if (i + 1 >= n || (spec[i] != 'V' && spec[i] != 'I') || spec[i + 1] != '(') {
            throw std::runtime_error(
                "output term must look like V(node) or I(ref): " +
                spec.substr(i));
        }
        char type = spec[i];
        size_t close = spec.find(')', i);
        if (close == std::string::npos)
            throw std::runtime_error("unbalanced parentheses in output: " + spec);
        std::string name = spec.substr(i + 2, close - (i + 2));
        acc += ex(sign * coeff) * one_term(type, name);
        any = true;
        sign = 1;
        i = close + 1;
    }
    if (!any)
        throw std::runtime_error(
            "output must look like V(node), I(ref), or a sum such as "
            "V(a)-V(b), got: " + spec);
    return acc;
}

} // namespace

Solved solve(const Circuit& circ, const AnalysisRequest& req) {
    // The output node is always read, and the caller may add more (DC enumerates
    // every node): those nodes must not be folded into an internal series node.
    std::set<std::string> used = req.used_nodes;
    // Every probe in the output expression must resolve to a real unknown: a
    // V(node) names a node; an I(ref) names the branch's two terminals (folding
    // a series group through them would change which branch the current refers
    // to). A differential port "V(a)-V(b)" protects both a and b.
    for (size_t i = 0; i + 1 < req.output.size(); ++i) {
        char type = req.output[i];
        if ((type != 'V' && type != 'I') || req.output[i + 1] != '(') continue;
        size_t close = req.output.find(')', i + 2);
        if (close == std::string::npos) continue;
        std::string name = req.output.substr(i + 2, close - (i + 2));
        if (type == 'V') {
            if (name != "GND" && name != "0") used.insert(name);
        } else {
            std::string ref = name;
            if (ref.rfind("L:", 0) == 0) ref = ref.substr(2);
            if (const Component* cp = circ.find(ref))
                used.insert(cp->nodes.begin(), cp->nodes.end());
        }
        i = close;
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

    // The output may be a single probe (V(node) / I(ref)) or a linear
    // combination of probes with +/- and an optional numeric coefficient, e.g.
    // a differential port "V(out+)-V(out-)". Every term is evaluated over the
    // common determinant `det`, so the numerators simply add.
    auto one_term = [&](char type, const std::string& name) -> ex {
        if (type == 'V') {
            std::string node = (name == "GND") ? "0" : name;
            return node_solution(node);
        }
        // I(ref): prefer the branch unknown; fall back to v/z for a 2-terminal
        // passive that has no branch unknown.
        std::string ref = name;
        std::string look = ref;
        if (look.rfind("L:", 0) == 0) look = look.substr(2);
        auto bit = sys.branch_idx.find(look);
        if (bit == sys.branch_idx.end() && look != ref)
            bit = sys.branch_idx.find(ref);
        if (bit != sys.branch_idx.end()) return solution(bit->second);
        const Component* cp = circ.find(ref);
        if (!cp) throw std::runtime_error("unknown component: " + ref);
        if (cp->kind == Kind::I || pin_count(cp->kind) != 2)
            throw std::runtime_error("branch current not available for " + ref +
                                     " (supported: R, C, L, V, E)");
        ex va = node_solution(cp->nodes[0]);
        ex vb = node_solution(cp->nodes[1]);
        ex z;
        switch (cp->kind) {
            case Kind::R: z = out.params.get(cp->ref); break;
            case Kind::C: z = ex(1) / (s * out.params.get(cp->ref)); break;
            case Kind::L: z = s * out.params.get(cp->ref); break;
            default: throw std::runtime_error("unsupported I() target: " + ref);
        }
        return (va - vb) / z;
    };

    const std::string& spec = req.output;
    ex num = parse_probe_sum(spec, one_term);
    out.output_desc = spec;
    // A bare single probe keeps the tidy "V(out)" description.
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
    // --- per-device drain current ---
    // Mode 1's unknown is X = 2*Id/gm (Vdsat/Vov, signed), so Id = X*gm/2;
    // Mode 2's unknown is Id itself (the raw KCL current: a mirror leg
    // reports I1 directly).
    auto branch_val = [&](const char* prefix, const std::string& ref) -> ex {
        auto it = sys.branch_idx.find(std::string(prefix) + ref);
        return it == sys.branch_idx.end() ? ex(0) : solution(it->second).normal();
    };
    for (const auto& cc : c.comps) {
        if (cc.kind != Kind::NMOS && cc.kind != Kind::PMOS) continue;
        bool has_id = sys.branch_idx.count("Id:" + cc.ref) != 0;
        bool has_vd = sys.branch_idx.count("Vdsat:" + cc.ref) != 0;
        if (!has_id && !has_vd) continue;
        out.mosfets.push_back(cc.ref);
        if (has_id) {
            out.id[cc.ref] = branch_val("Id:", cc.ref);
        } else {
            // The Mode 1 unknown is X = 2*Id/gm (Vdsat for an NMOS, Vov for a
            // PMOS, both signed), so Id = X*gm/2 directly.
            ex gm = reg_param(out.params, cc, "gm");
            out.id[cc.ref] = (branch_val("Vdsat:", cc.ref) * gm / 2).normal();
        }
        // The saturation voltage Vdsat = 2*Id/gm is an unknown in Mode 1 and a
        // plain report quantity in Mode 2.
        out.vov[cc.ref] = (2 * out.id[cc.ref] / reg_param(out.params, cc, "gm"))
                              .normal();
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
    exmap gm_to_vov, vov_to_val, vov_recip;
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
            ex r2 = (2 * sgn * id_sub * lsym / (ucox * wsym)).normal();
            ex vov_val = sqrt(r2);
            if (!r2.has(vov)) {
                vov_to_val[vov] = vov_val;
                // A 1/Vov factor would otherwise leave a second radical that
                // GiNaC will not combine with a numerator sqrt; replace Vov by
                // the equivalent r2/Vov so every term has at most one radical.
                vov_recip[vov] = (r2 / vov).normal();
                out.vov[cc.ref] = vov_val.normal();
            } else {
                out.vov[cc.ref] = vov; // gate-driven: keep the overdrive symbol
            }
        }
    }
    // Staged: substitute gm (Vov still a symbol), clear any 1/Vov via Vov = r/Vov,
    // and only then replace Vov by its single sqrt -- otherwise the numerator
    // sqrt and the denominator sqrt stay two un-combinable radicals.
    auto simplify = [&](ex e) -> ex {
        if (!gm_to_vov.empty()) e = e.subs(gm_to_vov).normal();
        for (const auto& kv : vov_recip) {
            if (e.has(kv.first)) {
                exmap m;
                m[kv.first] = kv.second;
                e = e.subs(m).normal();
            }
        }
        if (!vov_to_val.empty()) e = e.subs(vov_to_val).normal();
        return e;
    };
    if (!gm_to_vov.empty())
        for (auto& kv : out.node_v) kv.second = simplify(kv.second);

    for (const auto& cc : c.comps) {
        if (cc.kind != Kind::NMOS && cc.kind != Kind::PMOS) continue;
        if (sys.branch_idx.count("Id:" + cc.ref) == 0 &&
            sys.branch_idx.count("Vdsat:" + cc.ref) == 0)
            continue;
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
        }
    }
    return out;
}

} // namespace syms
