#include "core/Solver.h"

#include <algorithm>
#include <cctype>
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
        // A device's terminal current from its small-signal model: the MOSFET
        // drain (or BJT collector / diode anode) current, so I(M4), I(Q1), I(D1)
        // are valid probe outputs for every analysis. The symbols must be the
        // *mirror-aware* ones the MNA stamped (a copy's gm is 4*gm_unit, ...),
        // or the current will not collapse for a current mirror.
        auto psym = [&](const Component& c, const char* p) -> ex {
            std::string nm = param_symbol(c, p);
            if (!out.params.syms.count(nm) && c.mirror_ref.empty()) return ex(0);
            return reg_param(out.params, c, p);
        };
        if (cp->kind == Kind::NMOS || cp->kind == Kind::PMOS) {
            if (cp->nodes.size() < 3)
                throw std::runtime_error("device " + ref + " has too few nodes");
            ex vd = node_solution(cp->nodes[0]);
            ex vg = node_solution(cp->nodes[1]);
            ex vs = node_solution(cp->nodes[2]);
            ex id = psym(*cp, "gm") * (vg - vs);
            if (cp->param_enabled("ro")) id += (vd - vs) / psym(*cp, "ro");
            if (cp->param_enabled("Cgd"))
                id += s * psym(*cp, "Cgd") * (vg - vd);
            if (cp->param_enabled("Cds"))
                id += s * psym(*cp, "Cds") * (vd - vs);
            if (cp->param_enabled("Cdb"))
                id += s * psym(*cp, "Cdb") * (vd - vs);
            return id.normal();
        }
        if (cp->kind == Kind::NPN || cp->kind == Kind::PNP) {
            if (cp->nodes.size() < 3)
                throw std::runtime_error("device " + ref + " has too few nodes");
            ex vc = node_solution(cp->nodes[0]);
            ex vb = node_solution(cp->nodes[1]);
            ex ve = node_solution(cp->nodes[2]);
            ex ic = psym(*cp, "gm") * (vb - ve);
            if (cp->param_enabled("ro")) ic += (vc - ve) / psym(*cp, "ro");
            if (cp->param_enabled("Cmu"))
                ic += s * psym(*cp, "Cmu") * (vb - vc);
            return ic.normal();
        }
        if (cp->kind == Kind::D) {
            ex va = node_solution(cp->nodes[0]);
            ex vk = node_solution(cp->nodes[1]);
            ex i = (va - vk) / psym(*cp, "rd");
            if (cp->param_enabled("Cd")) i += s * psym(*cp, "Cd") * (va - vk);
            return i.normal();
        }
        if (cp->kind == Kind::I || pin_count(cp->kind) != 2)
            throw std::runtime_error("branch current not available for " + ref +
                                     " (supported: R, C, L, V, E, M, Q, D)");
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
    dc.vthn = tech.vthn;
    dc.vthp = tech.vthp;
    dc.vdd = tech.vdd;
    dc.include_clm = tech.include_clm;
    dc.lambda = tech.lambda;
    dc.square_law = tech.dc_mode == DcMode::SquareLaw;
    dc.uncox = tech.uncox;
    dc.upcox = tech.upcox;
    MnaSystem sys = build_mna(c, std::string(), used, &dc);

    ex det = bareiss_det(sys.Y);
    if (det.expand().is_zero())
        throw std::runtime_error(
            "DC: MNA matrix is singular -- check for floating nodes or missing "
            "ground");

    DcSolution out;
    out.params = sys.params;

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
        ex gm = reg_param(out.params, cc, "gm");
        if (has_id) {
            out.id[cc.ref] = branch_val("Id:", cc.ref);
            // The saturation voltage Vdsat = 2*Id/gm is a plain report quantity.
            out.vov[cc.ref] = (2 * out.id[cc.ref] / gm).normal();
        } else {
            // The Mode 1 unknown is X = 2*Id/gm (Vdsat, signed).
            ex xval = branch_val("Vdsat:", cc.ref);
            out.vov[cc.ref] = xval.normal();
            if (tech.dc_mode == DcMode::GmOverId) {
                double od = 0.0;
                ex ev = out.params.eval_real(xval);
                if (GiNaC::is_a<GiNaC::numeric>(ev))
                    od = GiNaC::ex_to<GiNaC::numeric>(ev).to_double();
                out.params.set("Vdsat_" + cc.ref, od, UnitClass::Volt);
                // Report the actual solved current gm*Vdsat/2: for a
                // current-driven device (a mirror leg) the KCL value collapses
                // to the input current (I1), just as in the square-law mode.
                out.id[cc.ref] = (xval * gm / 2).normal();
            } else {
                out.id[cc.ref] = (xval * gm / 2).normal();
            }
        }
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
            ex wsym = cc.mirror_ref.empty()
                          ? out.params.get("W_" + wref)
                          : ex(cc.mirror_num()) *
                                out.params.get("W_" + wref) /
                                ex(cc.mirror_dn());
            ex lsym = out.params.get("L_" + wref);
            ex ucox = out.params.get((cc.kind == Kind::NMOS) ? "unCox"
                                                            : "upCox");
            int sgn = (cc.kind == Kind::NMOS) ? 1 : -1;
            ex th = out.params.get((cc.kind == Kind::NMOS) ? "Vthn" : "Vthp");
            // Mode 2 is the square law in the device's gate-source voltage:
            //   gm = uCox*(W/L)*(Vgs - Vth),  Id = 1/2*uCox*(W/L)*(Vgs - Vth)^2
            // so no "Vdsat" symbol appears in this mode.
            ex gm_sym = out.params.get(param_symbol(cc, "gm"));
            ex vgs_ex = (node_of(cc.nodes[1]) - node_of(cc.nodes[2])).normal();
            double vgs_est = 0.0;
            {
                ex ev = out.params.eval_real(vgs_ex);
                if (GiNaC::is_a<GiNaC::numeric>(ev))
                    vgs_est = GiNaC::ex_to<GiNaC::numeric>(ev).to_double();
            }
            out.params.set("Vgs_" + cc.ref, vgs_est, UnitClass::Volt);
            ex vgs_sym = out.params.get("Vgs_" + cc.ref);
            ex vov = (vgs_sym - sgn * th).normal();
            gm_to_vov[gm_sym] = (ucox * wsym * vov / lsym).normal();
            // Vov = sqrt(2*Id*L/(uCox*W)) using the KCL current with gm already
            // replaced (so no gm survives). For a current-driven device (a
            // mirror leg) this is clean; for a gate-driven device the current
            // itself depends on Vov, so the sqrt would be self-referential --
            // there Vov stays the symbol and Vgs - Vth is the meaningful form.
            ex id_sub = out.id[cc.ref];
            id_sub = id_sub.subs(gm_sym == (ucox * wsym * vov / lsym));
            ex r2 = (2 * sgn * id_sub * lsym / (ucox * wsym)).normal();
            if (!r2.has(vov)) {
                // Current-driven (a mirror leg): Id comes from KCL and does not
                // depend on the overdrive, so Vgs = Vth + sqrt(2*Id*L/(uCox*W)).
                ex ov = sqrt(r2);
                vov_to_val[vgs_sym] = (ov + sgn * th).normal();
                // The Vov = r2/Vov identity keeps every term to one radical.
                vov_recip[vgs_sym] = (r2 / vov + sgn * th).normal();
                out.vov[cc.ref] = ov.normal();
            } else if (!vgs_ex.has(gm_sym) && !vgs_ex.has(vgs_sym)) {
                // Gate driven from a fixed source: Vgs is a plain node
                // difference, so substitute it and the current collapses to
                // (V1 - Vthn)^2.
                vov_to_val[vgs_sym] = vgs_ex.normal();
                out.vov[cc.ref] = (vgs_ex - sgn * th).normal();
            } else {
                // Source degeneration: the gate-source voltage itself depends on
                // the device current, so keep it as the named symbol Vgs_<ref>
                // and present the current as the square law in (Vgs - Vth).
                out.id[cc.ref] =
                    (ucox * wsym / lsym * vov * vov / 2).normal();
                out.vov[cc.ref] = vov;
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
            // Vgs = Vov + sgn*Vth keeps the low-entropy overdrive form; Vds and
            // the node voltages take the solved sqrt(...) values.
            const int sgn = (cc.kind == Kind::NMOS) ? 1 : -1;
            ex th = out.params.get((cc.kind == Kind::NMOS) ? "Vthn" : "Vthp");
            out.vgs[cc.ref] = (sgn * th + out.vov[cc.ref]).normal();
            out.vds[cc.ref] = simplify(vds);
        } else {
            out.vgs[cc.ref] = vgs.normal();
            out.vds[cc.ref] = vds.normal();
        }
    }

    // Top-down presentation: solve a second system in which each device is an
    // ideal current source I(M<ref>) (no gate row), so each node voltage comes
    // out as a linear, factored combination of the device currents. Skipped when
    // that system is singular (a node fed only by device currents), in which case
    // the caller keeps the device-parameter presentation.
    if (!out.mosfets.empty()) {
        for (int pass = 0; pass < 2 && !out.have_td; ++pass) {
            MnaDcSupply tdc = dc;
            tdc.current_sources = true;
            tdc.current_sources_ro = (pass == 1);
            MnaSystem td = build_mna(c, std::string(), used, &tdc);
            for (const auto& ref : out.mosfets) {
                double e = 0.0;
                ex ev = out.params.eval_real(out.id[ref]);
                if (GiNaC::is_a<GiNaC::numeric>(ev))
                    e = GiNaC::ex_to<GiNaC::numeric>(ev).to_double();
                td.params.set("I(" + ref + ")", e, UnitClass::Plain);
            }
            ex tdet = bareiss_det(td.Y);
            if (tdet.expand().is_zero()) continue;
            for (const auto& kv : td.node_idx)
                out.node_v_td[kv.first] =
                    (det_with_column(td.Y, kv.second, td.b) / tdet).normal();
            out.have_td = true;
            out.td_used_ro = (pass == 1);
        }
    }
    return out;
}

} // namespace syms
