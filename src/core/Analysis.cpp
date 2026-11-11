#include "core/Analysis.h"
#include "core/DcNumeric.h"
#include "core/Eng.h"
#include "core/LowEntropy.h"
#include "core/MNA.h"
#include "core/Par.h"
#include "core/Print.h"
#include "core/SpiceModel.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <functional>
#include <iomanip>
#include <map>
#include <queue>
#include <set>
#include <sstream>
#include <stdexcept>

namespace syms {

using GiNaC::ex;
using GiNaC::is_a;

namespace {

// An underscore starts a subscript in LaTeX, so a name that carries one
// (a branch like I(__SHORT__)) has to be escaped before it goes into the
// typeset report -- otherwise MathJax reports "Missing open brace for
// subscript".
std::string latex_escape_underscores(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        if (c == '_') out += "\\_";
        else out += c;
    }
    return out;
}

PruneOptions opts_of(const AnalysisSpec& s) {
    PruneOptions o;
    o.f0_hz = s.f0_hz;
    o.component_threshold_ratio = s.component_threshold_ratio;
    o.pole_zero_threshold_ratio = s.pole_zero_threshold_ratio;
    o.threshold_db = s.threshold_db;
    o.pole_zero_threshold_db = s.pole_zero_threshold_db;
    o.pole_ref = (s.pole_ref == AnalysisSpec::PoleRef::UgBw)
                     ? PruneOptions::PoleRef::UgBw
                     : PruneOptions::PoleRef::Dominant;
    o.global_ref = s.global_ref;
    o.prune = s.prune;
    // Parallelizing terms (R1||R2) is hardcoded on for now (#6).
    o.use_parallel = true;
    o.approx_factor = s.approx_factor;
    // Rank terms by their worst case across the sweep band.
    o.band_lo_hz = s.sweep.f_start_hz;
    o.band_hi_hz = s.sweep.f_stop_hz;
    // Normalize the denominator's DC term to 1 for every analysis. This is
    // what yields (1 + s*tau) factors AND what lets a parallel combination
    // collapse: an output impedance R1*ro/(R1+ro+s*R1*ro*C1) normalizes to
    // (R1||ro)/(1 + s*(R1||ro)*C1). Impedance analyses previously disabled
    // this, which is exactly why their results came out fully expanded and
    // high-entropy.
    o.normalize = true;
    return o;
}

AnalysisRequest req_of(const AnalysisSpec& s, const std::string& in,
                       const std::string& out) {
    AnalysisRequest r;
    r.input_ref = in;
    r.output = out;
    r.f0_hz = s.f0_hz;
    r.component_threshold_ratio = s.component_threshold_ratio;
    r.pole_zero_threshold_ratio = s.pole_zero_threshold_ratio;
    r.threshold_db = s.threshold_db;
    r.pole_zero_threshold_db = s.pole_zero_threshold_db;
    r.pole_ref = (s.pole_ref == AnalysisSpec::PoleRef::UgBw)
                     ? AnalysisRequest::PoleRef::UgBw
                     : AnalysisRequest::PoleRef::Dominant;
    r.global_ref = s.global_ref;
    r.prune = s.prune;
    r.use_parallel = s.use_parallel;
    r.approx_factor = s.approx_factor;
    r.normalize = s.normalize;
    r.sweep = s.sweep;
    return r;
}

// ---------------------------------------------------------------------------
// Graph/EET transfer-function extraction
//
// The MNA solve produces a raw rational function by expanding a determinant.
// For a network whose reactive elements are explicit C/L branches, the same
// function can be built as a product of EET factors instead:
//
//   H = H_ref * (1 + Z/Zn) / (1 + Z/Zd)
//
// where Zn and Zd are the open-port driving-point impedances with the output
// nulled and with the input zeroed. We obtain them from three EET solves:
// the element shorted, open, and replaced by a 1-ohm resistor. The recursion
// peels one element at a time; the reference transfer for the next step is the
// same network with the peeled element at its reference value. The result is
// verified symbolically against the ordinary MNA solve before it is accepted,
// so an unsupported topology falls back to the exact MNA path.
// ---------------------------------------------------------------------------
struct EetTF {
    bool ok = false;
    ex num, den;
    ParamTable params;
    std::string out_desc;
};

// One peelable reactive element: an explicit C/L, or an enabled device
// capacitance such as Cds_M1. `param` is empty for an explicit component.
struct EetElem {
    std::string key;                 // C1, Cds_M1, ...
    std::string comp;                // owning component ref
    std::string param;               // empty for explicit C/L
    bool cap = true;
    std::vector<std::string> nodes;  // the two terminals
};

// EET's linear-fractional fit needs the held R1||R2 / (R2+R3) groups to behave
// as algebraic atoms, but we must not expand them: that destroys exactly the
// low-entropy grouping the solver is trying to preserve. Give each distinct
// held group a temporary symbol for the derivation, then restore the group in
// the final expression.
struct EetGroups {
    std::map<ex, ex> to_sym;
    std::map<ex, ex> from_sym;
    int next = 0;

    ex atomize(const ex& e) {
        GiNaC::exmap m;
        std::function<void(const ex&)> collect = [&](const ex& x) {
            if (is_parallel(x) || is_series(x)) {
                auto it = to_sym.find(x);
                if (it == to_sym.end()) {
                    std::string name = "__EET_GROUP_" + std::to_string(next++);
                    ex sym = GiNaC::symbol(name);
                    to_sym[x] = sym;
                    from_sym[sym] = x;
                    it = to_sym.find(x);
                }
                m[x] = it->second;
                return;
            }
            for (size_t i = 0; i < x.nops(); ++i) collect(x.op(i));
        };
        collect(e);
        return m.empty() ? e : e.subs(m);
    }

    ex restore(const ex& e) const {
        GiNaC::exmap m;
        for (const auto& kv : from_sym) m[kv.first] = kv.second;
        return m.empty() ? e : e.subs(m).normal();
    }
};

// Verification-only expansion. The final result keeps the held groups, but
// two algebraically equal expressions can use different par()/ser() nodes;
// expand both sides just for the equality check.
ex eet_expand_groups(const ex& e) {
    GiNaC::exmap m;
    std::function<void(const ex&)> collect = [&](const ex& x) {
        if (is_parallel(x)) {
            auto a = parallel_args(x);
            if (a.size() == 2) {
                ex av = eet_expand_groups(a[0]);
                ex bv = eet_expand_groups(a[1]);
                m[x] = (av * bv / (av + bv)).normal();
                return;
            }
        }
        if (is_series(x)) {
            auto a = series_args(x);
            if (a.size() == 2) {
                m[x] = (eet_expand_groups(a[0]) +
                        eet_expand_groups(a[1])).normal();
                return;
            }
        }
        for (size_t i = 0; i < x.nops(); ++i) collect(x.op(i));
    };
    collect(e);
    return m.empty() ? e : e.subs(m).normal();
}

bool eet_solve_h(const Circuit& c, const std::string& input,
                 const std::string& output,
                 const std::set<std::string>& used, ex& H,
                 ParamTable& params, std::string* out_desc = nullptr) {
    try {
        AnalysisRequest r;
        r.input_ref = input;
        r.output = output;
        r.used_nodes = used;
        Solved s = solve(c, r);
        H = (s.num / s.den).normal();
        params = std::move(s.params);
        if (out_desc) *out_desc = s.output_desc;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool eet_eligible(const Circuit& c, std::vector<EetElem>& elems) {
    elems.clear();
    for (const auto& cc : c.comps) {
        if (cc.kind == Kind::K || cc.kind == Kind::T) return false;
        if (cc.kind == Kind::C || cc.kind == Kind::L) {
            if (!cc.mirror_ref.empty()) return false;
            if (cc.nodes.size() < 2) return false;
            elems.push_back({cc.ref, cc.ref, "", cc.kind == Kind::C,
                             {cc.nodes[0], cc.nodes[1]}});
            // Passive parasitics change the topology inside the component;
            // leave those to the MNA fallback.
            for (const auto& pd : param_defs(cc.kind))
                if (cc.param_enabled(pd.name)) return false;
        }
        if (cc.param_enabled("GBW") && cc.param_estimate("GBW") > 0.0)
            return false;

        // Device capacitances are two-terminal branches in the small-signal
        // model. Cgs, Cgd and Cds are enough for the common transistor stages;
        // Cdb/Cpi/Cmu are also visible two-terminal branches.
        auto add_cap = [&](const char* name, const std::string& a,
                           const std::string& b) {
            if (!cc.param_enabled(name)) return;
            if (cc.nodes.size() < 3) return;
            elems.push_back({param_symbol(cc, name), cc.ref, name, true,
                             {a, b}});
        };
        switch (cc.kind) {
            case Kind::NMOS:
            case Kind::PMOS:
                add_cap("Cgs", cc.nodes[1], cc.nodes[2]);
                add_cap("Cgd", cc.nodes[1], cc.nodes[0]);
                add_cap("Cds", cc.nodes[0], cc.nodes[2]);
                add_cap("Cdb", cc.nodes[0], cc.nodes[2]);
                break;
            case Kind::NPN:
            case Kind::PNP:
                add_cap("Cpi", cc.nodes[1], cc.nodes[2]);
                add_cap("Cmu", cc.nodes[1], cc.nodes[0]);
                break;
            case Kind::D:
                add_cap("Cd", cc.nodes[0], cc.nodes[1]);
                break;
            default:
                break;
        }
    }
    return !elems.empty() && elems.size() <= 4;
}

Circuit eet_variant(const Circuit& c, const EetElem& elem, int mode) {
    Circuit out = c;
    if (elem.param.empty()) {
        for (size_t i = 0; i < out.comps.size(); ++i) {
            if (out.comps[i].ref != elem.comp) continue;
            if (mode == 0) {
                out.comps.erase(out.comps.begin() + i);
            } else {
                Component replacement = out.comps[i];
                replacement.ref = (mode == 1) ? "__EET_SHORT__"
                                  : (mode == 2) ? "__EET_ONE__"
                                                : "__EET_SMALL__";
                replacement.kind = (mode == 1) ? Kind::V : Kind::R;
                replacement.value_text = (mode == 1) ? "0"
                                        : (mode == 2) ? "1"
                                                      : "0.001";
                replacement.dc_text = (mode == 1) ? "0" : "";
                replacement.ac_text = (mode == 1) ? "0" : "";
                out.comps[i] = replacement;
            }
            break;
        }
        return out;
    }

    for (auto& cc : out.comps) {
        if (cc.ref != elem.comp) continue;
        cc.param_on[elem.param] = false;
        break;
    }
    if (mode != 0) {
        Component replacement;
        replacement.ref = (mode == 1) ? "__EET_SHORT__"
                          : (mode == 2) ? "__EET_ONE__"
                                        : "__EET_SMALL__";
        replacement.kind = (mode == 1) ? Kind::V : Kind::R;
        replacement.nodes = elem.nodes;
        replacement.value_text = (mode == 1) ? "0"
                                : (mode == 2) ? "1"
                                              : "0.001";
        replacement.dc_text = (mode == 1) ? "0" : "";
        replacement.ac_text = (mode == 1) ? "0" : "";
        out.comps.push_back(replacement);
    }
    return out;
}

bool eet_rec(const Circuit& c, std::vector<EetElem> elems,
             const std::string& input, const std::string& output,
             const std::set<std::string>& used, EetGroups& groups,
             ex& H, ParamTable& params, std::string& out_desc, int depth) {
    if (depth > 8) return false;
    if (elems.empty()) {
        if (!eet_solve_h(c, input, output, used, H, params, &out_desc))
            return false;
        H = groups.atomize(H);
        return true;
    }

    for (size_t pick = 0; pick < elems.size(); ++pick) {
        const EetElem elem = elems[pick];
        if (!c.find(elem.comp)) continue;
        const bool is_cap = elem.cap;

        Circuit c_open = eet_variant(c, elem, 0);
        Circuit c_short = eet_variant(c, elem, 1);
        Circuit c_one = eet_variant(c, elem, 2);
        Circuit c_small = eet_variant(c, elem, 3);
        ex H0, Hinf, H1, H2;
        ParamTable p0, pinf, p1, p2;
        bool haveinf = eet_solve_h(c_open, input, output, used, Hinf, pinf);
        bool have0 = false;
        // For a shunt element the open-circuit reference is already non-zero;
        // the shorted solve is unnecessary (and can be singular).
        if (!haveinf || Hinf.is_zero())
            have0 = eet_solve_h(c_short, input, output, used, H0, p0);
        if (!eet_solve_h(c_one, input, output, used, H1, p1)) continue;
        if (!eet_solve_h(c_small, input, output, used, H2, p2)) continue;
        // The replacement resistors are stamped under their reference names;
        // substitute their numeric values before fitting the EET coefficients.
        {
            GiNaC::exmap sub;
            sub[p1.get("__EET_ONE__")] = ex(1);
            H1 = H1.subs(sub).normal();
            GiNaC::exmap sub2;
            sub2[p2.get("__EET_SMALL__")] = ex(1) / ex(1000);
            H2 = H2.subs(sub2).normal();
        }
        H0 = groups.atomize(H0);
        Hinf = groups.atomize(Hinf);
        H1 = groups.atomize(H1);
        H2 = groups.atomize(H2);
        // Prefer the shorted reference (H0) when it exists and is non-zero,
        // but a shorted port can make the network singular (e.g. an output
        // capacitor). Fall back to the open reference; two finite impedance
        // probes then determine Zn/Zd without needing the singular state.
        // Prefer the open-circuit reference whenever it exists: the
        // short-circuit fit has a different algebraic form. The shorted form
        // is only needed for a zero open-circuit transfer (a series element),
        // which is handled by the special case below.
        const bool use_open = haveinf && !Hinf.is_zero();
        const bool use_short = !use_open && have0 && !H0.is_zero();
        if (!use_short && !use_open) continue;
        ex Href = use_open ? Hinf : H0;
        Circuit cref = use_open ? c_open : c_short;

        // For H(Z) = Href * (1 + Z/Zn)/(1 + Z/Zd) (short reference) or
        // Href * (1 + Zn/Z)/(1 + Zd/Z) (open reference), two finite probes
        // Z1 = 1 ohm and Z2 = 0.001 ohm solve the linear fractional fit:
        //   A = Zn, D = Zd,
        //   A = (r_i - 1) Zi + r_i D,   r_i = H(Zi)/Href.
        const ex Z1 = ex(1), Z2 = ex(1) / ex(1000);
        ex Zn, Zd;
        bool zn_inf = false, zd_inf = false;
        if (use_short && Hinf.is_zero()) {
            // H = H0 * (1 + Z/Zn)/(1 + Z/Zd); a zero open-circuit transfer
            // means Zn is infinite and only Zd is needed.
            ex r = (H1 / Href).normal();
            if ((r - 1).is_zero()) continue;
            zn_inf = true;
            Zd = (Z1 / (1 / r - 1)).normal();
        } else if (!use_short && H0.is_zero()) {
            // H = Hinf * (1 + Zn/Z)/(1 + Zd/Z); a zero short-circuit
            // transfer means Zn = 0 and only Zd is needed.
            ex r = (H1 / Href).normal();
            if ((r - 1).is_zero()) continue;
            Zn = ex(0);
            Zd = (Z1 * (1 / r - 1)).normal();
        } else {
            ex r1 = (H1 / Href).normal();
            ex r2 = (H2 / Href).normal();
            ex den = (r1 - r2).normal();
            if (den.is_zero()) continue;
            ex D = (((r2 - 1) * Z2 - (r1 - 1) * Z1) / den).normal();
            ex A = ((r1 - 1) * Z1 + r1 * D).normal();
            Zn = A;
            Zd = D;
        }

        ex zsym = p1.get(elem.key);
        ex Z = is_cap ? (ex(1) / (p1.get("s") * zsym)).normal()
                      : (p1.get("s") * zsym).normal();
        ex F;
        if (use_short) {
            ex n = zn_inf ? ex(1) : (ex(1) + Z / Zn).normal();
            ex d = zd_inf ? ex(1) : (Zd.is_zero() ? ex(1)
                                                  : (ex(1) + Z / Zd).normal());
            F = (n / d).normal();
        } else {
            ex n = zn_inf ? ex(1) : (Zn.is_zero() ? ex(1)
                                                  : (ex(1) + Zn / Z).normal());
            ex d = zd_inf ? ex(1) : (Zd.is_zero() ? ex(1)
                                                  : (ex(1) + Zd / Z).normal());
            F = (n / d).normal();
        }

        std::vector<EetElem> rest;
        for (size_t i = 0; i < elems.size(); ++i)
            if (i != pick) rest.push_back(elems[i]);
        ex Href_f;
        ParamTable pref;
        if (rest.empty()) {
            Href_f = Href;
        } else if (!eet_rec(cref, rest, input, output, used, groups, Href_f,
                            pref, out_desc, depth + 1)) {
            continue;
        }
        ex candidate = (Href_f * F).normal();
        // Verify against the ordinary exact solve (after restoring the held
        // groups for the comparison). If the fit is not exact, MNA/LowEntropy
        // remains the safe fallback.
        ex exact;
        ParamTable pexact;
        std::string od;
        if (!eet_solve_h(c, input, output, used, exact, pexact, &od)) continue;
        if (!(eet_expand_groups(groups.restore(candidate)) -
              eet_expand_groups(exact)).normal().is_zero())
            continue;
        H = candidate;
        params = pexact;
        out_desc = od;
        return true;
    }
    return false;
}

bool eet_transfer(const Circuit& c, const std::string& input,
                  const std::string& output,
                  const std::set<std::string>& used, EetTF& out) {
    std::vector<EetElem> elems;
    if (!eet_eligible(c, elems)) return false;
    ex H;
    ParamTable params;
    std::string desc;
    EetGroups groups;
    if (!eet_rec(c, elems, input, output, used, groups, H, params, desc, 0))
        return false;
    H = groups.restore(H);
    out.ok = true;
    out.num = H.numer();
    out.den = H.denom();
    out.params = std::move(params);
    out.out_desc = desc;
    return true;
}

// H(s) = num/den of V(node) per V(source), returned raw (unpruned).
struct RawTF {
    ex num, den;
    ParamTable params;
    std::string out_desc;
    // Zero-value time constants of the network (TTC ordering); computed here so
    // every analysis that prunes a transfer function gets the physical
    // element-to-pole attribution.
    std::vector<TimeConstant> octc;
    // A return-ratio result can carry its low-entropy presentation separately
    // from the exact raw numerator/denominator. This keeps the plot/metrics
    // numerically identical while avoiding a second generic prune.
    bool ready_pruned = false;
    Pruned ready;
};

// Return-ratio H(s) needs the ordinary symbolic solve for its H_inf, beta and
// H0 pieces. This guard keeps those inner solves from recursively attempting
// another return-ratio extraction.
thread_local bool g_return_ratio_active = false;

// Try to assemble H(s) from a feedback loop (Rosenstark/Mason). Declared here
// because analyze_tf() is defined before the return-ratio machinery.
bool try_return_ratio_tf(const Circuit& c, const AnalysisSpec& s, RawTF& out);
ex pruned_to_expr(const Pruned& p);

RawTF raw_tf(const Circuit& c, const std::string& input_ref,
             const std::string& output,
             const std::set<std::string>& used_nodes = {},
             bool allow_eet = true) {
    RawTF t;
    // Prefer the graph/EET construction for small explicit-RLC networks: it
    // builds a factored expression directly. It is verified against MNA, so
    // unsupported topologies fall through to the existing solver.
    EetTF eet;
    if (allow_eet && eet_transfer(c, input_ref, output, used_nodes, eet)) {
        t.num = std::move(eet.num);
        t.den = std::move(eet.den);
        t.params = std::move(eet.params);
        t.out_desc = eet.out_desc.empty() ? output : eet.out_desc;
    } else {
        AnalysisRequest r;
        r.input_ref = input_ref;
        r.output = output;
        r.used_nodes = used_nodes;
        Solved sv = solve(c, r);
        t.num = sv.num;
        t.den = sv.den;
        t.params = std::move(sv.params);
        t.out_desc = sv.output_desc;
    }
    // OCTC must fold exactly as the main solve did: pass the same used-node set
    // (the caller's extras plus the output node).
    std::set<std::string> octc_used = used_nodes;
    if (output.size() > 3 && output.front() == 'V' && output.back() == ')')
        octc_used.insert(output.substr(2, output.size() - 3));
    t.octc = open_circuit_time_constants(c, input_ref, t.params, octc_used);
    return t;
}

CardResult make_transfer(const RawTF& t, const AnalysisSpec& s,
                         const std::string& title) {
    CardResult cr;
    cr.kind = s.kind;
    cr.title = title;
    cr.has_transfer = true;
    PruneOptions o = opts_of(s); // use_parallel is hardcoded on inside
    o.octc = t.octc;

    AnalysisRequest req = req_of(s, "V1", "V(out)");
    // Rebuild an AnalysisResult so the GUI (Bode etc.) can use it directly.
    AnalysisResult res;
    res.input_desc = "source";
    res.output_desc = t.out_desc;
    res.num_raw = t.num;
    res.den_raw = t.den;
    res.params = t.params;
    res.opts = o;
    res.sweep = s.sweep;
    res.octc = t.octc;
    res.pruned = t.ready_pruned
                     ? t.ready
                     : prune_low_entropy(t.num, t.den, res.params, o);
    res.report = format_report(res);

    cr.transfer = res;
    cr.text = res.pruned.text;
    cr.latex = res.pruned.latex;
    cr.summary = res.output_desc + " -- " + res.pruned.text;
    cr.report = res.report;
    cr.latex_report = format_report_latex(res);
    return cr;
}

} // namespace

// The right-hand side of a low-entropy LaTeX string that already carries an
// "H(s) = " prefix (Pruned stores the prefixed form).
std::string latex_rhs(const std::string& full) {
    const std::string pfx = "H(s) = ";
    if (full.rfind(pfx, 0) == 0) return full.substr(pfx.size());
    return full;
}

// The same string presented under a different left-hand side: a low-entropy
// result is a polynomial ratio, not necessarily a transfer function, so the
// card names the quantity it actually measured (V(out), Zin(s), Isc(out), ...).
std::string relabel_latex(const std::string& latex, const std::string& label) {
    const std::string pfx = "H(s) = ";
    if (latex.rfind(pfx, 0) == 0) return label + " = " + latex.substr(pfx.size());
    return latex;
}

// Drop the leading "H(s) = ..." line from a report, keeping the rest (used
// when the caller already printed the expression above).
std::string strip_first_line(const std::string& report) {
    const std::string pfx = "H(s) = ";
    if (report.rfind(pfx, 0) != 0) return report;
    size_t nl = report.find('\n');
    return nl == std::string::npos ? std::string() : report.substr(nl + 1);
}

// ---------------------------------------------------------------------------
// s-domain transfer function
// ---------------------------------------------------------------------------
CardResult analyze_tf(const Circuit& c, const AnalysisSpec& s) {
    RawTF t;
    // First look for a forward path plus a feedback loop and use the
    // return-ratio assembly. The result is numerically verified against a
    // direct MNA solve before it is accepted, so a topology that is not a
    // single-reference feedback loop falls back to the ordinary solver.
    if (s.kind == AnalysisKind::TransferFunction && !g_return_ratio_active &&
        try_return_ratio_tf(c, s, t)) {
        return make_transfer(t, s, "Transfer function");
    }
    t = raw_tf(c, s.input_ref, s.output);
    CardResult cr = make_transfer(t, s, "Transfer function");
    return cr;
}

// ---------------------------------------------------------------------------
// AC: small-signal response to the AC values of every independent source,
// superposed. There is no single "input": each source contributes its own
// transfer to the chosen output scaled by its AC value, so the result reads
//   V(out) = V1*H_1(s) + V2*H_2(s) + ...
// with the source symbols kept in the answer. The output is a node voltage or
// a branch current, not a ratio.
// ---------------------------------------------------------------------------
CardResult analyze_ac(const Circuit& c, const AnalysisSpec& s) {
    CardResult cr;
    cr.kind = AnalysisKind::AC;
    cr.title = "AC (small-signal)";

    // Collect every independent voltage/current source and its AC value. VDD
    // is a supply rail, not a signal source: it is AC ground and never
    // contributes to the AC superposition.
    struct Src {
        const Component* comp;
        std::string ref;
        double ac;
    };
    std::vector<Src> srcs;
    for (const auto& cc : c.comps) {
        if (cc.kind != Kind::V && cc.kind != Kind::I) continue;
        double ac = 1.0;
        if (!eng::parse_value(cc.ac_text, ac)) ac = 1.0;
        srcs.push_back({&cc, cc.ref, ac});
    }
    if (srcs.empty()) {
        cr.summary = "AC: no independent sources to excite";
        cr.report = cr.summary + "\n";
        return cr;
    }

    // H_i(s) = output per unit of source i. The physical response is the
    // source's own symbolic value (V1, I1, ...) times its AC value, so the
    // result reads V(out) = V1*H_1(s) + V2*H_2(s) + ... -- the sources are
    // part of the answer, not silently divided out.
    ex num = 0;
    ex le_num = 0;
    bool all_low_entropy = true;
    ParamTable params;
    std::string out_desc;
    double w0 = 2.0 * M_PI * s.f0_hz;
    std::vector<std::pair<std::string, std::string>> contrib; // ref -> dB / off
    std::vector<std::string> term_text; // per-source symbolic term
    for (const auto& src : srcs) {
        AnalysisRequest r;
        r.input_ref = src.ref;
        r.output = s.output;
        if (s.output.size() > 3 && s.output.front() == 'V' &&
            s.output.back() == ')')
            r.used_nodes.insert(s.output.substr(2, s.output.size() - 3));
        ex h;
        ex h_le = 0;
        bool have_le = false;
        ParamTable src_params;
        std::string src_desc;
        RawTF rr;
        AnalysisSpec acspec = s;
        acspec.kind = AnalysisKind::AC;
        acspec.input_ref = src.ref;
        if (src.ac != 0.0 && !g_return_ratio_active &&
            try_return_ratio_tf(c, acspec, rr)) {
            h = (rr.num / rr.den).normal();
            src_params = rr.params;
            src_desc = rr.out_desc;
            if (rr.ready_pruned) {
                h_le = pruned_to_expr(rr.ready);
                have_le = true;
            }
        } else {
            Solved sv = solve(c, r);
            h = (sv.num / sv.den).normal();
            src_params = sv.params;
            src_desc = sv.output_desc;
        }
        if (params.syms.empty()) params = src_params;
        out_desc = src_desc;
        ex term = 0;
        ex le_term = 0;
        if (src.ac != 0.0) {
            // The source's value symbol. Its numeric estimate is the AC
            // amplitude (what actually drives the node), not the DC value.
            ex vsym = value_symbol_scaled(params, c, *src.comp);
            params.set(src.ref, std::abs(src.ac),
                       value_unit_class(src.comp->kind));
            term = (ex(src.ac) * vsym * h).normal();
            if (have_le) {
                le_term = (ex(src.ac) * vsym * h_le).normal();
            } else {
                all_low_entropy = false;
            }
        }
        num = (num + term).normal();
        le_num = (le_num + le_term).normal();
        // A source with a zero AC value is off, not -inf dB.
        char b2[64];
        if (src.ac == 0.0)
            std::snprintf(b2, sizeof(b2), "off");
        else
            std::snprintf(b2, sizeof(b2), "%.3g dB",
                          eval_mag_db(h, src_params, w0) +
                              20.0 * std::log10(std::abs(src.ac)));
        contrib.push_back({src.ref, b2});
        // Print the term in the same s-ordering as the main result. Use the
        // factored return-ratio form when available.
        ex shown = have_le ? le_term : term;
        term_text.push_back(
            pretty_ratio(shown.numer(), shown.denom(), params.get("s")));
    }

    // Low-entropy prune of the combined response. Normalize the denominator so
    // the result factors as gain * (1 + s*tau)...; for a response the scale is
    // simply folded into the reported gain K.
    PruneOptions o = opts_of(s);
    Pruned p;
    if (all_low_entropy && !le_num.is_zero())
        p = prune_low_entropy(le_num.numer(), le_num.denom(), params, o);
    else
        p = prune_low_entropy(num.numer(), num.denom(), params, o);

    // The measured quantity is the output itself, not a ratio: name it so.
    const std::string label = out_desc;
    cr.has_transfer = true;
    {
        AnalysisResult res;
        res.label = label;
        res.input_desc = "AC";
        res.output_desc = out_desc;
        res.num_raw = num.numer();
        res.den_raw = num.denom();
        res.params = params;
        res.opts = o;
        res.sweep = s.sweep;
        res.pruned = p;
        res.report = format_report(res);
        cr.transfer = res;
    }
    cr.text = label + " = " + p.text;
    cr.latex = relabel_latex(p.latex, label);
    // `num` is already a reduced ratio of polynomials, so it is evaluated as
    // it stands (there is no separate denominator).
    double mag = eval_mag_db(num, params, w0);
    double ph = eval_phase_deg(num, params, w0);
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%.3g dB @ %.4g Hz, phase %.3g deg", mag,
                  s.f0_hz, ph);
    cr.values.push_back({"at f0", buf});
    for (const auto& cc : contrib) cr.values.push_back({cc.first, cc.second});

    std::string rep =
        "AC (small-signal) -- superposition of the sources' AC values\n";
    rep += "output: " + out_desc + "\n";
    rep += "----------------------------------------\n";
    rep += "  " + label + " = " + p.text + "\n";
    rep += "  at f0: " + std::string(buf) + "\n";
    rep += "Per-source terms:\n";
    for (size_t i = 0; i < srcs.size(); ++i) {
        if (srcs[i].ref.rfind("__", 0) == 0) continue; // internal test source
        char b[96];
        std::snprintf(b, sizeof(b), "  %s (AC %g): ", srcs[i].ref.c_str(),
                      srcs[i].ac);
        rep += std::string(b) +
               (srcs[i].ac == 0.0 ? std::string("off") : term_text[i]) + "\n";
    }
    rep += "Per-source contributions at f0:\n";
    for (size_t i = 0; i < contrib.size(); ++i) {
        if (contrib[i].first.rfind("__", 0) == 0) continue;
        rep += "  " + contrib[i].first + ": " + contrib[i].second + "\n";
    }
    cr.report = rep;
    cr.summary = cr.text + "   (" + buf + ")";
    {
        std::ostringstream lr;
        lr << "AC (superposition of the sources' AC values):\n";
        // The output description can carry a branch name with underscores
        // (I(__SHORT__)); in LaTeX an underscore starts a subscript, so it
        // has to be escaped or MathJax reports "Missing open brace for
        // subscript".
        lr << latex_escape_underscores(label) << " = " << latex_rhs(p.latex)
           << "\n";
        cr.latex_report = lr.str();
    }
    return cr;
}

// ---------------------------------------------------------------------------
// DC Mode 3: turn a numeric operating point into a report, and (optionally)
// override each device's small-signal parameters from it. The override is the
// point of the mode: e.g. a real bias may make a device's Cds far smaller than
// the user's guess, so a following symbolic analysis prunes it by the normal
// threshold rule.
// ---------------------------------------------------------------------------
CardResult dc_numeric_report(const Circuit& c, const AnalysisSpec& s,
                             const NumericDcResult& nr) {
    CardResult cr;
    cr.kind = AnalysisKind::DC;
    cr.title = "DC operating point (numeric)";

    cr.report = "DC analysis -- numeric operating point (square law: uCox with "
                "symbolic W/L)\n";
    cr.report += "----------------------------------------\n";

    // Node voltages, sorted.
    std::vector<std::string> names;
    for (const auto& kv : nr.node_v) names.push_back(kv.first);
    std::sort(names.begin(), names.end());
    for (const auto& nd : names) {
        char b[64];
        std::snprintf(b, sizeof(b), "%.6g V", nr.node_v.at(nd));
        cr.values.push_back({"V(" + nd + ")", b});
        cr.report += "  V(" + nd + ") = " + b + "\n";
    }
    if (!nr.mos.empty()) {
        cr.report += "\nDevice operating points:\n";
        for (const auto& op : nr.mos) {
            char b[256];
            std::snprintf(b, sizeof(b),
                          "  %s: Vgs=%.4g Vds=%.4g Vdsat=%.4g Id=%.4g A "
                          "gm=%.4g S%s\n",
                          op.ref.c_str(), op.vgs, op.vds, op.vov, op.id, op.gm,
                          op.saturated ? "" : "  [TRIODE]");
            cr.report += b;
            char ib[32];
            std::snprintf(ib, sizeof(ib), "%.6g A", op.id);
            cr.values.push_back({"I(" + op.ref + ")", ib});
        }
    }

    cr.summary = cr.report.substr(cr.report.find('\n') + 1);
    cr.latex_report = "DC operating point (numeric):\n";
    for (const auto& kv : cr.values)
        cr.latex_report += "\\mathrm{" + kv.first + "} = \\mathrm{" +
                           kv.second + "}\n";
    cr.latex.clear();
    return cr;
}

// ---------------------------------------------------------------------------
// DC Modes 1 & 2: symbolic operating point. Every source is stamped with its DC
// value. Mode 1 (gm/Id): each MOSFET's Id is an unknown and Vgs = 2*Id/gm + Vth.
// Mode 2 (square law): Vov is a symbolic design variable and
// Id = 1/2*uCox*(W/L)*Vov^2. Both check saturation: a device pushed into triode
// aborts the analysis and is named.
// Present a DC value's sum with the simple terms first, so a KVL equation reads
// as it is written by hand: "VDD - R1*I1" instead of "-R1*I1 + VDD". Only the
// top-level +/- of the string are reordered; anything inside parentheses (a
// product, a ratio, sqrt(...)) is left alone.
static std::string dc_order(const std::string& s) {
    std::vector<std::string> terms;
    int depth = 0;
    std::string cur;
    for (size_t i = 0; i < s.size(); ++i) {
        char ch = s[i];
        if (ch == '(') ++depth;
        else if (ch == ')') --depth;
        if ((ch == '+' || ch == '-') && depth == 0 && !cur.empty()) {
            terms.push_back(cur);
            cur = std::string(1, ch);
        } else {
            cur += ch;
        }
    }
    if (!cur.empty()) terms.push_back(cur);
    if (terms.size() < 2) return s;
    auto trim = [](std::string t) {
        while (!t.empty() && (t.front() == ' ' || t.front() == '\t'))
            t.erase(t.begin());
        while (!t.empty() && (t.back() == ' ' || t.back() == '\t')) t.pop_back();
        return t;
    };
    for (auto& t : terms) t = trim(t);
    auto simple = [](const std::string& t) {
        std::string b = t;
        if (!b.empty() && (b[0] == '+' || b[0] == '-')) b = b.substr(1);
        return b.find('*') == std::string::npos &&
               b.find('/') == std::string::npos &&
               b.find('^') == std::string::npos;
    };
    std::vector<std::string> out;
    for (const auto& t : terms)
        if (simple(t)) out.push_back(t);
    for (const auto& t : terms)
        if (!simple(t)) out.push_back(t);
    std::string r;
    for (size_t i = 0; i < out.size(); ++i) {
        const std::string& t = out[i];
        std::string body = (!t.empty() && (t[0] == '+' || t[0] == '-')) ? t.substr(1)
                                                                        : t;
        if (i == 0) {
            r += body;
        } else if (!t.empty() && t[0] == '-') {
            r += " - " + body;
        } else {
            r += " + " + body;
        }
    }
    return r;
}

// ---------------------------------------------------------------------------
CardResult analyze_dc(const Circuit& c, const AnalysisSpec& s) {
    CardResult cr;
    cr.kind = AnalysisKind::DC;

    // ---- Modes 1 and 2: symbolic operating point ----
    cr.title = "DC operating point (large-signal)";

    // Mode 2: unless "assume all in saturation" is set, a numerical pre-solve
    // decides each device's operating region. The output itself stays SYMBOLIC
    // -- the region only selects which device relation is reported (the square
    // law, or the triode Id = uCox*(W/L)*(Vdsat*Vds - Vds^2/2)).
    std::map<std::string, bool> region_sat; // ref -> saturated?
    bool have_region = false;
    std::string region_note;
    if (s.tech.dc_mode == DcMode::SquareLaw && !s.tech.assume_saturation) {
        MosModel nm, pm;
        nm.level = 1;
        nm.vto = s.tech.vthn;
        nm.kp = s.tech.uncox;
        pm.level = 1;
        pm.pmos = true;
        pm.vto = s.tech.vthp;
        pm.kp = s.tech.upcox;
        NumericDcResult nr = solve_dc_numeric(c, nm, pm, s.tech.vdd);
        if (nr.ok) {
            have_region = true;
            for (const auto& op : nr.mos) region_sat[op.ref] = op.saturated;
        } else {
            // Say so rather than silently treating every device as saturated:
            // with the checkbox off the user is asking for the region check,
            // and a silent fallback makes the option look inert.
            region_note = "region check unavailable (" + nr.error +
                          "): every device is reported as saturated.\n";
        }
    }

    DcSolution dc;
    try {
        dc = solve_dc(c, s.tech);
    } catch (const std::exception& e) {
        cr.summary = std::string("DC solve failed: ") + e.what();
        cr.report = cr.summary + "\n";
        return cr;
    }

    ParamTable& pt = dc.params;

    // If the numeric region solve did not converge, judge the region from the
    // *symbolic* operating point instead: Vds against Vov at the parameter
    // estimates. This is the same post-hoc rule mode 1 already uses, and it is
    // all the numeric solve was ever needed for -- a device with Vds = 0 and a
    // large Vgs is in triode whatever the exact numbers, so an unconverged
    // numeric solve must not make "assume all in saturation" inert.
    if (s.tech.dc_mode == DcMode::SquareLaw && !s.tech.assume_saturation &&
        !have_region) {
        for (const auto& ref : dc.mosfets) {
            const Component* mc = c.find(ref);
            const bool pmos = mc && mc->kind == Kind::PMOS;
            double vds_n = eval_complex(dc.vds[ref], pt, 0.0).real();
            double vov_n = eval_complex(dc.vov[ref], pt, 0.0).real();
            if (!std::isfinite(vds_n) || !std::isfinite(vov_n)) continue;
            region_sat[ref] = pmos ? (vds_n <= vov_n) : (vds_n >= vov_n);
            have_region = true;
        }
        if (have_region)
            region_note = "region check: numeric solve unavailable, regions "
                          "judged from the symbolic operating point.\n";
    }

    // Mode 2 with a region check: a device the check found in triode keeps its
    // KCL branch current -- the current through a cascode/mirror leg is the
    // reference current even when the device law is the triode one. Only the
    // presented *relation* and the [TRIODE] flag change (handled downstream).

    // Named device overdrives: each device's overdrive expression (e.g.
    // V1 - Vthn) mapped to a symbol Vdsat_<ref>, so the operating point reads
    // Vdsat_M1 instead of the raw difference. Only the gm/Id mode names it; the
    // square-law mode keeps (Vgs - Vth)^2 visible.
    std::vector<std::pair<ex, ex>> od_subs;
    if (s.tech.dc_mode == DcMode::GmOverId) {
        for (const auto& ref : dc.mosfets) {
            ex od = dc.vov[ref];
            if (od.is_zero()) continue;
            od_subs.push_back({od, pt.get("Vdsat_" + ref)});
        }
    }

    // Factor the additive terms of a sum without combining them back into an
    // expanded polynomial: -V1*gm*R1 + Vth*gm*R1 -> -gm*R1*(V1 - Vth). GiNaC's
    // factor() throws on a non-polynomial (a sqrt/radical): fall back then.
    auto factor_terms = [](const ex& x) -> ex {
        if (GiNaC::is_a<GiNaC::add>(x)) {
            ex sum = 0;
            for (size_t i = 0; i < x.nops(); ++i)
                try {
                    sum += GiNaC::factor(x.op(i));
                } catch (const std::exception&) {
                    sum += x.op(i);
                }
            return sum;
        }
        try {
            return GiNaC::factor(x);
        } catch (const std::exception&) {
            return x;
        }
    };

    // True when an expression contains a factored square, pow(add(...), k>=2),
    // e.g. (Vgs - Vth)^2: the low-entropy form we want to prefer over an
    // expanded polynomial.
    std::function<bool(const ex&)> factored_square = [&](const ex& e) -> bool {
        if (GiNaC::is_a<GiNaC::power>(e)) {
            const ex& b = e.op(0);
            const ex& p = e.op(1);
            if (GiNaC::is_a<GiNaC::add>(b) && GiNaC::is_a<GiNaC::numeric>(p) &&
                GiNaC::ex_to<GiNaC::numeric>(p).is_integer() &&
                GiNaC::ex_to<GiNaC::numeric>(p).to_double() >= 2.0)
                return true;
        }
        for (size_t i = 0; i < e.nops(); ++i)
            if (factored_square(e.op(i))) return true;
        return false;
    };

    // Put one operating-point value into a design-readable form:
    //   1. combine over a common denominator (exact rationals, so 1/2 stays
    //      1/2, not 0.5);
    //   2. keep a *factored* structure: collect a device coefficient (gm_M1,
    //      unCox, ...) and factor each additive term, so a node voltage reads
    //      VDD - 1/2*R1*gm_M1*Vdsat_M1 rather than the expanded polynomial;
    //   3. name each device overdrive (Vdsat_<ref>);
    //   4. recover parallel structure: R1*R2/(R1+R2) -> R1||R2.
    auto dc_value = [&](ex v, bool name_od = true) -> ex {
        if (!s.prune) return v.normal();
        // Expand first: an operating-point value is usually a single fraction
        // (mul), in which case the overdrive 2*Id/gm is not a subexpression at
        // all and can never be named. Expanding splits it into a sum whose
        // terms do contain the overdrive.
        ex out = v.expand();
        // Name each device overdrive (Vdsat_<ref>) before any factoring, so
        // every use of it reads as the named symbol. Substituting only inside
        // the per-coefficient candidates left a node voltage in the raw
        // 2*Id/gm form whenever that candidate did not win on length.
        if (name_od) {
            for (const auto& sub : od_subs) {
                if (out.has(sub.first))
                    out = out.subs(sub.first == sub.second);
                else if (out.has(-sub.first))
                    out = out.subs((-sub.first) == (-sub.second));
            }
        }
        out = out.normal();
        ex num = out.numer().expand();
        ex den = out.denom().expand();
        // GiNaC's factor()/collect() can explode on a large multivariate
        // polynomial (e.g. once channel-length modulation adds the ro terms), so
        // only run the factoring pass when both parts are small.
        const bool small = num.nops() <= 20 && den.nops() <= 20;
        if (small) {
            try {
                if (!is_a<GiNaC::numeric>(num)) num = GiNaC::factor(num);
                if (!is_a<GiNaC::numeric>(den)) den = GiNaC::factor(den);
            } catch (const std::exception&) {
                // Non-polynomial (a radical): keep the expanded form.
            }
        }
        ex best = (num / den).normal();
        // Prefer a factored presentation when it prints shorter: collect by
        // each device coefficient and factor the additive terms (so a node
        // voltage reads VDD - 1/2*R1*gm_M1*Vdsat_M1 or ...*(Vgs - Vth)^2 rather
        // than the expanded polynomial).
        for (const auto& kv : pt.syms) {
            if (!small) break;
            const ex& sym = kv.second;
            if (!num.has(sym)) continue;
            ex ratio2;
            try {
                ex cand = factor_terms(num.collect(sym));
                ratio2 = cand / den;
            } catch (const std::exception&) {
                continue;
            }
            std::string rs = pretty(ratio2), bs = pretty(best);
            bool better = rs.size() < bs.size() ||
                          (factored_square(ratio2) && !factored_square(best));
            if (better) best = ratio2;
        }
        // Recover R1||R2 only when it makes the expression shorter (to_parallel
        // re-expands, which would otherwise undo the factored form above).
        ex parred = to_parallel(best);
        if (!parred.is_equal(best) && pretty(parred).size() <= pretty(best).size())
            return parred;
        return best;
    };

    // ---- saturation check (Mode 1) ---------------------------------------
    // Mode 1 solves Vgs = 2*Id/gm + Vth, so a device can land in triode. Rather
    // than aborting (which hid the whole operating point), flag each triode
    // device in the report so V(n1), Vdsat, ... stay visible.
    std::map<std::string, bool> triode1;
    if (s.tech.dc_mode == DcMode::GmOverId) {
        for (const auto& ref : dc.mosfets) {
            const Component* mc = c.find(ref);
            const bool pmos = mc && mc->kind == Kind::PMOS;
            double vds_n = eval_complex(dc.vds[ref], pt, 0.0).real();
            double vov_n = eval_complex(dc.vov[ref], pt, 0.0).real();
            if (!std::isfinite(vds_n) || !std::isfinite(vov_n)) continue;
            bool saturated = pmos ? (vds_n <= vov_n) : (vds_n >= vov_n);
            triode1[ref] = !saturated;
        }
    }

    // ---- report every node voltage, plus each device's operating point ------
    std::vector<ex> latex_vals;
    std::vector<std::string> latex_names;

    std::vector<std::string> node_names;
    // The device-parameter solve: node voltages in terms of the device
    // overdrives (Vdsat_M<ref>), the sources and the component values.
    const std::map<std::string, ex>& node_v = dc.node_v;
    for (const auto& kv : node_v) node_names.push_back(kv.first);
    std::sort(node_names.begin(), node_names.end());
    for (const auto& nd : node_names) {
        ex vp = dc_value(node_v.at(nd));
        cr.values.push_back({"V(" + nd + ")", dc_order(pretty(vp))});
        latex_vals.push_back(vp);
        latex_names.push_back("V(" + nd + ")");
    }
    for (const auto& ref : dc.mosfets) {
        ex id = dc_value(dc.id[ref]);
        cr.values.push_back({"I(" + ref + ")", dc_order(pretty(id))});
        latex_vals.push_back(id);
        latex_names.push_back("I(" + ref + ")");
    }

    std::string summary;
    for (const auto& kv : cr.values) {
        if (!summary.empty()) summary += ",   ";
        summary += kv.first + " = " + kv.second;
    }
    cr.summary = summary.empty() ? "(no nodes)" : summary;

    // The Math tab shows `latex_report` (one line per value, mirroring the
    // text). There is no single "headline" expression for a multi-value
    // operating point, so leave `latex` empty rather than emit an aligned
    // block the renderer cannot lay out cleanly.
    cr.latex.clear();

    std::string rep = "DC analysis -- large-signal operating point\n";
    if (s.tech.dc_mode == DcMode::SquareLaw) {
        rep += "square-law mode: Id = 1/2*uCox*(W/L)*(Vgs - Vth)^2, saturation "
               "assumed\n";
        rep += "Vth is Vthn for an NMOS, Vthp for a PMOS; W/L are symbolic "
               "(mirror-aware)\n";
    } else {
        rep += "gm/Id mode: Vgs = 2*Id/gm + Vth, saturation assumed\n";
        rep += "Vth is Vthn for an NMOS, Vthp for a PMOS; Vdsat = 2*Id/gm\n";
    }
    rep += "pruning threshold = " + std::to_string(int(s.threshold_db)) + " dB\n";
    if (!region_note.empty()) rep += region_note;
    rep += "----------------------------------------\n";
    for (const auto& kv : cr.values)
        rep += "  " + kv.first + " = " + kv.second + "\n";
    if (!dc.mosfets.empty()) {
        rep += "\nDevice operating points:\n";
        for (const auto& ref : dc.mosfets) {
            const Component* mc = c.find(ref);
            const char* th = (mc && mc->kind == Kind::PMOS) ? "Vthp" : "Vthn";
            // Region: prefer the numeric region check (mode 2); otherwise fall
            // back to evaluating the symbolic operating point.
            bool in_triode = false;
            bool have_dev_region = false;
            if (s.tech.dc_mode == DcMode::GmOverId) {
                auto it = triode1.find(ref);
                if (it != triode1.end()) {
                    in_triode = it->second;
                    have_dev_region = true;
                }
            }
            if (have_region) {
                auto it = region_sat.find(ref);
                if (it != region_sat.end()) {
                    in_triode = !it->second;
                    have_dev_region = true;
                }
            }
            const char* triode_note = in_triode ? "   [TRIODE]" : "";
            const bool sq = (s.tech.dc_mode == DcMode::SquareLaw);
            rep += "  " + ref + ": Vgs = " + pretty(dc_value(dc.vgs[ref])) +
                   ", Vds = " + pretty(dc_value(dc.vds[ref])) +
                   (sq ? ", Vov = " : ", Vdsat = ") +
                   pretty(dc_value(dc.vov[ref], false)) +
                   ", Id = " + pretty(dc_value(dc.id[ref])) + triode_note + "\n";
            if (s.tech.dc_mode == DcMode::SquareLaw) {
                if (in_triode)
                    rep += std::string("      relation: Id = uCox*(W/L)*((Vgs - ") +
                           th + ")*Vds - Vds^2/2)\n";
                else
                    rep += std::string("      relation: Id = 1/2*uCox*(W/L)*(Vgs - ") +
                           th + ")^2" +
                           (s.tech.include_clm ? "*(1 + lambda*Vds)" : "") +
                           "\n";
            } else {
                rep += std::string("      relation: Vdsat_") + ref + " = 2*Id_" +
                       ref + "/gm_" + ref + " = Vgs - " + th;
                if (s.tech.include_clm)
                    rep += ", lambda_" + ref + " = 1/(ro_" + ref + "*Id_" + ref +
                           ")";
                rep += "\n";
            }
        }
    }
    cr.report = rep;
    cr.text = cr.summary;
    {
        std::ostringstream lr;
        lr << "DC operating point (large-signal):\n";
        for (size_t i = 0; i < latex_vals.size(); ++i)
            lr << "\\mathrm{" << latex_names[i] << "} = "
               << to_latex(latex_vals[i]) << "\n";
        cr.latex_report = lr.str();
    }
    return cr;
}

// ---------------------------------------------------------------------------
// PSR / PSRR: excitation = the supply rail (VDD); PSR = H(VDD->out),
// PSRR = H(Vin->out) / H(VDD->out).
// ---------------------------------------------------------------------------
CardResult analyze_psrr(const Circuit& c, const AnalysisSpec& s) {
    // PSR: drive the supply. The Kind::VDD symbol now stamps as an ideal
    // voltage source from VDD to ground, so the supply rail IS the input.
    // Drive it by re-pointing the analysis input at "VDD" (the supply net)
    // and running H(s) the same way as for any other source.
    bool has_vdd = false;
    for (const auto& cc : c.comps) {
        if (cc.kind == Kind::VDD) has_vdd = true;
        for (const auto& n : cc.nodes)
            if (n == "VDD") has_vdd = true;
    }

    CardResult cr;
    cr.kind = AnalysisKind::PSRR;
    cr.title = "PSR / PSRR";
    if (!has_vdd) {
        cr.summary = "no VDD rail in this circuit (place a VDD symbol)";
        cr.report = cr.summary + "\n";
        return cr;
    }

    // Signal path: H_sig = Vout/Vin (input = user-selected source)
    RawTF sig = raw_tf(c, s.input_ref, s.output);

    // Supply path: drive the VDD rail directly. VDD stamps as an ideal V
    // source so the rail is the input -- no test source needed (and adding
    // one would over-constrain the MNA). Use the VDD symbol's own ref as
    // the input so the engine treats it as the excitation.
    std::string vdd_ref;
    for (const auto& cc : c.comps) {
        if (cc.kind == Kind::VDD) { vdd_ref = cc.ref; break; }
    }
    if (vdd_ref.empty()) vdd_ref = "VDD";
    RawTF psr = raw_tf(c, vdd_ref, s.output);

    ex s_sym = sig.params.get("s");
    ex Hsig = (sig.num / sig.den).normal();
    ex Hpsr = (psr.num / psr.den).normal();
    ex psrr;
    bool psrr_ok = true;
    if (Hpsr.is_zero()) {
        psrr = ex(0);
        psrr_ok = false;
    } else {
        psrr = (Hsig / Hpsr).normal();
    }

    PruneOptions o = opts_of(s);
    // PSR low-entropy form
    Pruned psr_p = prune_low_entropy(psr.num, psr.den, psr.params, o);
    Pruned psrr_p;
    if (psrr_ok)
        psrr_p = prune_low_entropy(psrr.numer(), psrr.denom(), sig.params, o);
    if (psrr_p.text.empty()) psrr_p.text = "0 (no supply coupling)";

    std::string rep = "PSR / PSRR analysis\n";
    rep += "input: " + s.input_ref + "   supply: " + vdd_ref + "\n";
    rep += "----------------------------------------\n";
    rep += "PSR  = H(VDD -> out)  = " + psr_p.text + "\n";
    rep += "PSRR = H(Vin -> out) / H(VDD -> out) = " + psrr_p.text + "\n";
    (void)s_sym;

    cr.text = "PSR = " + psr_p.text + " ;  PSRR = " + psrr_p.text;
    cr.latex = "PSRR = " + latex_rhs(psrr_p.latex);
    // The LaTeX report mirrors the text report section-for-section.
    {
        std::ostringstream lr;
        lr << "PSR / PSRR:\\quad \\mathrm{input}=" << s.input_ref
           << ",\\ \\mathrm{supply}=" << vdd_ref << "\n";
        lr << "PSR:\n";
        lr << "\\mathrm{PSR} = \\frac{\\mathrm{V_{out}}}{\\mathrm{V_{DD}}} = "
           << latex_rhs(psr_p.latex) << "\n";
        lr << "PSRR:\n";
        lr << "\\mathrm{PSRR} = \\frac{\\mathrm{V_{out}/V_{in}}}"
              "{\\mathrm{V_{out}/V_{DD}}} = "
           << latex_rhs(psrr_p.latex) << "\n";
        cr.latex_report = lr.str();
    }
    cr.summary = cr.text;
    cr.report = rep;
    cr.has_transfer = true;
    {
        AnalysisResult res;
        res.label = "PSR";
        res.input_desc = "VDD";
        res.output_desc = psr.out_desc;
        res.num_raw = psr.num;
        res.den_raw = psr.den;
        res.params = psr.params;
        res.opts = o;
        res.pruned = psr_p;
        res.report = format_report(res);
        cr.transfer = res;
    }
    return cr;
}

// ---------------------------------------------------------------------------
// Differential analysis: Adm, Acm and CMRR for a differential input port.
//
// The circuit is linear, so each modal response is a linear combination of
// ordinary single-source transfers. With a 1 V differential drive (v(i+) =
// +1/2, v(i-) = -1/2) the output is the differential-mode response; with a 1 V
// common-mode drive (v(i+) = v(i-) = 1) it is the common-mode response. We
// realise each drive by adding two ideal test voltage sources (one per input
// node) to a copy of the circuit and solving once, so the input port need not
// be an existing independent source. The output may itself be a port
// ("V(out+)-V(out-)" or a single "V(out)"), which the solver already handles.
// ---------------------------------------------------------------------------
CardResult analyze_differential(const Circuit& c, const AnalysisSpec& s) {
    CardResult cr;
    cr.kind = AnalysisKind::Differential;
    cr.title = "Differential (Adm / Acm / CMRR)";

    // The port fields accept either a bare node name ("n2") or a probe
    // ("V(n2)"); strip a surrounding V(..) so both forms work.
    auto as_node = [](std::string t) {
        if (t.size() > 3 && (t[0] == 'V' || t[0] == 'v') && t[1] == '(' &&
            t.back() == ')')
            t = t.substr(2, t.size() - 3);
        return t == "GND" ? std::string("0") : t;
    };
    std::string ip = as_node(s.input_port_p), in = as_node(s.input_port_n);
    if (ip.empty() || in.empty() || ip == "0" || in == "0") {
        cr.summary = "differential analysis needs two distinct input-port nodes";
        cr.report = cr.summary + "\n";
        return cr;
    }
    // Validate the port nodes exist, so a typo is a clear error rather than a
    // silent 0 V answer (a phantom node has no connection).
    {
        std::set<std::string> nodes;
        for (const auto& cc : c.comps)
            for (const auto& n : cc.nodes)
                nodes.insert(n == "GND" ? "0" : n);
        for (const std::string& nd : {ip, in})
            if (!nodes.count(nd)) {
                std::string avail;
                for (const auto& n : nodes)
                    if (n != "0") avail += (avail.empty() ? "" : ", ") + n;
                cr.summary = "differential input port node '" + nd +
                             "' does not exist. Existing nets: " +
                             (avail.empty() ? std::string("(none)") : avail);
                cr.report = cr.summary + "\n";
                return cr;
            }
    }

    // The differential response is H(i+ -> out) - H(i- -> out); the common-mode
    // response is H(i+ -> out) + H(i- -> out) (superposition: driving i+ with
    // +v and i- with -v, or both with +v). Each way of driving the port is one
    // ordinary solve.
    //
    // BOTH port nodes get a ground-referenced test source in every solve: the
    // selected input is driven at 1 V and the other source is zeroed by the
    // engine -- which grounds that port node (so it is never left floating when
    // the original bridging source, e.g. the V1 between the gates, is removed)
    // and gives the i- drive its own reference. Removing the incident
    // independent sources first avoids an ideal source in parallel with ours.
    auto port_solve = [&](const std::string& driven) -> RawTF {
        Circuit ct = c;
        std::vector<Component> keep;
        for (auto& cc : ct.comps) {
            if (is_independent_source(cc.kind)) {
                bool touches = false;
                for (const auto& n : cc.nodes)
                    if (n == ip || n == in) touches = true;
                if (touches) continue; // drop sources clamping a port node
                cc.value_text = "0";
            }
            keep.push_back(cc);
        }
        ct.comps.swap(keep);
        auto add_src = [&](const std::string& node, const char* ref) {
            Component vs;
            vs.kind = Kind::V;
            vs.ref = ref;
            vs.nodes = {node, "0"};
            vs.dc_text = "0";
            vs.ac_text = "1";
            vs.value_text = "1";
            ct.comps.push_back(vs);
        };
        add_src(ip, "__DP__");
        add_src(in, "__DN__");
        // Solve directly (no OCTC): the differential card only needs the
        // transfer polynomials, and OCTC runs a full solve per reactive element
        // -- doubling or tripling the cost for no benefit here.
        AnalysisRequest r;
        r.input_ref = driven;
        r.output = s.output;
        Solved sv = solve(ct, r);
        RawTF t;
        t.num = sv.num;
        t.den = sv.den;
        t.params = std::move(sv.params);
        t.out_desc = sv.output_desc;
        return t;
    };
    RawTF tp = port_solve("__DP__");
    RawTF tn = port_solve("__DN__");
    // The denominator is det(Y), which does NOT depend on which source drives,
    // so tp.den == tn.den and the modal responses are just numerator sums over
    // that one denominator (no degree-doubling product, no gcd):
    //     Adm = (num_p - num_n)/den,   Acm = (num_p + num_n)/(2*den).
    // The common-mode 1 -> 1/2 scale is a *denominator* factor 2 so a stray 2.0
    // never enters the symbolic numerator.
    auto combine = [&](bool common) -> RawTF {
        RawTF t;
        t.params = tp.params;
        t.out_desc = s.output;
        t.octc = tp.octc;
        t.num = common ? (tp.num + tn.num) : (tp.num - tn.num);
        t.den = common ? (2 * tp.den) : tp.den;
        return t;
    };

    RawTF dm = combine(false);
    RawTF cm = combine(true);
    // Adm = dm.num/det and Acm = cm.num/det share the denominator, so
    // CMRR = Adm/Acm = dm.num/cm.num: form the ratio directly and let the
    // pruner reduce it.
    ex CMRR_num = dm.num;
    ex CMRR_den = cm.num;
    bool cm_zero = cm.num.expand().is_zero();

    PruneOptions o = opts_of(s);
    Pruned adm_p = prune_low_entropy(dm.num, dm.den, dm.params, o);
    Pruned cm_p = prune_low_entropy(cm.num, cm.den, cm.params, o);
    Pruned cmrr_p;
    if (!cm_zero)
        cmrr_p = prune_low_entropy(CMRR_num, CMRR_den, dm.params, o);
    if (cmrr_p.text.empty())
        cmrr_p.text = cm_zero
                          ? std::string("infinite (no common-mode response)")
                          : pretty(CMRR_num / CMRR_den);

    cr.text = "Adm = " + adm_p.text + " ;  Acm = " + cm_p.text +
              " ;  CMRR = " + cmrr_p.text;
    cr.latex = "A_{dm} = " + latex_rhs(adm_p.latex);

    std::string rep = "Differential analysis\n";
    rep += "input port: " + ip + " - " + in + "   output: " + s.output + "\n";
    rep += "----------------------------------------\n";
    rep += "Adm(s)  = Vout / (v(" + ip + ") - v(" + in + ")) = " + adm_p.text + "\n";
    rep += "Acm(s)  = Vout / (common-mode drive)            = " + cm_p.text + "\n";
    rep += "CMRR(s) = Adm / Acm                             = " + cmrr_p.text + "\n";
    cr.report = rep;

    cr.latex_report = "Differential (input " + ip + " - " + in + "):\n";
    cr.latex_report += "A_{dm} = " + latex_rhs(adm_p.latex) + "\n";
    cr.latex_report += "A_{cm} = " + latex_rhs(cm_p.latex) + "\n";
    if (!cmrr_p.latex.empty())
        cr.latex_report += "\\mathrm{CMRR} = " + latex_rhs(cmrr_p.latex) + "\n";

    cr.has_transfer = true;
    {
        AnalysisResult res;
        res.label = "A_{dm}";
        res.input_desc = ip + " - " + in;
        res.output_desc = s.output;
        res.num_raw = dm.num;
        res.den_raw = dm.den;
        res.params = dm.params;
        res.opts = o;
        res.pruned = adm_p;
        res.report = format_report(res);
        cr.transfer = res;
    }
    return cr;
}

// ---------------------------------------------------------------------------
// Loop gain by the return-ratio (Rosenstark) method.
//
// We never break the loop. The reference amplifier is:
//   1. replaced by a nullor to get the asymptotic (ideal) closed-loop gain
//      H_inf -- the inputs become a virtual short;
//   2. replaced by an *independent test voltage* at its output, with its
//      control coupling opened, so the network returns the feedback factor
//      beta = v(ctrl+) - v(ctrl-) to its control port.
// The return ratio is T(s) = -A(s)*beta, where A(s) = A/(1 + s*A/(2*pi*GBW))
// is the amplifier's own single-pole gain. H_inf and T are both reported.
// ---------------------------------------------------------------------------
namespace {

struct AmpPorts {
    std::string cp, cn; // control port (cp - cn is the returned voltage)
    std::string op, on; // output port the test source drives
    bool ok = false;
    bool current_control = false; // CCCS/CCVS: control is a branch current
};

AmpPorts amp_ports(const Component& c) {
    const auto& nd = c.nodes;
    switch (c.kind) {
        case Kind::OPAMP:
            if (nd.size() >= 3) return {nd[0], nd[1], nd[2], "0", true};
            break;
        case Kind::FDOPAMP:
            if (nd.size() >= 4) return {nd[0], nd[1], nd[2], nd[3], true};
            break;
        case Kind::NULLOR:
            // Two-port: in+ , in- , out+ , out-
            if (nd.size() >= 4) return {nd[0], nd[1], nd[2], nd[3], true};
            break;
        case Kind::AMP:
            // single-ended input to ground, output to ground
            if (nd.size() >= 2) return {nd[0], "0", nd[1], "0", true};
            break;
        case Kind::OTA:
            // in+, in-, out
            if (nd.size() >= 3) return {nd[0], nd[1], nd[2], "0", true};
            break;
        case Kind::GM:
            // in, out (both single-ended to ground)
            if (nd.size() >= 2) return {nd[0], "0", nd[1], "0", true};
            break;
        case Kind::E:
        case Kind::G:
            // control port (ctrl+, ctrl-), output port (out+, out-).
            if (nd.size() >= 4) return {nd[2], nd[3], nd[0], nd[1], true};
            break;
        case Kind::CCCS:
        case Kind::CCVS:
            // current control is the current through the shorted input port.
            if (nd.size() >= 4) return {nd[0], nd[1], nd[2], nd[3], true, true};
            break;
        case Kind::NMOS:
        case Kind::PMOS:
            // Control port is gate-source; output port is drain-source. Using
            // drain-ground breaks as soon as the source is not at AC ground
            // (a cascode / source follower / output stage).
            if (nd.size() >= 3) return {nd[1], nd[2], nd[0], nd[2], true};
            break;
        case Kind::NPN:
        case Kind::PNP:
            if (nd.size() >= 3) return {nd[1], nd[2], nd[0], nd[2], true};
            break;
        default:
            break;
    }
    return {};
}

// ---------------------------------------------------------------------------
// Shared return-ratio pieces
// ---------------------------------------------------------------------------

std::string rr_num_text(double v) {
    std::ostringstream os;
    os << std::setprecision(17) << v;
    return os.str();
}

// Add a synthetic passive carrying a model-parameter symbol. The component
// reference is deliberately the parameter symbol itself (e.g. Cds_M1), so the
// synthesized H_inf/beta network keeps the same symbolic names as the device
// model and the low-entropy pruner can still recognise them.
void add_synth_passive(Circuit& circ, Kind kind, const std::string& ref,
                       const std::string& a, const std::string& b, double value) {
    if (ref.empty() || circ.find(ref)) return;
    Component x;
    x.kind = kind;
    x.ref = ref;
    x.nodes = {a, b};
    x.value_text = rr_num_text(value);
    circ.comps.push_back(x);
}

void add_device_parasitics(Circuit& circ, const Component& orig,
                           const AmpPorts& ports) {
    const auto& nd = orig.nodes;
    if (orig.kind == Kind::NMOS || orig.kind == Kind::PMOS) {
        if (nd.size() < 3) return;
        if (orig.param_enabled("ro"))
            add_synth_passive(circ, Kind::R, param_symbol(orig, "ro"),
                              nd[0], nd[2], orig.param_estimate("ro"));
        if (orig.param_enabled("Cgs"))
            add_synth_passive(circ, Kind::C, param_symbol(orig, "Cgs"),
                              nd[1], nd[2], orig.param_estimate("Cgs"));
        if (orig.param_enabled("Cgd"))
            add_synth_passive(circ, Kind::C, param_symbol(orig, "Cgd"),
                              nd[1], nd[0], orig.param_estimate("Cgd"));
        if (orig.param_enabled("Cds"))
            add_synth_passive(circ, Kind::C, param_symbol(orig, "Cds"),
                              nd[0], nd[2], orig.param_estimate("Cds"));
        if (orig.param_enabled("Cdb"))
            add_synth_passive(circ, Kind::C, param_symbol(orig, "Cdb"),
                              nd[0], nd[2], orig.param_estimate("Cdb"));
        return;
    }
    if (orig.kind == Kind::NPN || orig.kind == Kind::PNP) {
        if (nd.size() < 3) return;
        if (orig.param_enabled("rpi"))
            add_synth_passive(circ, Kind::R, param_symbol(orig, "rpi"),
                              nd[1], nd[2], orig.param_estimate("rpi"));
        if (orig.param_enabled("ro"))
            add_synth_passive(circ, Kind::R, param_symbol(orig, "ro"),
                              nd[0], nd[2], orig.param_estimate("ro"));
        if (orig.param_enabled("Cpi"))
            add_synth_passive(circ, Kind::C, param_symbol(orig, "Cpi"),
                              nd[1], nd[2], orig.param_estimate("Cpi"));
        if (orig.param_enabled("Cmu"))
            add_synth_passive(circ, Kind::C, param_symbol(orig, "Cmu"),
                              nd[1], nd[0], orig.param_estimate("Cmu"));
        return;
    }
    if (orig.kind == Kind::OTA || orig.kind == Kind::GM) {
        if (orig.param_enabled("Rout"))
            add_synth_passive(circ, Kind::R, param_symbol(orig, "Rout"),
                              ports.op, ports.on, orig.param_estimate("Rout"));
        return;
    }
    if (orig.kind == Kind::OPAMP || orig.kind == Kind::FDOPAMP ||
        orig.kind == Kind::AMP) {
        if (orig.param_enabled("Rin"))
            add_synth_passive(circ, Kind::R, param_symbol(orig, "Rin"),
                              ports.cp, ports.cn, orig.param_estimate("Rin"));
        if (orig.param_enabled("Cin"))
            add_synth_passive(circ, Kind::C, param_symbol(orig, "Cin"),
                              ports.cp, ports.cn, orig.param_estimate("Cin"));
        if (orig.kind == Kind::FDOPAMP &&
            orig.param_enabled("Rout"))
            add_synth_passive(circ, Kind::R, param_symbol(orig, "Rout"),
                              ports.op, ports.on, orig.param_estimate("Rout"));
        if (orig.param_enabled("Cout"))
            add_synth_passive(circ, Kind::C, param_symbol(orig, "Cout"),
                              ports.op, ports.on, orig.param_estimate("Cout"));
    }
}

Circuit nullor_reference_circuit(const Circuit& c, const std::string& ref,
                                 const AmpPorts& ports,
                                 bool include_parasitics = true) {
    Circuit ci = c;
    for (auto& cc : ci.comps) {
        if (cc.ref != ref) continue;
        Component orig = cc;
        cc.kind = Kind::NULLOR;
        cc.nodes = {ports.cp, ports.cn, ports.op, ports.on};
        cc.value_text.clear();
        cc.param_on.clear();
        cc.param_text.clear();
        cc.param_override.clear();
        cc.links.clear();
        // A reference that is itself a copy must not remain a copy of a
        // MOSFET/BJT after becoming a nullor.
        cc.mirror_ref.clear();
        if (include_parasitics) add_device_parasitics(ci, orig, ports);
        break;
    }
    // The reference has become a nullor; copies still pointing at it would make
    // resolve_mirrors reject a kind mismatch. They keep their materialised
    // parameters and become independent devices in this reduced network.
    for (auto& cc : ci.comps)
        if (cc.mirror_ref == ref) cc.mirror_ref.clear();
    return ci;
}

Circuit beta_reference_circuit(const Circuit& c, const std::string& ref,
                               const AmpPorts& ports,
                               bool include_parasitics = true) {
    Circuit ct = c;
    Component orig;
    bool found = false;
    for (size_t i = 0; i < ct.comps.size(); ++i) {
        if (ct.comps[i].ref == ref) {
            orig = ct.comps[i];
            ct.comps.erase(ct.comps.begin() + i);
            found = true;
            break;
        }
    }
    if (found && include_parasitics) add_device_parasitics(ct, orig, ports);
    for (auto& cc : ct.comps)
        if (cc.mirror_ref == ref) cc.mirror_ref.clear();
    return ct;
}

Circuit zero_gain_reference_circuit(const Circuit& c, const std::string& ref,
                                    const AmpPorts& ports) {
    Circuit c0 = c;
    for (auto& cc : c0.comps) {
        if (cc.ref != ref) continue;
        Component orig = cc;
        if (orig.kind == Kind::NMOS || orig.kind == Kind::PMOS ||
            orig.kind == Kind::NPN || orig.kind == Kind::PNP) {
            cc.param_text["gm"] = "0";
            cc.param_on["gm"] = true;
            cc.param_override.erase("gm");
        } else if (orig.kind == Kind::OPAMP || orig.kind == Kind::AMP ||
                   orig.kind == Kind::FDOPAMP) {
            Component off;
            off.kind = Kind::V;
            off.ref = ref;
            off.nodes = {ports.op, ports.on};
            off.value_text = "0";
            cc = off;
            add_device_parasitics(c0, orig, ports);
        } else {
            // OTA/GM, VCVS/VCCS, CCCS/CCVS: an explicit zero value is the
            // zero-gain limit. A zero-voltage output (E, CCVS) is a 0 V
            // branch; a zero current gain is an open output.
            cc.value_text = "0";
        }
        break;
    }
    return c0;
}

ex controlled_gain(ParamTable& pt, const Component& c) {
    switch (c.kind) {
        case Kind::NMOS:
        case Kind::PMOS:
        case Kind::NPN:
        case Kind::PNP:
            return reg_param(pt, c, "gm");
        case Kind::OTA:
        case Kind::GM: {
            std::string name = param_symbol(c, "gm");
            ex sym = pt.get(name);
            double g = c.estimate();
            double mag = std::fabs(g);
            if (!std::isfinite(mag)) mag = 1e-3;
            pt.set(name, mag, UnitClass::Siemens);
            return (g < 0.0) ? -sym : sym;
        }
        case Kind::OPAMP:
        case Kind::FDOPAMP:
        case Kind::AMP: {
            std::string name = "A_" + c.ref;
            ex sym = pt.get(name);
            double a = c.estimate();
            double mag = std::fabs(a);
            if (!std::isfinite(mag)) mag = 1.0;
            pt.set(name, mag, UnitClass::Plain);
            return (a < 0.0) ? -sym : sym;
        }
        case Kind::E:
        case Kind::G:
        case Kind::CCCS:
        case Kind::CCVS:
            // These blocks use a numeric gain in the MNA stamp.
            return ex(c.estimate());
        default:
            break;
    }
    return ex(0);
}

bool has_reference_pole(Kind k) {
    return k == Kind::OPAMP || k == Kind::FDOPAMP || k == Kind::AMP ||
           k == Kind::OTA || k == Kind::GM;
}

bool reference_uses_current_test(const Component& c) {
    // A transconductance/current-output reference is tested with a current
    // source (its natural output quantity); a voltage-output reference with a
    // voltage source. With the matching test the return ratio is dimensionless
    // and T = -A*beta is exact for every controlled-source type.
    switch (c.kind) {
        case Kind::NMOS:
        case Kind::PMOS:
        case Kind::NPN:
        case Kind::PNP:
        case Kind::OTA:
        case Kind::GM:
        case Kind::G:
        case Kind::CCCS:
            return true;
        default:
            return false;
    }
}

std::string reference_gain_symbol(const Component& c) {
    switch (c.kind) {
        case Kind::NMOS:
        case Kind::PMOS:
        case Kind::NPN:
        case Kind::PNP:
        case Kind::OTA:
        case Kind::GM:
            return param_symbol(c, "gm");
        case Kind::OPAMP:
        case Kind::FDOPAMP:
        case Kind::AMP:
            return "A_" + c.ref;
        default:
            return "";
    }
}

ex pruned_to_expr(const Pruned& p) {
    ex e = p.gain;
    for (const auto& f : p.num_factors) e *= f.expr;
    ex d = ex(1);
    for (const auto& f : p.den_factors) d *= f.expr;
    return (e / d).normal();
}

// ---------------------------------------------------------------------------
// Signal-flow graph used to find the loop-defining controlled source.
// ---------------------------------------------------------------------------
struct FlowGraph {
    std::map<std::string, std::vector<std::string>> adj;
    void add(const std::string& a, const std::string& b) {
        if (!a.empty() && !b.empty() && a != b) adj[a].push_back(b);
    }
    void bi(const std::string& a, const std::string& b) {
        add(a, b);
        add(b, a);
    }
};

void add_component_flow(FlowGraph& g, const Component& c) {
    const auto& nd = c.nodes;
    auto node = [&](size_t i) -> std::string {
        return i < nd.size() ? nd[i] : std::string("0");
    };
    switch (c.kind) {
        case Kind::R:
        case Kind::C:
        case Kind::L:
        case Kind::D:
            if (nd.size() >= 2) g.bi(nd[0], nd[1]);
            break;
        case Kind::V:
            if (nd.size() >= 2) g.bi(nd[0], nd[1]);
            break;
        case Kind::VDD:
            if (!nd.empty()) g.bi(nd[0], "0");
            break;
        case Kind::T:
            if (nd.size() >= 4) {
                g.bi(nd[0], nd[1]);
                g.bi(nd[2], nd[3]);
                g.bi(nd[0], nd[2]);
                g.bi(nd[1], nd[3]);
            }
            break;
        case Kind::NMOS:
        case Kind::PMOS:
            if (nd.size() >= 3) {
                // gate and source both drive the drain current; the output
                // conductance couples drain and source in both directions.
                g.add(nd[1], nd[0]);
                g.add(nd[1], nd[2]);
                g.add(nd[2], nd[0]);
                g.bi(nd[0], nd[2]);
            }
            break;
        case Kind::NPN:
        case Kind::PNP:
            if (nd.size() >= 3) {
                g.add(nd[1], nd[0]);
                g.add(nd[1], nd[2]);
                g.add(nd[2], nd[0]);
                g.bi(nd[0], nd[2]);
            }
            break;
        case Kind::IS:
        case Kind::SBLK:
            if (nd.size() >= 2) g.add(nd[0], nd[1]);
            break;
        default: {
            AmpPorts p = amp_ports(c);
            if (p.ok) {
                // A controlled source transfers its control port to its
                // output port; this is the directed SFG edge whose return
                // path identifies a feedback loop.
                g.add(p.cp, p.op);
                g.add(p.cp, p.on);
                g.add(p.cn, p.op);
                g.add(p.cn, p.on);
                if (p.current_control) g.bi(p.cp, p.cn);
            }
            break;
        }
    }
    (void)node;
}

int flow_distance(const FlowGraph& g, const std::vector<std::string>& starts,
                  const std::vector<std::string>& goals, bool allow_equal) {
    if (goals.empty()) return -1;
    std::set<std::string> goal_set(goals.begin(), goals.end());
    std::queue<std::pair<std::string, int>> q;
    std::set<std::string> seen;
    for (const auto& s : starts) {
        if (s.empty() || seen.count(s)) continue;
        if (allow_equal && goal_set.count(s)) return 0;
        seen.insert(s);
        q.push({s, 0});
    }
    while (!q.empty()) {
        auto [u, d] = q.front();
        q.pop();
        auto it = g.adj.find(u);
        if (it == g.adj.end()) continue;
        for (const auto& v : it->second) {
            if (v.empty() || seen.count(v)) continue;
            if (goal_set.count(v)) return d + 1;
            seen.insert(v);
            q.push({v, d + 1});
        }
    }
    return -1;
}

std::string rr_voltage_node(const std::string& output) {
    if (output.size() < 4 || output.rfind("V(", 0) != 0 ||
        output.back() != ')')
        return "";
    std::string inner = output.substr(2, output.size() - 3);
    for (char ch : inner)
        if (ch == '-' || ch == ')') return "";
    return inner;
}

int rr_output_var(const MnaSystem& sys, const std::string& output) {
    std::string want;
    if (!rr_voltage_node(output).empty())
        want = "v(" + rr_voltage_node(output) + ")";
    else if (output.size() > 3 && output.rfind("I(", 0) == 0 &&
             output.back() == ')')
        want = "i(" + output.substr(2, output.size() - 3) + ")";
    else
        return -1;
    for (size_t i = 0; i < sys.var_names.size(); ++i)
        if (sys.var_names[i] == want) return int(i);
    return -1;
}

bool solve_complex(std::vector<std::vector<std::complex<double>>> A,
                   std::vector<std::complex<double>> b,
                   std::vector<std::complex<double>>& x) {
    const int n = int(A.size());
    if (n == 0 || int(b.size()) != n) return false;
    for (int col = 0; col < n; ++col) {
        int piv = col;
        double best = std::abs(A[col][col]);
        for (int r = col + 1; r < n; ++r) {
            double v = std::abs(A[r][col]);
            if (v > best) {
                best = v;
                piv = r;
            }
        }
        if (!(best > 1e-300)) return false;
        if (piv != col) {
            std::swap(A[piv], A[col]);
            std::swap(b[piv], b[col]);
        }
        const std::complex<double> d = A[col][col];
        for (int r = col + 1; r < n; ++r) {
            const std::complex<double> f = A[r][col] / d;
            if (std::abs(f) == 0.0) continue;
            for (int k = col; k < n; ++k) A[r][k] -= f * A[col][k];
            b[r] -= f * b[col];
        }
    }
    x.assign(n, {});
    for (int r = n - 1; r >= 0; --r) {
        std::complex<double> sum = b[r];
        for (int k = r + 1; k < n; ++k) sum -= A[r][k] * x[k];
        x[r] = sum / A[r][r];
    }
    return true;
}

bool numeric_mna_value(const MnaSystem& sys, int out_idx, double omega,
                       std::complex<double>& value) {
    if (out_idx < 0 || out_idx >= sys.n) return false;
    std::vector<std::vector<std::complex<double>>> A(
        sys.n, std::vector<std::complex<double>>(sys.n));
    std::vector<std::complex<double>> b(sys.n);
    for (int i = 0; i < sys.n; ++i) {
        b[i] = eval_complex(sys.b(i, 0), sys.params, omega);
        for (int j = 0; j < sys.n; ++j)
            A[i][j] = eval_complex(sys.Y(i, j), sys.params, omega);
    }
    std::vector<std::complex<double>> x;
    if (!solve_complex(std::move(A), std::move(b), x)) return false;
    value = x[out_idx];
    return true;
}

void merge_params(ParamTable& dst, const ParamTable& src) {
    // The asymptotic circuit is the master parameter set: beta/H0 are reduced
    // versions of the same circuit and must not overwrite an estimate (most
    // importantly the reference gm, which is zero in the H0 network).
    for (const auto& kv : src.est)
        if (!dst.est.count(kv.first)) dst.est[kv.first] = kv.second;
    for (const auto& kv : src.cls)
        if (!dst.cls.count(kv.first)) dst.cls[kv.first] = kv.second;
    for (const auto& kv : src.syms) dst.syms[kv.first] = kv.second;
    for (const auto& kv : src.defs) dst.defs[kv.first] = kv.second;
    dst.off.insert(src.off.begin(), src.off.end());
}

bool verify_return_ratio(const Circuit& c, const AnalysisSpec& s,
                         const std::string& out_node, const ex& Hraw,
                         const ParamTable& pt, double tolerance = 2e-5) {
    try {
        std::set<std::string> used;
        used.insert(out_node);
        MnaSystem sys = build_mna(c, s.input_ref, used);
        int idx = rr_output_var(sys, s.output);
        if (idx < 0) return false;
        const double freqs[] = {10.0, 1e3, 1e5, 1e7, 1e9};
        int tested = 0, failed = 0;
        for (double f : freqs) {
            double w = 2.0 * M_PI * f;
            std::complex<double> direct;
            if (!numeric_mna_value(sys, idx, w, direct)) return false;
            std::complex<double> rr = eval_complex(Hraw, pt, w);
            if (!std::isfinite(rr.real()) || !std::isfinite(rr.imag()))
                continue;
            ++tested;
            double scale = std::max(std::abs(direct), 1e-30);
            if (std::abs(rr - direct) / scale > tolerance) ++failed;
        }
        return tested >= 2 && failed == 0;
    } catch (const std::exception&) {
        return false;
    }
}

bool try_one_return_ratio(const Circuit& c, const AnalysisSpec& s,
                          const Component& ref, const AmpPorts& ports,
                          RawTF& out) {
    struct Guard {
        bool old;
        Guard() : old(g_return_ratio_active) { g_return_ratio_active = true; }
        ~Guard() { g_return_ratio_active = old; }
    } guard;

    ParamTable pt;
    ex Hinf_raw = 0, Hinf_le = 0;
    ex H0_raw = 0, H0_le = 0;
    Pruned Hinf_ready;
    std::string out_desc;
    std::vector<TimeConstant> octc;

    try {
        Circuit ci = nullor_reference_circuit(c, ref.ref, ports);
        CardResult ideal = analyze_tf(ci, s);
        if (!ideal.has_transfer) return false;
        Hinf_raw = (ideal.transfer.num_raw / ideal.transfer.den_raw).normal();
        Hinf_le = pruned_to_expr(ideal.transfer.pruned);
        Hinf_ready = ideal.transfer.pruned;
        pt = ideal.transfer.params;
        out_desc = ideal.transfer.output_desc;
    } catch (const std::exception&) {
        return false;
    }

    // High-loop-gain shortcut. The asymptotic gain is already a factored
    // expression; when it matches the network to the pruning tolerance, use it
    // as the presentation and take the exact raw MNA ratio only for plotting
    // and metrics. This avoids the large T/H0 symbolic expansion entirely.
    {
        std::string out_node = rr_voltage_node(s.output);
        if (!out_node.empty() &&
            verify_return_ratio(c, s, out_node, Hinf_le, pt, 0.20)) {
            try {
                AnalysisRequest r;
                r.input_ref = s.input_ref;
                r.output = s.output;
                Solved sv = solve(c, r);
                merge_params(pt, sv.params);
                out.num = sv.num;
                out.den = sv.den;
                out.params = pt;
                out.out_desc = sv.output_desc;
                std::set<std::string> used;
                used.insert(out_node);
                out.octc = open_circuit_time_constants(c, s.input_ref, pt, used);
                out.ready_pruned = true;
                out.ready = Hinf_ready;
                return true;
            } catch (const std::exception&) {
                // fall through to the full return-ratio assembly
            }
        }
    }

    // Feedback factor with the reference gain opened. The device's passive
    // parasitics stay in place; only the controlled transfer is removed.
    ex beta = 0;
    try {
        Circuit ct = beta_reference_circuit(c, ref.ref, ports);
        Component vt;
        vt.kind = reference_uses_current_test(ref) ? Kind::I : Kind::V;
        vt.ref = "__TEST__";
        // A voltage test is applied + at op, - at on. A current test is the
        // dual: inject 1 A into the output terminal on and remove it from op.
        // (For a MOSFET's D-S port this makes the returned T positive for
        // negative feedback, i.e. T = -A*beta with beta measured at the
        // control port.)
        vt.nodes = reference_uses_current_test(ref)
                       ? std::vector<std::string>{ports.op, ports.on}
                       : std::vector<std::string>{ports.op, ports.on};
        vt.value_text = "1";
        ct.comps.push_back(vt);
        if (ports.current_control) {
            Component sense;
            sense.kind = Kind::V;
            sense.ref = "__SENSE__";
            sense.nodes = {ports.cp, ports.cn};
            sense.value_text = "0";
            ct.comps.push_back(sense);
            RawTF t = raw_tf(ct, "__TEST__", "I(__SENSE__)", {}, false);
            beta = (t.num / t.den).normal();
            merge_params(pt, t.params);
        } else {
            std::string probe;
            auto add_node = [&](const std::string& node, bool neg) {
                if (node == "0" || node == "GND") return;
                if (!probe.empty() || neg) probe += neg ? "-" : "+";
                probe += "V(" + node + ")";
            };
            add_node(ports.cp, false);
            add_node(ports.cn, true);
            if (!probe.empty()) {
                RawTF t = raw_tf(ct, "__TEST__", probe, {}, false);
                beta = (t.num / t.den).normal();
                merge_params(pt, t.params);
            }
        }
    } catch (const std::exception&) {
        return false;
    }

    ex A = controlled_gain(pt, ref);
    ex s_sym = pt.get("s");
    ex T = (-A * beta).normal();
    if (T.is_zero() || beta.is_zero()) return false;
    if (has_reference_pole(ref.kind) && ref.param_enabled("GBW") &&
        ref.param_estimate("GBW") > 0.0) {
        std::string gname = "GBW_" + ref.ref;
        ex gsym = pt.get(gname);
        pt.set(gname, 2.0 * M_PI * ref.param_estimate("GBW"),
               UnitClass::Plain);
        T = (T / (1 + s_sym * A / gsym)).normal();
    }

    // Forward gain with the reference controlled source switched off. This
    // only needs the raw MNA ratio (H0 is a small correction term), so avoid
    // the full report/prune path.
    try {
        Circuit c0 = zero_gain_reference_circuit(c, ref.ref, ports);
        AnalysisRequest r;
        r.input_ref = s.input_ref;
        r.output = s.output;
        Solved sv = solve(c0, r);
        H0_raw = (sv.num / sv.den).normal();
        H0_le = H0_raw;
        merge_params(pt, sv.params);
        // The zero-gain circuit still carries the gain *symbol* in its matrix
        // (with a zero estimate). Remove it from H0 explicitly so the
        // assembled formula is evaluated with the reference off.
        std::string gs = reference_gain_symbol(ref);
        if (!gs.empty()) {
            GiNaC::exmap zm;
            zm[pt.get(gs)] = ex(0);
            H0_raw = H0_raw.subs(zm).normal();
            H0_le = H0_le.subs(zm).normal();
        }
    } catch (const std::exception&) {
        H0_raw = 0;
        H0_le = 0;
    }

    ex denom = (1 + T).normal();
    if (denom.is_zero()) return false;
    ex Hraw = ((Hinf_raw * T + H0_raw) / denom).normal();
    ex Hle = ((Hinf_le * T + H0_le) / denom).normal();
    if (Hraw.is_zero()) return false;

    if (!verify_return_ratio(c, s, rr_voltage_node(s.output), Hraw, pt))
        return false;

    try {
        std::set<std::string> used;
        std::string node = rr_voltage_node(s.output);
        if (!node.empty()) used.insert(node);
        octc = open_circuit_time_constants(c, s.input_ref, pt, used);
    } catch (const std::exception&) {
        octc.clear();
    }
    // For a high-gain loop Hcl -> H_inf. Keep the exact raw H for plots and
    // metrics, but publish the already-factored asymptotic gain as the
    // low-entropy presentation. The loose numeric check guards this shortcut.
    bool high_gain = false;
    try {
        double w0 = 2.0 * M_PI * (s.f0_hz > 0.0 ? s.f0_hz : 1.0);
        double tm = std::abs(eval_complex(T, pt, w0));
        double hm = std::abs(eval_complex(Hinf_raw, pt, w0));
        double h0m = std::abs(eval_complex(H0_raw, pt, w0));
        high_gain = std::isfinite(tm) && tm > 20.0 &&
                    std::isfinite(hm) &&
                    (!std::isfinite(h0m) || h0m < 0.25 * (hm + 1e-30));
    } catch (const std::exception&) {
    }
    if (high_gain &&
        verify_return_ratio(c, s, rr_voltage_node(s.output), Hinf_le, pt,
                            0.20)) {
        out.num = Hraw.numer();
        out.den = Hraw.denom();
        out.params = pt;
        out.out_desc = out_desc.empty() ? s.output : out_desc;
        out.octc = octc;
        out.ready_pruned = true;
        out.ready = Hinf_ready;
        return true;
    }

    // Do not hand an enormous expanded expression to the generic pruner: in a
    // moderate-loop case that can take minutes. The direct solver remains the
    // safe fallback for such topologies.
    if (Hle.numer().nops() + Hle.denom().nops() > 300) return false;

    out.num = Hle.numer();
    out.den = Hle.denom();
    out.params = pt;
    out.out_desc = out_desc.empty() ? s.output : out_desc;
    out.octc = octc;
    return true;
}

bool try_return_ratio_tf(const Circuit& c, const AnalysisSpec& s, RawTF& out) {
    if (g_return_ratio_active) return false;
    std::string out_node = rr_voltage_node(s.output);
    if (out_node.empty()) return false;
    const Component* input = c.find(s.input_ref);
    if (!input || input->nodes.empty()) return false;

    FlowGraph g;
    for (const auto& cc : c.comps) add_component_flow(g, cc);
    std::vector<std::string> input_nodes = input->nodes;

    struct Cand {
        const Component* comp;
        AmpPorts ports;
        int score;
    };
    std::vector<Cand> cands;
    for (const auto& cc : c.comps) {
        if (cc.kind == Kind::NULLOR) continue; // no finite H0 reference
        AmpPorts p = amp_ports(cc);
        if (!p.ok) continue;
        // The source must have a forward path from the input to its control
        // port and a return path from its output to that control port.
        if (flow_distance(g, input_nodes, {p.cp, p.cn}, false) < 0) continue;
        if (flow_distance(g, {p.op, p.on}, {p.cp, p.cn}, true) < 0) continue;
        int od = flow_distance(g, {p.op, p.on}, {out_node}, true);
        if (od < 0) continue;
        int score = od * 1000;
        if (p.current_control) score += 20;
        // Prefer a reference whose output sits directly at the requested
        // output (the usual single-loop case).
        cands.push_back({&cc, p, score});
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
        return a.score < b.score;
    });
    // Trying a handful of candidates keeps pathological circuits bounded.
    const size_t limit = std::min<size_t>(cands.size(), 4);
    for (size_t i = 0; i < limit; ++i) {
        if (try_one_return_ratio(c, s, *cands[i].comp, cands[i].ports, out))
            return true;
    }
    return false;
}

} // namespace

CardResult analyze_loop_gain(const Circuit& c, const AnalysisSpec& s) {
    CardResult cr;
    cr.kind = AnalysisKind::LoopGain;
    cr.title = "Return ratio (loop gain)";

    const Component* ref = c.find(s.probe_ref);
    if (!ref) {
        cr.summary = "reference element '" + s.probe_ref + "' not found";
        cr.report = cr.summary + "\n";
        return cr;
    }
    AmpPorts ports = amp_ports(*ref);
    if (!ports.ok) {
        cr.summary = "reference element '" + s.probe_ref +
                     "' is not an amplifier-like device";
        cr.report = cr.summary + "\n";
        return cr;
    }

    // Only the ideal/asymptotic solve and beta are needed for T. The actual
    // closed-loop H(s) is H_inf*T/(1+T) + H0/(1+T); solving it separately was
    // a full extra symbolic MNA solve and dominated the loop-gain runtime.
    ParamTable pt;
    ex s_ex;

    // ---- 1. H_inf: replace the amplifier with a nullor (virtual short) ----
    // The nullor is a two-port: the INPUT port (in+, in-) is the nullator and
    // the OUTPUT port (out+, out-) is the norator. Map each amplifier kind's
    // ports onto it.
    // Loop gain keeps the previous ideal reference (no synthesized ro/caps):
    // T is the controlled-source return ratio, and retaining every parasitic
    // here makes the symbolic T expansion needlessly large. The H(s) path
    // below keeps them, because there they are needed for numerical identity.
    Circuit ci = nullor_reference_circuit(c, s.probe_ref, ports, false);
    CardResult ideal;
    try {
        ideal = analyze_tf(ci, s);
    } catch (const std::exception& e) {
        cr.summary = std::string("nullor substitution failed: ") + e.what();
        cr.report = cr.summary + "\n";
        return cr;
    }
    pt = ideal.transfer.params;
    s_ex = pt.get("s");

    // ---- 2. beta: drive the output with a test voltage, control opened ----
    Circuit ct = beta_reference_circuit(c, s.probe_ref, ports, false);
    {
        Component vt;
        vt.kind = reference_uses_current_test(*ref) ? Kind::I : Kind::V;
        vt.ref = "__TEST__";
        vt.nodes = std::vector<std::string>{ports.op, ports.on};
        vt.value_text = "1";
        ct.comps.push_back(vt);
        if (ports.current_control) {
            Component sense;
            sense.kind = Kind::V;
            sense.ref = "__SENSE__";
            sense.nodes = {ports.cp, ports.cn};
            sense.value_text = "0";
            ct.comps.push_back(sense);
        }
    }
    ex beta;
    try {
        std::string out;
        if (ports.current_control) {
            // A CCCS/CCVS control quantity is the current through its shorted
            // input port, so beta is the sense-branch current per test volt.
            out = "I(__SENSE__)";
        } else {
            // One differential probe instead of two separate MNA solves.
            auto add_node = [&](const std::string& node, bool negative) {
                if (node == "0" || node == "GND") return;
                if (!out.empty() || negative) out += negative ? "-" : "+";
                out += "V(" + node + ")";
            };
            add_node(ports.cp, false);
            add_node(ports.cn, true);
        }
        if (out.empty()) {
            beta = 0;
        } else {
            // beta is a single low-order transfer; the direct MNA solve is
            // cheaper than setting up EET probes for it.
            RawTF t = raw_tf(ct, "__TEST__", out, {}, false);
            beta = (t.num / t.den).normal();
        }
    } catch (const std::exception& e) {
        cr.summary = std::string("return-ratio test failed: ") + e.what();
        cr.report = cr.summary + "\n";
        return cr;
    }
    // A nullor has infinite gain: the return ratio is infinite and the
    // closed-loop gain is exactly H_inf. Reporting T = -A*beta with A = 1 (the
    // generic fallback estimate) would claim a finite loop gain and a finite
    // closed-loop gain, which is wrong. There is no finite unity-gain
    // crossover to plot either, so this card has no transfer curve.
    if (ref->kind == Kind::NULLOR) {
        cr.text = "T(s) = infinite (ideal nullor)";
        cr.latex = "T(s) = \\infty";
        cr.summary = "H_inf = " + ideal.text +
                     " ;  T = infinite (ideal nullor)";
        std::string rep;
        rep += "Return-ratio / loop-gain analysis\n";
        rep += "reference amplifier: " + s.probe_ref + "\n";
        rep += "----------------------------------------\n\n";
        rep += "Asymptotic (ideal) gain:\n  H_inf(s) = " + ideal.text + "\n";
        rep += "\nForward gain with the amplifier off:\n  H_0(s) = 0\n";
        rep += "\nReturn ratio:\n"
               "  T(s) = infinite (ideal nullor: A = infinity)\n";
        rep += "\nFeedback factor:\n  beta(s) = " + pretty(beta) + "\n";
        rep += "\nClosed-loop gain:\n"
               "  H(s) = H_inf (ideal nullor: T -> infinity)\n";
        rep += "\n";
        cr.report = rep;
        std::string lrep;
        lrep += "Return-ratio / loop-gain analysis\n";
        lrep += "reference amplifier: " + s.probe_ref + "\n";
        lrep += "----------------------------------------\n\n";
        lrep += "Asymptotic (ideal) gain:\n";
        lrep += "H_{\\infty}(s) = " + latex_rhs(ideal.latex) + "\n";
        lrep += "\nForward gain with the amplifier off:\n";
        lrep += "H_0(s) = 0\n";
        lrep += "\nReturn ratio:\n";
        lrep += "T(s) = \\infty \\quad (\\mathrm{ideal\\ nullor})\n";
        lrep += "\nFeedback factor:\n";
        lrep += "\\beta(s) = " + to_latex(beta) + "\n";
        lrep += "\nClosed-loop gain:\n";
        lrep += "H(s) = H_{\\infty} \\quad (T \\to \\infty)\n";
        lrep += "\n";
        cr.latex_report = lrep;
        cr.has_transfer = false;
        return cr;
    }

    // A(s) = A / (1 + s*A/GBW)  (GBW_<ref> is the unity-gain angular frequency
    // in rad/s -- the user's Hz value converted once here); T(s) = -A(s)*beta.
    // The reference gain is the actual controlled-source gain: gm for a
    // transistor/OTA/Gm, A for a voltage amplifier, and the numeric gain for a
    // VCVS/VCCS/CCCS/CCVS.
    ex A_sym = controlled_gain(pt, *ref);
    ex gbw_sym = pt.get("GBW_" + s.probe_ref);
    double gbw_num = 2.0 * M_PI * ref->param_estimate("GBW"); // rad/s
    bool gbw_on = has_reference_pole(ref->kind) &&
                  ref->param_enabled("GBW") && gbw_num > 0.0;
    if (gbw_on)
        pt.set("GBW_" + s.probe_ref, gbw_num, UnitClass::Plain);
    ex T = (-A_sym * beta).normal();
    if (gbw_on)
        T = (T / (1 + s_ex * A_sym / gbw_sym)).normal();

    PruneOptions o = opts_of(s);
    Pruned Tp = prune_low_entropy(T.numer(), T.denom(), pt, o);
    if (Tp.text.empty()) Tp.text = "0";

    AnalysisResult res;
    res.input_desc = "return ratio";
    res.output_desc = "T(" + s.probe_ref + ")";
    res.num_raw = T.numer();
    res.den_raw = T.denom();
    res.params = pt;
    res.opts = o;
    res.sweep = s.sweep;
    res.pruned = Tp;
    res.report = format_report(res);

    // Phase margin: at the unity-gain crossover, PM = 180 + angle(T). Also
    // capture the pole corner frequencies so the symbolic PM below can be
    // written in terms of the dominant pole.
    double pm = -1.0, ugf = -1.0;
    {
        double f0 = s.sweep.f_start_hz > 0 ? s.sweep.f_start_hz : 1.0;
        double f1 = s.sweep.f_stop_hz > f0 ? s.sweep.f_stop_hz : f0 * 1e6;
        const int N = 800;
        // Normalize the rational ONCE (see compute_metrics) -- 800 gcds would
        // otherwise dominate the loop-gain analysis.
        ex Hloop = (res.num_raw / res.den_raw);
        if (!GiNaC::is_a<GiNaC::numeric>(Hloop)) Hloop = Hloop.normal();
        double prev_f = f0, prev_m = eval_mag_db(Hloop, res.params, 2.0 * M_PI * f0);
        for (int i = 1; i <= N; ++i) {
            double t = double(i) / N;
            double f = f0 * std::pow(f1 / f0, t);
            double m = eval_mag_db(Hloop, res.params, 2.0 * M_PI * f);
            if (std::isfinite(prev_m) && std::isfinite(m) && prev_m > 0.0 &&
                m <= 0.0) {
                double frac = (prev_m - 0.0) / (prev_m - m);
                ugf = prev_f + frac * (f - prev_f);
                double ph = phase_deg_at(res, 2.0 * M_PI * ugf);
                pm = 180.0 + ph;
                break;
            }
            prev_f = f;
            prev_m = m;
        }
    }

    // A closed-form phase margin. With the loop factored as
    //   T(s) = K / prod(1 + j*w/w_pi) * prod(1 + j*w/w_zj),
    // the phase at the unity-gain frequency w_ug is a sum of arctangents, so
    //   PM = 180 - sum_i atan(w_ug/w_pi) + sum_j atan(w_ug/w_zj).
    // w_ug itself has no closed form for a general loop, but for the common
    // single-pole case w_ug = K*w_p0 and PM = 180 - atan(K), which we write
    // symbolically; a two-pole loop gives the classic
    //   PM = 90 - atan(w_ug/w_p1).
    // PM = 180 - sum atan(w_ug/w_pi) + sum atan(w_ug/w_zj). The unity-gain
    // frequency w_ug is a plain number (it was found numerically), so the
    // symbolic PM is the exact arctangent sum with w_ug pinned to that number
    // and the pole corner frequencies kept symbolic: this is low entropy and
    // matches the numeric PM.
    // PM is written in the low-entropy form
    //   PM = 180 - sum_i atan(w_ug / w_pi)  [+ sum_j atan(w_ug/w_zj)]
    // with w_ug pinned to its numeric value and each w_pi kept as the symbol
    // omega_pi (their symbolic values are listed under Pole Corner
    // Frequencies), so the reader sees the structure, not a substituted mess.
    std::string pm_ENVLP_txt, pm_ENVLP_tex;
    if (pm >= 0.0 && ugf > 0.0) {
        char wugb[64];
        std::snprintf(wugb, sizeof(wugb), "%s", eng::format_rads(2 * M_PI * ugf).c_str());
        std::string t = "180";
        std::string l = "180";
        for (size_t i = 0; i < Tp.poles.size(); ++i) {
            t += " - atan(" + std::string("omega_ug") + "/omega_p" +
                 std::to_string(i) + ")";
            l += " - \\atan\\left(\\frac{\\omega_{ug}}{\\omega_{p" +
                 std::to_string(i) + "}}\\right)";
        }
        for (size_t i = 0; i < Tp.zeros.size(); ++i) {
            t += " + atan(omega_ug/omega_z" + std::to_string(i) + ")";
            l += " + \\atan\\left(\\frac{\\omega_{ug}}{\\omega_{z" +
                 std::to_string(i) + "}}\\right)";
        }
        pm_ENVLP_txt = "PM = " + t + "   (omega_ug = " + wugb +
                     ", one atan per pole/zero)";
        pm_ENVLP_tex = "\\mathrm{PM} = " + l +
                     ",\\quad \\omega_{ug} = \\mathrm{" + wugb + "}";
    }

    // H_0: the forward gain with the reference amplifier's gain set to zero.
    // A zero-gain voltage amplifier forces v(out) = 0, i.e. its output is a
    // virtual ground: model it as a 0 V source there (for a fully differential
    // amp, a 0 V source between out+ and out-). The surrounding network then
    // provides whatever feedforward the output sees with the amplifier off.
    ex H0 = 0;
    {
        try {
            Circuit c0 = zero_gain_reference_circuit(c, s.probe_ref, ports);
            AnalysisRequest r;
            r.input_ref = s.input_ref;
            r.output = s.output;
            Solved sv = solve(c0, r);
            H0 = (sv.num / sv.den).normal();
            std::string gs = reference_gain_symbol(*ref);
            if (!gs.empty()) {
                GiNaC::exmap zm;
                zm[pt.get(gs)] = ex(0);
                H0 = H0.subs(zm).normal();
            }
        } catch (const std::exception&) {
            H0 = 0;
        }
    }

    // Phase margin is reported inside the gain/bandwidth block (there is no
    // separate "Stability" section). Its symbolic form is the arctangent sum
    // derived above.
    std::string pm_sym = pm_ENVLP_txt, pm_ENVLP_latex = pm_ENVLP_tex;

    // Closed-loop gain assembled from the asymptotic-gain formula, so the
    // printed expression is exactly what the formula evaluates to:
    //   H = H_inf*T/(1+T) + H_0/(1+T) = (H_inf*T + H_0)/(1+T).
    //
    // Rebuild H_inf and T from their *low-entropy* forms (gain x factors) so a
    // parallel combination stays the held atom (R1||R2) instead of being
    // multiplied back out into R1*R2/(R1+R2), which would make the product
    // high-entropy.
    auto le_expr = [](const Pruned& p) {
        ex e = p.gain;
        for (const auto& f : p.num_factors) e *= f.expr;
        ex d = ex(1);
        for (const auto& f : p.den_factors) d *= f.expr;
        return (e / d).normal();
    };
    ex Hinf = le_expr(ideal.transfer.pruned);
    ex Tle = le_expr(Tp);
    // A high-gain loop has Hcl -> H_inf. In that region expanding
    // (H_inf*T+H0)/(1+T) and pruning it is both unnecessary and extremely
    // expensive (the symbolic cancellation is large); report the already
    // factored asymptotic gain instead. For moderate loop gain keep the exact
    // assembled expression.
    bool use_hinf_hcl = false;
    try {
        double w0 = 2.0 * M_PI * (s.f0_hz > 0.0 ? s.f0_hz : 1.0);
        // Use the unpruned T for the regime test: pruning T can remove the
        // small terms that distinguish a 30 dB loop from a 45 dB one.
        double tm = std::abs(eval_complex(T, pt, w0));
        double hm = std::abs(eval_complex(Hinf, pt, w0));
        double h0m = std::abs(eval_complex(H0, pt, w0));
        use_hinf_hcl = std::isfinite(tm) && tm > 20.0 &&
                        std::isfinite(hm) &&
                        (!std::isfinite(h0m) || h0m < 0.25 * (hm + 1e-30));
    } catch (const std::exception&) {
    }
    Pruned Hcl_p;
    if (use_hinf_hcl) {
        Hcl_p = ideal.transfer.pruned;
    } else {
        ex Hcl_formula = ((Hinf * Tle + H0) / (1 + Tle)).normal();
        ex Hcl_ratio = (Hcl_formula.numer() / Hcl_formula.denom()).normal();
        Hcl_p = prune_low_entropy(Hcl_ratio.numer(), Hcl_ratio.denom(), pt, o);
    }

    // Publish the phase margin on the return-ratio result so the gain/bandwidth
    // metrics block can print it alongside the other characteristics.
    if (pm >= 0.0) {
        res.has_pm = true;
        res.ugf_hz = eng::format_hz(ugf);
        res.pm_deg = pm;
        res.pm_sym = pm_sym;
        res.pm_ENVLP_latex = pm_ENVLP_latex;
    }
    res.report = format_report(res);

    std::string rep = "Return-ratio / loop-gain analysis\n";
    rep += "reference amplifier: " + s.probe_ref + "\n";
    rep += "----------------------------------------\n";
    rep += "\nAsymptotic (ideal) gain:\n  H_inf(s) = " + ideal.text + "\n";
    rep += "\nForward gain with the amplifier off:\n  H_0(s) = " +
           pretty(H0) + "\n";
    rep += "\nReturn ratio:\n  T(s) = -A(s)*beta(s) = " + Tp.text + "\n";
    rep += "\nFeedback factor:\n  beta(s) = " + pretty(beta) + "\n";
    rep += "\nClosed-loop gain:\n";
    rep += "  H(s) = H_inf*T/(1+T) + H_0/(1+T) = " + Hcl_p.text + "\n";
    // The return ratio's own gain/bandwidth (which now carries the phase
    // margin) and poles/zeros, with the leading "H(s) = ..." line stripped.
    rep += "\n" + strip_first_line(res.report);
    rep += "\n";

    // The LaTeX report mirrors the text report section-for-section (the Math
    // tab shows exactly the same content, typeset). Section headings end in
    // ':' so MathPanel renders them as headings, exactly like poles/zeros.
    std::string lrep;
    lrep += "Asymptotic gain:\n";
    lrep += "H_{\\infty}(s) = " + latex_rhs(ideal.latex) + "\n";
    lrep += "Forward gain with the amplifier off:\n";
    lrep += "H_0(s) = " + to_latex(H0) + "\n";
    lrep += "Return ratio:\n";
    lrep += "T(s) = -A(s)\\beta(s) = " + latex_rhs(Tp.latex) + "\n";
    lrep += "Feedback factor:\n";
    lrep += "\\beta(s) = " + to_latex(beta) + "\n";
    lrep += "Closed-loop gain:\n";
    lrep += "H(s) = \\frac{H_{\\infty} T}{1+T} + \\frac{H_0}{1+T} = " +
            latex_rhs(Hcl_p.latex) + "\n";
    lrep += "\n";
    // The gain/bandwidth block (which now includes the phase margin) follows.
    lrep += format_report_latex(res);

    cr.text = "T(s) = " + Tp.text;
    // No headline expression: the return-ratio section already shows T(s), so
    // the typeset tab leads straight with the report and does not repeat it.
    cr.latex.clear();
    cr.latex_report = lrep;
    cr.summary = "H_inf = " + ideal.text + " ;  T = " + Tp.text;
    cr.report = rep;
    cr.has_transfer = true;
    cr.transfer = res;
    return cr;
}

// ---------------------------------------------------------------------------
// Short-circuit current: short the node to ground through a 0 V source and
// report the current through it.
// ---------------------------------------------------------------------------
CardResult analyze_short_circuit(const Circuit& c, const AnalysisSpec& s) {
    // which node?
    std::string node = s.output;
    if (node.size() > 3 && node.front() == 'V' && node.back() == ')')
        node = node.substr(2, node.size() - 3);
    else
        node = s.output;

    Circuit cs = c;
    Component vs;
    vs.kind = Kind::V;
    vs.ref = "__SHORT__";
    vs.nodes = {node, "0"};
    vs.value_text = "0";
    // The short is the *measurement*, not an excitation: it must not drive the
    // network in the superposition, or its own response (the impedance seen
    // looking into the shorted node) would be added to the answer -- which on
    // an RC swamps V1/R1 with the capacitor's current.
    vs.ac_text = "0";
    cs.comps.push_back(vs);

    // The short-circuit current is driven like the AC card: every independent
    // source is excited in turn and the responses superpose.
    AnalysisSpec s2 = s;
    s2.output = "I(__SHORT__)";
    CardResult cr = analyze_ac(cs, s2);
    cr.kind = AnalysisKind::ShortCircuitCurrent;
    cr.title = "Short-circuit current at " + node;

    // The AC card names the internal branch it measures. That name is an
    // implementation detail, so the result says what the current is instead.
    const std::string branch = "I(__SHORT__)";
    const std::string branch_tex = "I(\\_\\_SHORT\\_\\_)";
    const std::string nice = "Isc(" + node + ")";
    auto swap = [&](std::string t) {
        size_t p;
        while ((p = t.find(branch_tex)) != std::string::npos)
            t.replace(p, branch_tex.size(), nice);
        while ((p = t.find(branch)) != std::string::npos)
            t.replace(p, branch.size(), nice);
        return t;
    };
    cr.report = swap(cr.report);
    cr.latex_report = swap(cr.latex_report);
    cr.latex = swap(cr.latex);
    // The AC card's report header and its per-source terms are about the
    // sources; the measured quantity is the short's current. Report that.
    const std::string ac_head =
        "AC (small-signal) -- superposition of the sources' AC values\n";
    if (cr.report.compare(0, ac_head.size(), ac_head) == 0)
        cr.report = "Short-circuit current at " + node +
                    " (current flowing out of the node into the short)\n" +
                    cr.report.substr(ac_head.size());
    const std::string ac_tex = "AC (superposition of the sources' AC values):\n";
    if (cr.latex_report.compare(0, ac_tex.size(), ac_tex) == 0)
        cr.latex_report = "Short-circuit current at " + node + ":\n" +
                          cr.latex_report.substr(ac_tex.size());
    // Name the result after the measured current, in the report and in the
    // transfer the plot is built from.
    cr.transfer.label = nice;
    cr.transfer.report = swap(cr.transfer.report);
    cr.text = nice + " = " + cr.transfer.pruned.text;
    cr.summary = cr.text;
    return cr;
}

// ---------------------------------------------------------------------------
// Input impedance: Zin = V(in)/I(in) seen by the excitation.
//
// The generic, source-agnostic definition: apply a 1 A test current *into the
// input node* and read the voltage there. This works whether the driving
// source is a voltage or a current source (and does not depend on the source
// being of one particular type). The input node is the node the configured
// input source's first terminal sits on.
// ---------------------------------------------------------------------------
CardResult analyze_zin(const Circuit& c, const AnalysisSpec& s) {
    // Input impedance = the input source's OWN response, exactly the way the
    // EET treats an impedance: the "transfer function" is defined as the
    // source's own output, so there is nothing to specify beyond the source.
    //
    //   * current source  -> Zin = V(source)/1A : the node voltage it sees.
    //   * voltage source  -> Zin = V(source)/I(source) = 1/(I/V) : the current
    //     it must deliver. (I(source)/V(source) is the driving-point
    //     admittance, so the impedance is its reciprocal.)
    //
    // Both come out of one ordinary solve: drive the source with a unit
    // excitation and read the *other* quantity. No test source is added, the
    // output is not asked for, and the result is in low-entropy form.
    const Component* src = c.find(s.input_ref);
    if (!src || !is_independent_source(src->kind) || src->nodes.empty()) {
        CardResult cr;
        cr.kind = AnalysisKind::InputImpedance;
        cr.title = "Input impedance";
        cr.summary = "set the input source (a V or I source) whose impedance "
                     "to measure";
        cr.report = cr.summary + "\n";
        return cr;
    }
    const std::string node = src->nodes[0];
    const bool is_current = (src->kind == Kind::I);

    // Drive the source with 1 (the other sources are zeroed by the solver, as
    // for every transfer function) and read the quantity that shares its
    // terminal. V(node) is a node voltage; I(ref) is the branch current, which
    // the solver already exposes for a voltage source.
    const std::string probe =
        is_current ? ("V(" + node + ")") : ("I(" + s.input_ref + ")");
    RawTF rt = raw_tf(c, s.input_ref, probe);
    ex driven = (rt.num / rt.den).normal(); // V(node) per A, or I(src) per V

    // Zin is the voltage-per-amp in both cases. The engine's source convention
    // is that the source's value is the current flowing + to - *through* the
    // source, which is the reverse of the current delivered to the network at
    // the + terminal. So for both source kinds the impedance is the negative
    // of the raw response: V(node)/I(src) for a current source, and
    // I(src)/V(src) -> reciprocal (also negated) for a voltage source.
    bool infinite = false;
    ex Z;
    if (is_current) {
        Z = driven.is_zero() ? ex(0) : (-driven).normal();
    } else if (driven.is_zero()) {
        infinite = true;
        Z = ex(0);
    } else {
        Z = (ex(-1) / driven).normal();
    }

    PruneOptions o = opts_of(s);
    o.octc = rt.octc;
    ParamTable pt = rt.params;

    CardResult cr;
    cr.kind = AnalysisKind::InputImpedance;
    cr.title = "Input impedance";
    if (infinite) {
        cr.text = "Zin = infinite (the source drives an open circuit)";
        cr.latex = "Z_{in}(s) = \\infty";
        cr.summary = cr.text;
        cr.report = "Input impedance seen by " + s.input_ref + " (at " + node +
                    ")\n----------------------------------------\n  " + cr.text +
                    "\n";
        cr.latex_report = "Input impedance:\n" + cr.latex + "\n";
        return cr;
    }
    Pruned p = prune_low_entropy(Z.numer(), Z.denom(), pt, o);

    cr.text = "Zin = " + p.text;
    std::string zl = p.latex;
    if (zl.rfind("H(s) = ", 0) == 0) zl = zl.substr(7);
    cr.latex = "Z_{in}(s) = " + zl;
    cr.summary = cr.text;
    std::string rep = "Input impedance seen by " + s.input_ref + " (at " +
                      node + ")\n";
    rep += "----------------------------------------\n";
    rep += "  Zin(s) = " + p.text + "\n";
    cr.report = rep;
    // LaTeX report mirrors the text report.
    cr.latex_report = "Input impedance:\nZ_{in}(s) = " + zl + "\n";
    cr.has_transfer = true;
    {
        AnalysisResult res;
        res.label = "Z_{in}(s)";
        res.input_desc = s.input_ref;
        res.output_desc = "Zin";
        res.sweep = s.sweep;
        res.num_raw = Z.numer();
        res.den_raw = Z.denom();
        res.params = pt;
        res.opts = o;
        res.pruned = p;
        res.report = format_report(res);
        cr.transfer = res;
    }
    return cr;
}

// ---------------------------------------------------------------------------
// Output impedance: drive the output node with a 1 A test current and read
// the voltage -- Zout = V(out)/1A, with the input source zeroed.
// ---------------------------------------------------------------------------
CardResult analyze_zout(const Circuit& c, const AnalysisSpec& s) {
    std::string node = s.output;
    if (node.size() > 3 && node.front() == 'V' && node.back() == ')')
        node = node.substr(2, node.size() - 3);

    // Turn off *every* independent source (the output impedance is a property
    // of the network alone -- the driven input is irrelevant), then add a 1 A
    // test current *into* the node. With the SPICE convention ({n+,n-}: current
    // flows n+ -> n- through the source), a source pushing 1 A into `node` has
    // its n- terminal there: {0, node}.
    Circuit cs = c;
    for (auto& cc : cs.comps)
        if (is_independent_source(cc.kind)) cc.value_text = "0";
    Component it;
    it.kind = Kind::I;
    it.ref = "__ITEST__";
    it.nodes = {"0", node};
    it.value_text = "1";
    cs.comps.push_back(it);

    AnalysisSpec s2 = s;
    s2.input_ref = "__ITEST__";
    s2.output = "V(" + node + ")";
    CardResult cr = analyze_tf(cs, s2);
    cr.kind = AnalysisKind::OutputImpedance;
    cr.title = "Output impedance at " + node;
    cr.text = "Zout = " + cr.text;
    cr.summary = cr.text;
    if (cr.latex.rfind("H(s) = ", 0) == 0)
        cr.latex = "Z_{out}(s) = " + cr.latex.substr(7);
    else if (!cr.latex.empty())
        cr.latex = "Z_{out}(s) = " + cr.latex;
    // The transfer is still useful for the plot, but it is an impedance: name
    // it so the report does not call it H(s).
    cr.transfer.label = "Z_{out}(s)";
    cr.transfer.report = format_report(cr.transfer);
    {
        // The report's leading line carries the old name.
        const std::string pfx = "H(s) = ";
        std::string& rep = cr.report;
        if (rep.rfind(pfx, 0) == 0)
            rep = "Zout(s) = " + rep.substr(pfx.size());
    }
    return cr;
}

// ---------------------------------------------------------------------------
// Noise: output- and input-referred noise over the sweep band.
//
// Every noisy element is modelled as a current source between its physical
// terminals (thermal 4kT/R for resistors, 4kT*(2/3)*gm + 1/f for MOSFETs,
// shot + 1/f for BJTs/diodes), or as a voltage source in series with an
// amplifier's input (en, the op-amp's input voltage noise density). Each
// source's transfer to the output is found once symbolically and evaluated
// across the sweep; powers add. Flicker noise follows the standard
//   i_n^2(f) = i_th^2 * (1 + fcn/f)
// so `fcn` is the 1/f corner frequency where flicker equals thermal.
//
// Input referral: with a voltage-source excitation the result is V/sqrt(Hz);
// with a current-source excitation it is A/sqrt(Hz) (input-referred current
// noise). The integrated (rms) noise over the sweep band is also reported.
// ---------------------------------------------------------------------------
CardResult analyze_noise(const Circuit& c, const AnalysisSpec& s) {
    CardResult cr;
    cr.kind = AnalysisKind::Noise;
    cr.title = "Noise";

    const double kT = 1.380649e-23 * 300.15; // kT at 300.15 K
    const double q = 1.602176634e-19;        // electron charge

    ParamTable pt;
    // Register every component symbol/estimate so the noise transfer functions
    // can be evaluated numerically (independent of the excitation type).
    for (const auto& cc : c.comps) {
        try {
            MnaSystem sys = build_mna(c, cc.ref);
            pt = sys.params;
            break;
        } catch (const std::exception&) {
        }
    }
    ex s_sym = pt.get("s");

    // Sweep band (for the spectrum, corner frequency and integrated noise).
    double f0 = s.sweep.f_start_hz > 0 ? s.sweep.f_start_hz : 1.0;
    double f1 = s.sweep.f_stop_hz > f0 ? s.sweep.f_stop_hz : f0 * 1e6;
    if (f1 <= f0) f1 = f0 * 1e3;
    int npts = s.sweep.points_per_interval > 0 ? s.sweep.points_per_interval * 20
                                               : 200;
    npts = std::max(50, std::min(2000, npts));
    std::vector<double> freqs = sweep_hz(f0, f1, npts);

    // ---- collect noise sources -------------------------------------------
    struct Src {
        std::string ref, mech;
        bool voltage = false;         // true: series voltage source
        std::string na, nb;           // terminals
        double psd_const = 0.0;       // A^2/Hz (or V^2/Hz) thermal/shot part
        double corner_hz = 0.0;       // 1/f corner (0 = no flicker)
        ex xfer;                      // unit-source transfer to V(out)
        ex psd_sym;                   // symbolic thermal/shot PSD
        ex psd_flick_sym;             // symbolic flicker coefficient (times 1/f)
        bool unity = false;           // amplifier en: appears at the output with
                                      // unity gain (see below), so no transfer
                                      // solve is needed
    };
    // kT is a symbolic design constant so the noise densities stay analytic
    // (e.g. 4*kT/R1) rather than collapsing to a decimal.
    ex kT_sym = pt.get("kT");
    pt.set("kT", kT, UnitClass::Plain);
    ex q_sym = pt.get("q");
    pt.set("q", q, UnitClass::Plain);

    std::vector<Src> srcs;
    for (const auto& cc : c.comps) {
        if (cc.kind == Kind::R) {
            double R = cc.estimate();
            if (R > 0.0) {
                // Mirror-aware: a copied resistor's noise is expressed via its
                // unit (R2 -> 4kT/R1), so a "copy of" ratio stays collapsed.
                ex Rs = value_symbol_scaled(pt, c, cc);
                srcs.push_back({cc.ref, "4kT/R", false, cc.nodes[0],
                                cc.nodes[1], 4.0 * kT / R, 0.0, ex(0),
                                4 * kT_sym / Rs, ex(0)});
            }
        } else if (cc.kind == Kind::NMOS || cc.kind == Kind::PMOS) {
            double gm = cc.param_estimate("gm");
            if (gm > 0.0) {
                double fcn = cc.param_enabled("fcn")
                                 ? cc.param_estimate("fcn")
                                 : 0.0;
                // Mirror-aware: a copied device's gm is mult*gm_unit, so the
                // noise of, say, four parallel devices is expressed via M1.
                ex gms = reg_param(pt, cc, "gm");
                // thermal: 4kT*(2/3)*gm; flicker coefficient: (thermal)*fcn,
                // with fcn the 1/f corner frequency kept symbolic.
                ex i_th = 4 * kT_sym * (ex(2) / 3) * gms;
                // The 1/f corner is a property of the unit device, so a copy
                // shares M1's fcn symbol.
                ex fcn_sym = reg_param(pt, cc, "fcn");
                ex i_fl = fcn > 0.0 ? i_th * fcn_sym : ex(0);
                srcs.push_back({cc.ref, "4kT(2/3)gm + 1/f", false, cc.nodes[0],
                                cc.nodes[2], 4.0 * kT * (2.0 / 3.0) * gm, fcn,
                                ex(0), i_th, i_fl});
            }
        } else if (cc.kind == Kind::NPN || cc.kind == Kind::PNP) {
            double gm = cc.param_estimate("gm");
            double beta = 100.0;
            double ic = gm * kT / q;
            double ib = ic / beta;
            double i2 = 2.0 * q * (ic + ib);
            if (i2 > 0.0) {
                double fcn = cc.param_enabled("fcn")
                                 ? cc.param_estimate("fcn")
                                 : 0.0;
                srcs.push_back({cc.ref, "2q(Ic+Ib) + 1/f", false, cc.nodes[0],
                                cc.nodes[2], i2, fcn, ex(0),
                                GiNaC::numeric(i2), GiNaC::numeric(i2 * fcn)});
            }
        } else if (cc.kind == Kind::D) {
            double rd = cc.param_estimate("rd");
            double gd = rd > 0.0 ? 1.0 / rd : 0.0;
            double id = gd * kT / q;
            double i2 = 2.0 * q * id + 4.0 * kT * gd * (2.0 / 3.0);
            if (i2 > 0.0) {
                double fcn = cc.param_enabled("fcn")
                                 ? cc.param_estimate("fcn")
                                 : 0.0;
                srcs.push_back({cc.ref, "2qId + 4kTgm + 1/f", false, cc.nodes[0],
                                cc.nodes[1], i2, fcn, ex(0),
                                GiNaC::numeric(i2), GiNaC::numeric(i2 * fcn)});
            }
        } else if (cc.kind == Kind::OPAMP || cc.kind == Kind::FDOPAMP ||
                   cc.kind == Kind::AMP) {
            // input voltage noise density en (V/sqrt(Hz)), optional 1/f below
            // en_flicker ... simplified: en is the flat density, and en_flicker
            // is the density at 1 Hz, so the corner is (en_flicker/en)^2.
            if (!cc.param_enabled("en")) continue;
            double en = cc.param_estimate("en");
            if (!(en > 0.0)) continue;
            double en1 = cc.param_enabled("en_flicker")
                             ? cc.param_estimate("en_flicker")
                             : 0.0;
            double fcn = 0.0;
            if (en1 > en) fcn = (en1 / en) * (en1 / en); // where 1/f = flat
            std::string ip = cc.nodes.size() > 0 ? cc.nodes[0] : "0";
            std::string im = cc.nodes.size() > 1 ? cc.nodes[1] : "0";
            ex ens = pt.get("en_" + cc.ref);
            pt.set("en_" + cc.ref, en, UnitClass::Plain);
            ex v_th = ens * ens;
            ex v_fl = fcn > 0.0 ? v_th * GiNaC::numeric(fcn) : ex(0);
            // From en's own perspective the amplifier is in unity-gain
            // configuration, so its input voltage noise appears at the output
            // with |H_en|^2 = 1 (the in-band value). No transfer solve needed.
            Src s_en{cc.ref, "en (input voltage)", true, ip, im, en * en, fcn,
                     ex(1), v_th, v_fl};
            s_en.unity = true;
            srcs.push_back(s_en);
        }
    }
    if (srcs.empty()) {
        cr.summary = "No noise sources (no resistors, devices or amplifiers)";
        cr.report = cr.summary + "\n";
        return cr;
    }

    // ---- transfer of each unit source to V(out) --------------------------
    auto xfer_from_current = [&](const std::string& na, const std::string& nb)
        -> ex {
        Circuit ct = c;
        Component it;
        it.kind = Kind::I;
        it.ref = "__INOISE__";
        it.nodes = {na, nb};
        it.value_text = "1";
        ct.comps.push_back(it);
        try {
            RawTF t = raw_tf(ct, "__INOISE__", s.output);
            return (t.num / t.den).normal();
        } catch (const std::exception&) {
            return ex(0);
        }
    };
    auto xfer_from_voltage = [&](const std::string& na, const std::string& nb)
        -> ex {
        Circuit ct = c;
        Component vt;
        vt.kind = Kind::V;
        vt.ref = "__VNOISE__";
        vt.nodes = {na, nb};
        vt.value_text = "1";
        ct.comps.push_back(vt);
        try {
            RawTF t = raw_tf(ct, "__VNOISE__", s.output);
            return (t.num / t.den).normal();
        } catch (const std::exception&) {
            return ex(0);
        }
    };
    for (auto& src : srcs) {
        if (src.unity) continue; // en already has its output gain (= 1) set
        src.xfer = src.voltage ? xfer_from_voltage(src.na, src.nb)
                               : xfer_from_current(src.na, src.nb);
    }

    // ---- forward transfer H_fwd(s): excitation -> V(out) -----------------
    // Everything is referred to the input by dividing by |H_fwd|^2. Its units
    // follow the excitation (V/A transimpedance for a current input, V/V for a
    // voltage input), so the input noise density is A^2/Hz or V^2/Hz
    // accordingly.
    ex fwd;
    bool input_is_current = false;
    {
        std::vector<std::string> cands;
        if (!s.input_ref.empty()) cands.push_back(s.input_ref);
        for (const auto& cc : c.comps)
            if (is_independent_source(cc.kind) && cc.ref != s.input_ref)
                cands.push_back(cc.ref);
        for (const auto& src : cands) {
            const Component* sc = c.find(src);
            if (!sc) continue;
            try {
                RawTF t = raw_tf(c, src, s.output);
                ex H = (t.num / t.den).normal();
                if (!H.is_zero()) {
                    fwd = H;
                    input_is_current = (sc->kind == Kind::I);
                    break;
                }
            } catch (const std::exception&) {
            }
        }
    }

    // ---- low-entropy symbolic |H(j2*pi*f)|^2 -----------------------------
    // Factor the transfer into gain x (1 + s*tau) factors (|| atoms held), then
    //   |H(jw)|^2 = K^2 * prod_num |brick(jw)|^2 / prod_den |brick(jw)|^2
    // with w = 2*pi*f and |1 + s*tau at s=jw|^2 = 1 + (w*tau)^2 built as
    // re^2 + im^2 per brick. Crucially this never expands or rationalizes the
    // whole expression: the result stays a product of physical brackets (e.g.
    // (Cgs+Cgd)*R2) rather than a single giant polynomial ratio.
    ex f_sym = pt.get("f");
    pt.set("f", s.f0_hz, UnitClass::Plain);
    ex w_expr = 2 * GiNaC::Pi * f_sym;
    auto brick2 = [&](const ex& b) -> ex {
        int deg = 0;
        try { deg = b.has(s_sym) ? b.degree(s_sym) : 0; } catch (...) { deg = 0; }
        if (deg <= 0) { ex c0 = b.coeff(s_sym, 0); return c0 * c0; }
        if (deg == 1) {
            ex c0 = b.coeff(s_sym, 0), c1 = b.coeff(s_sym, 1);
            return c0 * c0 + (c1 * w_expr) * (c1 * w_expr);
        }
        // 1 + a*s + b*s^2 (a complex pair): |.|^2 = (1 - b w^2)^2 + (a w)^2
        ex c0 = b.coeff(s_sym, 0), c1 = b.coeff(s_sym, 1),
           c2 = b.coeff(s_sym, 2);
        ex re = c0 - c2 * w_expr * w_expr, im = c1 * w_expr;
        return re * re + im * im;
    };
    auto mag2f = [&](const ex& H) -> ex {
        if (H.is_zero()) return ex(0);
        try {
            ex ratio = (H.numer() / H.denom()).normal();
            Pruned p = prune_low_entropy(ratio.numer(), ratio.denom(), pt,
                                         opts_of(s));
            ex n2 = p.gain * p.gain;
            for (const auto& fac : p.num_factors) n2 *= brick2(fac.expr);
            ex d2 = ex(1);
            for (const auto& fac : p.den_factors) d2 *= brick2(fac.expr);
            if (d2.is_zero()) return ex(0);
            return n2 / d2; // raw product/quotient, no normal()/expand()
        } catch (const std::exception&) {
            return ex(0);
        }
    };

    // ---- symbolic magnitude-squared |H(j2*pi*f)|^2 (real coefficients) ---
    // With real component values H(-jw) = conj(H(jw)), so |H(jw)|^2 =
    // H(jw)*H(-jw). Substituting s = j*2*pi*f and using H(-jw) removes the
    // ---- output noise spectrum -------------------------------------------
    auto psd_of = [](const Src& src, double f) {
        double p = src.psd_const;
        if (src.corner_hz > 0.0 && f > 0.0)
            p *= (1.0 + src.corner_hz / f);
        return p;
    };
    std::vector<double> vout(npts, 0.0);    // output V^2/Hz
    std::vector<double> vin(npts, 0.0);     // input-referred (A^2/Hz or V^2/Hz)
    std::vector<std::pair<std::string, double>> per_source;    // output V^2
    std::vector<std::pair<std::string, double>> per_source_in; // input A^2/V^2
    for (auto& src : srcs) {
        // trapezoid in log-f for the integrated power (output and input)
        double area = 0.0, area_in = 0.0;
        for (int i = 1; i < npts; ++i) {
            double w1 = 2.0 * M_PI * freqs[i];
            double x2 = std::norm(eval_complex(src.xfer, pt, w1));
            double c = psd_of(src, freqs[i]) * x2;
            double w0 = 2.0 * M_PI * freqs[i - 1];
            double x2m = std::norm(eval_complex(src.xfer, pt, w0));
            double cm = psd_of(src, freqs[i - 1]) * x2m;
            double lf = std::log(freqs[i] / freqs[i - 1]);
            area += 0.5 * (c * freqs[i] + cm * freqs[i - 1]) * lf;
            vout[i] += c;
            vout[i - 1] += cm;
            // input-referred: divide by |H_fwd(f)|^2 (the same referral)
            double hf1 = std::norm(eval_complex(fwd, pt, w1));
            double hf0 = std::norm(eval_complex(fwd, pt, w0));
            double ci = hf1 > 0.0 ? c / hf1 : 0.0;
            double cim = hf0 > 0.0 ? cm / hf0 : 0.0;
            area_in += 0.5 * (ci * freqs[i] + cim * freqs[i - 1]) * lf;
            vin[i] += ci;
            vin[i - 1] += cim;
        }
        per_source.push_back({src.ref, area});
        per_source_in.push_back({src.ref, area_in});
    }
    // total integrated output noise power (V rms^2) over the band
    double vout_total2 = 0.0;
    for (const auto& ps : per_source) vout_total2 += ps.second;
    double vin_total2 = 0.0;
    for (const auto& ps : per_source_in) vin_total2 += ps.second;

    // ---- symbolic output noise density -----------------------------------
    // Whiteboard-style decomposition: the output noise is a sum of per-source
    // contributions, each = (source current-noise density) x |H(j2*pi*f)|^2,
    // where H(s) is the transfer from that source's noise current to V(out).
    // Sources that share the same H(s) are grouped, so the total reads as
    //   S_v(f) = |H(s)|^2 * (i_R1^2 + i_M1,thermal^2 + i_M1,flicker^2/f + ...)
    // with the current densities and H(s) listed separately, exactly as an
    // engineer would write it by hand. Thermal and flicker are kept apart.
    struct Group {
        ex h2;                       // |H(j2*pi*f)|^2 (bracket product)
        ex i_th = 0;                 // sum of thermal/shot densities
        ex i_fl = 0;                 // sum of flicker coefficients (x 1/f)
        std::vector<std::string> refs;
    };
    std::vector<Group> groups;
    // Per-source density expressions for the breakdown (thermal and flicker).
    // `voltage` marks the amplifier's input voltage noise (e_n), whose symbol
    // is a voltage density, not a current density (i_n).
    struct SrcDens {
        std::string ref, mech;
        ex th, fl;                   // thermal density, flicker coefficient
        double th_num = 0.0, fl_num = 0.0;
        bool voltage = false;
    };
    std::vector<SrcDens> dens;
    for (const auto& src : srcs) {
        ex h2 = mag2f(src.xfer);
        if (h2.is_zero()) continue;
        ex th = src.psd_sym.is_zero() ? ex(GiNaC::numeric(src.psd_const))
                                      : src.psd_sym;
        ex fl = src.psd_flick_sym;
        dens.push_back({src.ref, src.mech, th, fl, src.psd_const,
                        src.corner_hz > 0.0
                            ? src.psd_const * src.corner_hz
                            : 0.0,
                        src.voltage});
        bool merged = false;
        for (auto& g : groups)
            if (g.h2.is_equal(h2)) {
                g.i_th += th;
                if (!fl.is_zero()) g.i_fl += fl;
                g.refs.push_back(src.ref);
                merged = true;
                break;
            }
        if (!merged) {
            Group g;
            g.h2 = h2;
            g.i_th = th;
            g.i_fl = fl;
            g.refs.push_back(src.ref);
            groups.push_back(g);
        }
    }
    // ---- output and input noise ------------------------------------------
    // Output: each source contributes its own PSD times |H_j(jw)|^2, where H_j
    // is the transfer from that source's noise current (or the amp's en
    // voltage) to V(out). Units: A^2/Hz * (V/A)^2 = V^2/Hz for a current
    // source, V^2/Hz * 1 = V^2/Hz for the amplifier's en.
    // Input: divide the whole output density by |H_fwd(jw)|^2 (the excitation
    // -> V(out) transfer). The device/resistor currents (H_j = H_fwd) then
    // reduce to their densities, while the amp's en keeps en^2/|H_fwd|^2.

    // Numeric input-referred density at f0 = S_out(f0)/|H_fwd(f0)|^2.
    double hfwd2_f0 = 1.0;
    if (!fwd.is_zero()) {
        double hv = std::norm(eval_complex(fwd, pt, 2.0 * M_PI * f0));
        if (hv > 0.0) hfwd2_f0 = hv;
    }

    auto db20 = [](double x) -> double {
        return x > 0 ? 20.0 * std::log10(x) : -1e300;
    };

    double vout_rms = std::sqrt(vout_total2);
    // The input-referred rms is the output rms divided by |H_fwd(f0)| (the
    // same referral the density uses).
    double gain0 = std::sqrt(hfwd2_f0);
    double iin_rms = input_is_current && gain0 > 0 ? vout_rms / gain0 : 0.0;
    double vin_rms = !input_is_current && gain0 > 0 ? vout_rms / gain0 : 0.0;

    // Output density at f0 and input-referred density at f0 (numeric).
    double sv_at_f0 = npts > 0 ? vout[0] : 0.0;
    double sin_at_f0 = sv_at_f0 / hfwd2_f0;

    // ---- symbolic report (text + LaTeX) ----------------------------------
    // Whiteboard-style, split into an output section and an input section.
    // Output: S_out(f) = sum_j |H_j|^2 * (i_j,th + i_j,fl/f), where H_j is the
    // transfer from source j to V(out) (V/A for a current source, 1 for the
    // amp's en).
    // Input:  S_in(f)  = S_out(f) / |H_fwd|^2, so the amp's en term becomes
    // en^2/|H_fwd|^2 while the device/resistor terms (H_j = H_fwd) lose it.
    // Render a transfer in low-entropy factored form.
    auto render_xfer = [&](const ex& H, std::string& txt, std::string& tex) {
        try {
            ex ratio = (H.numer() / H.denom()).normal();
            Pruned hp = prune_low_entropy(ratio.numer(), ratio.denom(), pt,
                                          opts_of(s));
            txt = hp.text;
            tex = latex_rhs(hp.latex);
        } catch (const std::exception&) {
            txt = pretty(H);
            tex = to_latex(H);
        }
    };
    // Forward transfer (excitation -> out), named H(s).
    std::string fwd_txt, fwd_tex;
    render_xfer(fwd, fwd_txt, fwd_tex);
    // Name each group: the forward transfer is "H"; a unity group (the amp's
    // en) has no transfer symbol at all; others get H1/H2/...
    auto inner_expr = [&](const Group& g, std::string& txt,
                          std::string& tex) {
        txt = pretty(g.i_th);
        tex = to_latex(g.i_th);
        if (!g.i_fl.is_zero()) {
            txt += " + " + pretty(g.i_fl) + "/f";
            tex += " + \\frac{" + to_latex(g.i_fl) + "}{f}";
        }
    };
    // Source-noise-density list (shared by both portions).
    std::string dens_txt, dens_tex;
    {
        std::ostringstream t, x;
        for (const auto& d : dens) {
            // The amplifier's en is a voltage-noise density (V^2/Hz); the
            // device/resistor sources are current-noise densities (A^2/Hz).
            const char* sym = d.voltage ? "e_n^2" : "i_n^2";
            const char* ENVLP_th = d.voltage ? "e_{n,th}^2" : "i_{n,th}^2";
            const char* ENVLP_fl = d.voltage ? "e_{n,1/f}^2" : "i_{n,1/f}^2";
            const char* unit = d.voltage ? "V" : "A";
            t << "    " << d.ref << " (thermal): " << sym << " = " << pretty(d.th)
              << " " << unit << "^2/Hz\n";
            x << "\\mathrm{" << d.ref << "}:\\quad " << ENVLP_th << " = "
              << to_latex(d.th) << "\\ " << unit << "^2/Hz\n";
            if (!d.fl.is_zero()) {
                t << "    " << d.ref << " (flicker): " << sym << " = "
                  << pretty(d.fl) << "/f " << unit << "^2/Hz\n";
                x << "\\mathrm{" << d.ref << "}:\\quad " << ENVLP_fl
                  << " = \\frac{" << to_latex(d.fl) << "}{f}\\ " << unit
                  << "^2/Hz\n";
            }
        }
        dens_txt = t.str();
        dens_tex = x.str();
    }
    // Symbolic OUTPUT density: S_v(f) = sum_j |H_j|^2 * (i_j,th + i_j,fl/f).
    std::string out_ENVLP_txt, out_ENVLP_tex;
    {
        std::ostringstream t, x;
        std::string tt = "    S_v(f) = ";
        std::string xx = "S_{v}(f) = ";
        for (size_t j = 0; j < groups.size(); ++j) {
            const Group& g = groups[j];
            std::string in_t, in_x;
            inner_expr(g, in_t, in_x);
            bool unity = g.h2.is_equal(ex(1));
            tt += std::string(j ? " + " : "");
            xx += std::string(j ? " + " : "");
            if (unity) {
                // the amp's en is already an output voltage density
                tt += "(" + in_t + ")";
                xx += "\\left(" + in_x + "\\right)";
            } else {
                tt += "H(s)^2 * (" + in_t + ")";
                xx += "\\left|H\\right|^2\\left(" + in_x + "\\right)";
            }
        }
        t << tt << "\n";
        x << xx << "\n";
        out_ENVLP_txt = t.str();
        out_ENVLP_tex = x.str();
    }
    // Symbolic INPUT density: S_i(f) = S_v(f)/|H|^2, so the device/resistor
    // currents drop their H(s)^2 and the amp's en becomes en^2/H(s)^2.
    std::string in_ENVLP_txt, in_ENVLP_tex;
    {
        std::ostringstream t, x;
        std::string it = "    S_i(f) = ";
        std::string ix = "S_{i}(f) = ";
        for (size_t j = 0; j < groups.size(); ++j) {
            const Group& g = groups[j];
            std::string in_t, in_x;
            inner_expr(g, in_t, in_x);
            bool unity = g.h2.is_equal(ex(1));
            it += std::string(j ? " + " : "");
            ix += std::string(j ? " + " : "");
            if (unity) {
                it += "(" + in_t + ")/H(s)^2";
                ix += "\\frac{" + in_x + "}{\\left|H\\right|^2}";
            } else {
                it += "(" + in_t + ")";
                ix += "\\left(" + in_x + "\\right)";
            }
        }
        t << it << "\n";
        x << ix << "\n";
        in_ENVLP_txt = t.str();
        in_ENVLP_tex = x.str();
    }


    // ---- reports: text and LaTeX, section-for-section identical ----------
    // Both list the sources, the density at f0, the integrated result, the
    // symbolic forms, then the per-source breakdown, in that order.
    std::sort(per_source.begin(), per_source.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    double tot = vout_total2 > 0 ? vout_total2 : 1e-300;

    // per_source_in was built in the same numeric loop as per_source.
    std::sort(per_source_in.begin(), per_source_in.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    double tot_in = vin_total2 > 0 ? vin_total2 : 1e-300;

    std::string rep, lrep;
    {
        std::ostringstream t, x;
        // Header
        std::string hdr = "Noise Analysis   " + eng::format_hz(f0) + " .. " +
                          eng::format_hz(f1);
        t << hdr << "\n========================================\n";
        x << "Noise:\\quad \\mathrm{" << eng::latex_safe(eng::format_hz(f0))
          << "} .. \\mathrm{" << eng::latex_safe(eng::format_hz(f1)) << "}\n";

        // ============================ OUTPUT =============================
        t << "\nOutput Noise\n------------\n";
        x << "Output Noise:\n";
        // density symbolic
        t << "  Density S_v(f) [V^2/Hz]:\n" << out_ENVLP_txt;
        x << "\\mathrm{Density}\\ S_{v}(f):\n" << out_ENVLP_tex;
        // density at f0 (numeric)
        {
            char b[200];
            std::snprintf(b, sizeof(b),
                          "    S_v(%s) = %.4g V^2/Hz   (%.2f dBV^2/Hz, "
                          "%.4g V/sqrt(Hz))\n",
                          eng::format_hz(f0).c_str(), sv_at_f0,
                          sv_at_f0 > 0 ? 10.0 * std::log10(sv_at_f0) : -1e300,
                          std::sqrt(std::max(0.0, sv_at_f0)));
            t << b;
            x << "S_{v} = \\mathrm{" << eng::format_si(sv_at_f0, 4)
              << "\\ V^2/Hz}\n";
        }
        // integrated
        {
            char b[160];
            std::snprintf(b, sizeof(b),
                          "    Integrated: V_n,out = %.4g V rms   (%.2f dBV)\n",
                          vout_rms, db20(vout_rms));
            t << b;
            x << "\\mathrm{Integrated:}\\ \\mathrm{V_{n,out}} = \\mathrm{"
              << eng::format_si(vout_rms, 4) << "\\ V_{rms}}\n";
        }
        t << "  Contribution:\n";
        x << "\\mathrm{Contribution:}\n";
        for (const auto& ps : per_source) {
            std::string pct = eng::format_percent(100.0 * ps.second / tot, 3);
            char sb[192];
            std::snprintf(sb, sizeof(sb), "    %-8s %8s %%   %.3g V^2\n",
                          ps.first.c_str(), pct.c_str(), ps.second);
            t << sb;
            x << "\\mathrm{" << ps.first << "} = " << pct << "\\%\n";
        }

        // ============================ INPUT ==============================
        const char* in_unit = input_is_current ? "A" : "V";
        t << "\nInput Noise\n-----------\n";
        x << "Input Noise:\n";
        t << "  Density S_i(f) [" << in_unit << "^2/Hz]:\n" << in_ENVLP_txt;
        x << "\\mathrm{Density}\\ S_{i}(f):\n" << in_ENVLP_tex;
        {
            char b[200];
            std::snprintf(b, sizeof(b),
                          "    S_i(%s) = %.4g %s^2/Hz   (%.2f dB%s^2/Hz)\n",
                          eng::format_hz(f0).c_str(), sin_at_f0, in_unit,
                          sin_at_f0 > 0 ? 10.0 * std::log10(sin_at_f0) : -1e300,
                          in_unit);
            t << b;
            x << "S_{i} = \\mathrm{" << eng::format_si(sin_at_f0, 4) << "\\ "
              << in_unit << "^2/Hz}\n";
        }
        if (input_is_current) {
            char b[192];
            std::snprintf(b, sizeof(b),
                          "    Integrated: I_n,in = %.4g A rms   (%.2f dBA)\n",
                          iin_rms, db20(iin_rms));
            t << b;
            x << "\\mathrm{Integrated:}\\ \\mathrm{I_{n,in}} = \\mathrm{"
              << eng::format_si(iin_rms, 4) << "\\ A_{rms}}\n";
        } else {
            char b[192];
            std::snprintf(b, sizeof(b),
                          "    Integrated: V_n,in = %.4g V rms   (%.2f dBV)\n",
                          vin_rms, db20(vin_rms));
            t << b;
            x << "\\mathrm{Integrated:}\\ \\mathrm{V_{n,in}} = \\mathrm{"
              << eng::format_si(vin_rms, 4) << "\\ V_{rms}}\n";
        }
        t << "  Contribution:\n";
        x << "\\mathrm{Contribution:}\n";
        for (const auto& ps : per_source_in) {
            std::string pct =
                eng::format_percent(100.0 * ps.second / tot_in, 3);
            char sb[192];
            std::snprintf(sb, sizeof(sb), "    %-8s %8s %%\n",
                          ps.first.c_str(), pct.c_str());
            t << sb;
            x << "\\mathrm{" << ps.first << "} = " << pct << "\\%\n";
        }

        // The symbolic definitions (source densities and H(s)) close the
        // report, after the two quantities the user reads first.
        t << "\nSource Noise Densities\n----------------------\n" << dens_txt;
        x << "Source Noise Densities:\n" << dens_tex;
        t << "\nTransducer\n----------\n    H(s) = " << fwd_txt << "\n";
        x << "Transducer:\\quad H(s) = " << fwd_tex << "\n";

        rep = t.str();
        lrep = x.str();
    }

    // ---- publish spectrum + metrics on the result ------------------------
    cr.has_transfer = false;
    {
        AnalysisResult res;
        res.input_desc = s.input_ref;
        res.output_desc = s.output;
        res.params = pt;
        res.sweep = s.sweep;
        res.has_noise = true;
        res.noise_input_is_current = input_is_current;
        res.noise_vout_total = vout_rms;
        res.noise_iin_total = iin_rms;
        res.noise_ENVLP_text = out_ENVLP_txt + in_ENVLP_txt;
        res.noise_ENVLP_latex = out_ENVLP_tex + in_ENVLP_tex;
        for (int i = 0; i < npts; ++i) {
            res.noise_f_hz.push_back(freqs[i]);
            res.noise_vout.push_back(std::sqrt(vout[i]));
        }
        cr.transfer = res;
        cr.has_transfer = true;
    }

    // One-line summary = the output density expression (for the card list).
    {
        std::ostringstream tx;
        tx << "S_v(f) = ";
        for (size_t j = 0; j < groups.size(); ++j) {
            const Group& g = groups[j];
            std::string in_t, in_x;
            inner_expr(g, in_t, in_x);
            bool unity = g.h2.is_equal(ex(1));
            tx << (j ? " + " : "");
            if (unity) tx << "(" << in_t << ")";
            else tx << "H(s)^2*(" << in_t << ")";
        }
        cr.summary = tx.str();
    }
    // No headline expression: the report already leads with the symbolic
    // density, so the typeset tab does not repeat it (matches loop gain).
    cr.latex.clear();
    cr.latex_report = lrep;
    cr.text = cr.summary;
    cr.report = rep;
    cr.values.push_back({"Vout_n", eng::format_si(vout_rms, 8)});
    if (input_is_current)
        cr.values.push_back({"Iin_n", eng::format_si(iin_rms, 8)});
    else
        cr.values.push_back({"Vin_n", eng::format_si(vin_rms, 8)});
    (void)s_sym;
    return cr;
}

// ---------------------------------------------------------------------------
// dispatch
// ---------------------------------------------------------------------------
CardResult run_analysis(const Circuit& c, const AnalysisSpec& spec) {
    switch (spec.kind) {
        case AnalysisKind::TransferFunction: return analyze_tf(c, spec);
        case AnalysisKind::AC: return analyze_ac(c, spec);
        case AnalysisKind::DC: return analyze_dc(c, spec);
        case AnalysisKind::PSRR: return analyze_psrr(c, spec);
        case AnalysisKind::Differential: return analyze_differential(c, spec);
        case AnalysisKind::LoopGain: return analyze_loop_gain(c, spec);
        case AnalysisKind::ShortCircuitCurrent:
            return analyze_short_circuit(c, spec);
        case AnalysisKind::InputImpedance: return analyze_zin(c, spec);
        case AnalysisKind::OutputImpedance: return analyze_zout(c, spec);
        case AnalysisKind::Noise: return analyze_noise(c, spec);
    }
    throw std::runtime_error("unknown analysis kind");
}

} // namespace syms
