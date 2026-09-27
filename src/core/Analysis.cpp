#include "core/Analysis.h"
#include "core/Eng.h"
#include "core/LowEntropy.h"
#include "core/MNA.h"
#include "core/Par.h"
#include "core/Print.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <stdexcept>

namespace syms {

using GiNaC::ex;
using GiNaC::is_a;

namespace {

PruneOptions opts_of(const AnalysisSpec& s) {
    PruneOptions o;
    o.f0_hz = s.f0_hz;
    o.threshold_db = s.threshold_db;
    o.pole_zero_threshold_db = s.pole_zero_threshold_db;
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
    r.threshold_db = s.threshold_db;
    r.pole_zero_threshold_db = s.pole_zero_threshold_db;
    r.global_ref = s.global_ref;
    r.prune = s.prune;
    r.use_parallel = s.use_parallel;
    r.approx_factor = s.approx_factor;
    r.normalize = s.normalize;
    r.sweep = s.sweep;
    return r;
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
};

RawTF raw_tf(const Circuit& c, const std::string& input_ref,
             const std::string& output,
             const std::set<std::string>& used_nodes = {}) {
    AnalysisRequest r;
    r.input_ref = input_ref;
    r.output = output;
    r.used_nodes = used_nodes;
    Solved sv = solve(c, r);
    RawTF t;
    t.num = sv.num;
    t.den = sv.den;
    t.params = std::move(sv.params);
    t.out_desc = sv.output_desc;
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
    res.pruned = prune_low_entropy(t.num, t.den, res.params, o);
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
    RawTF t = raw_tf(c, s.input_ref, s.output);
    CardResult cr = make_transfer(t, s, "Transfer function");
    return cr;
}

// ---------------------------------------------------------------------------
// AC: small-signal response to the *AC values* of every independent source,
// superposed. There is no single "input": each source contributes its own
// transfer to the chosen output scaled by its AC value, and the result is the
// linear combination sum_i (AC_i * H_i(s)). The output is a node voltage or a
// branch current, not a ratio.
// ---------------------------------------------------------------------------
CardResult analyze_ac(const Circuit& c, const AnalysisSpec& s) {
    CardResult cr;
    cr.kind = AnalysisKind::AC;
    cr.title = "AC (small-signal)";

    // Collect every independent voltage/current source and its AC value. VDD
    // is a supply rail, not a signal source: it is AC ground and never
    // contributes to the AC superposition.
    struct Src { std::string ref; double ac; };
    std::vector<Src> srcs;
    for (const auto& cc : c.comps) {
        if (cc.kind != Kind::V && cc.kind != Kind::I) continue;
        double ac = 1.0;
        if (!eng::parse_value(cc.ac_text, ac)) ac = 1.0;
        srcs.push_back({cc.ref, ac});
    }
    if (srcs.empty()) {
        cr.summary = "AC: no independent sources to excite";
        cr.report = cr.summary + "\n";
        return cr;
    }

    // H_i(s) = output per unit of source i; sum with the AC weights.
    ex num = 0, den = 1;
    ParamTable params;
    std::string out_desc;
    double w0 = 2.0 * M_PI * s.f0_hz;
    std::vector<std::pair<std::string, double>> contrib; // ref -> AC_i (dB) at f0
    for (const auto& src : srcs) {
        AnalysisRequest r;
        r.input_ref = src.ref;
        r.output = s.output;
        if (s.output.size() > 3 && s.output.front() == 'V' &&
            s.output.back() == ')')
            r.used_nodes.insert(s.output.substr(2, s.output.size() - 3));
        Solved sv = solve(c, r);
        if (params.syms.empty()) params = sv.params;
        out_desc = sv.output_desc;
        // scale this source's response by its AC value
        ex term = (sv.num / sv.den).normal() * ex(src.ac);
        num = (num + term).normal();
        den = sv.den;
        double m = eval_mag_db((sv.num / sv.den).normal(), sv.params, w0);
        contrib.push_back({src.ref, m + 20.0 * std::log10(std::abs(src.ac))});
    }

    // Low-entropy prune of the combined response. Normalize the denominator so
    // the result factors as gain * (1 + s*tau)...; for a response the scale is
    // simply folded into the reported gain K.
    PruneOptions o = opts_of(s);
    Pruned p = prune_low_entropy(num.numer(), num.denom(), params, o);

    cr.has_transfer = true;
    {
        AnalysisResult res;
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
    cr.text = p.text;
    cr.latex = p.latex;
    double mag = eval_mag_db((num / den).normal(), params, w0);
    double ph = eval_phase_deg((num / den).normal(), params, w0);
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%.3g dB @ %.4g Hz, phase %.3g deg", mag,
                  s.f0_hz, ph);
    cr.values.push_back({"at f0", buf});
    for (const auto& cc : contrib) {
        char b2[64];
        std::snprintf(b2, sizeof(b2), "%.3g dB", cc.second);
        cr.values.push_back({cc.first, b2});
    }

    std::string rep = "AC analysis -- superposition of source AC values\n";
    rep += "output: " + out_desc + "\n";
    rep += "----------------------------------------\n";
    rep += "  " + out_desc + " = " + p.text + "\n";
    rep += "  at f0: " + std::string(buf) + "\n";
    rep += "Per-source contributions (AC value x H_i) at f0:\n";
    for (size_t i = 0; i < contrib.size(); ++i)
        rep += "  " + contrib[i].first + ": " + cr.values[1 + i].second + "\n";
    cr.report = rep;
    cr.summary = out_desc + " = " + p.text + "   (" + buf + ")";
    {
        std::ostringstream lr;
        lr << "AC (superposition of source AC values):\n";
        lr << out_desc << " = " << latex_rhs(p.latex) << "\n";
        cr.latex_report = lr.str();
    }
    return cr;
}

// ---------------------------------------------------------------------------
// DC: large-signal operating point. Every source is stamped with its DC value;
// every MOSFET is a saturation square-law device (Id at the operating point,
// Vgs = 2*Id/gm + Vth from the DC tech settings, channel-length modulation via
// ro). The system is solved for every node and each device's Id, then each
// device is checked for saturation: its drain-source voltage magnitude must
// exceed its Vdsat = 2*|Id|/gm. If any device is pushed into triode, the
// analysis aborts and names it.
// ---------------------------------------------------------------------------
CardResult analyze_dc(const Circuit& c, const AnalysisSpec& s) {
    CardResult cr;
    cr.kind = AnalysisKind::DC;
    cr.title = "DC operating point (large-signal)";

    DcSolution dc;
    try {
        dc = solve_dc(c, s.tech);
    } catch (const std::exception& e) {
        cr.summary = std::string("DC solve failed: ") + e.what();
        cr.report = cr.summary + "\n";
        return cr;
    }

    ParamTable& pt = dc.params;

    // Put one operating-point value into a design-readable form:
    //   1. combine over a common denominator (exact rationals, so 1/2 stays
    //      1/2, not 0.5);
    //   2. recover parallel structure: R1*R2/(R1+R2) -> R1||R2 and
    //      R1*(R2+R3)/(R1+R2+R3) -> R1||(R2+R3);
    //   3. otherwise keep the denominator a clean factored block.
    // We keep a finite amplifier gain's "+1": V(out) then reads as the exact
    // KVL sum, not the ideal A -> infinity limit. (A "to_parallel" result is
    // already tidy, so we return it without a normal() that would re-expand.)
    auto dc_value = [&](ex v) -> ex {
        ex out = v.normal();
        if (!s.prune) return out;
        // Factor numerator AND denominator first so the additive factors are
        // visible to the parallel recovery: a numerator R1*I1*(R2+R3)*A_U1 and
        // a denominator (A_U1+1)*(R1+R2+R3) expose R1 + (R2+R3), giving
        // V(out) = I1*(R1||(R2+R3))*A_U1/(A_U1+1).
        ex num = out.numer().expand();
        ex den = out.denom().expand();
        if (!is_a<GiNaC::numeric>(num)) num = GiNaC::factor(num);
        if (!is_a<GiNaC::numeric>(den)) den = GiNaC::factor(den);
        ex ratio = (num / den).normal();
        ex parred = to_parallel(ratio);
        if (!parred.is_equal(ratio)) return parred;
        return num / den;
    };

    // ---- saturation check: Vds must exceed Vdsat = 2*Id/gm for each device --
    // Signed per device polarity: NMOS is on when Vov = Vgs - Vth > 0 and is
    // saturated when Vds >= Vov; PMOS is on when Vov < 0 and saturated when
    // Vds <= Vov. (Vov = 2*Id/gm by the model's construction.)
    for (const auto& ref : dc.mosfets) {
        const Component* mc = c.find(ref);
        const bool pmos = mc && mc->kind == Kind::PMOS;
        double vds_n = eval_complex(dc.vds[ref], pt, 0.0).real();
        double vov_n = eval_complex(dc.vov[ref], pt, 0.0).real();
        if (!std::isfinite(vds_n) || !std::isfinite(vov_n)) continue;
        bool saturated = pmos ? (vds_n <= vov_n) : (vds_n >= vov_n);
        if (!saturated) {
            ex vds = dc_value(dc.vds[ref]);
            ex vov = dc_value(dc.vov[ref]);
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                          "device %s is not in saturation: Vds = %s, Vdsat = %s "
                          "(pushed into triode). DC analysis stopped.",
                          ref.c_str(), pretty(vds).c_str(), pretty(vov).c_str());
            cr.summary = buf;
            cr.report = std::string("DC (large-signal) -- ABORTED\n") +
                        "----------------------------------------\n" + buf + "\n";
            return cr;
        }
    }

    // ---- report every node voltage, plus each device's operating point ------
    std::vector<ex> latex_vals;
    std::vector<std::string> latex_names;

    std::vector<std::string> node_names;
    for (const auto& kv : dc.node_v) node_names.push_back(kv.first);
    std::sort(node_names.begin(), node_names.end());
    for (const auto& nd : node_names) {
        ex vp = dc_value(dc.node_v[nd]);
        cr.values.push_back({"V(" + nd + ")", pretty(vp)});
        latex_vals.push_back(vp);
        latex_names.push_back("V(" + nd + ")");
    }
    for (const auto& ref : dc.mosfets) {
        ex id = dc_value(dc.id[ref]);
        cr.values.push_back({"I(" + ref + ")", pretty(id)});
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
    rep += "every source uses its DC value; MOSFETs assumed in saturation\n";
    rep += "pruning threshold = " + std::to_string(int(s.threshold_db)) + " dB\n";
    rep += "----------------------------------------\n";
    for (const auto& kv : cr.values)
        rep += "  " + kv.first + " = " + kv.second + "\n";
    if (!dc.mosfets.empty()) {
        rep += "\nDevice operating points:\n";
        for (const auto& ref : dc.mosfets) {
            rep += "  " + ref + ": Vgs = " + pretty(dc_value(dc.vgs[ref])) +
                   ", Vds = " + pretty(dc_value(dc.vds[ref])) +
                   ", Vdsat = " + pretty(dc_value(dc.vov[ref])) +
                   ", Id = " + pretty(dc_value(dc.id[ref])) + "\n";
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
    rep += "  LaTeX: " + psr_p.latex + "\n";
    rep += "PSRR = H(Vin -> out) / H(VDD -> out) = " + psrr_p.text + "\n";
    rep += "  LaTeX: " + psrr_p.latex + "\n";
    (void)s_sym;

    cr.text = "PSR = " + psr_p.text + " ;  PSRR = " + psrr_p.text;
    cr.latex = psrr_p.latex;
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
            if (nd.size() >= 3) return {nd[0], nd[1], nd[2], "0", true};
            break;
        case Kind::AMP:
            if (nd.size() >= 2) return {nd[0], nd[1], nd[1], "0", true};
            break;
        case Kind::NMOS:
        case Kind::PMOS:
        case Kind::NPN:
        case Kind::PNP:
            if (nd.size() >= 3) return {nd[1], nd[2], nd[0], "0", true};
            break;
        default:
            break;
    }
    return {};
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

    // Actual closed-loop gain with the element present.
    CardResult actual = analyze_tf(c, s);
    ParamTable pt = actual.transfer.params;
    ex s_ex = pt.get("s");

    // ---- 1. H_inf: replace the amplifier with a nullor (virtual short) ----
    Circuit ci = c;
    for (auto& cc : ci.comps) {
        if (cc.ref != s.probe_ref) continue;
        Kind orig = cc.kind;
        cc.kind = Kind::NULLOR;
        if (orig == Kind::NMOS || orig == Kind::PMOS || orig == Kind::NPN ||
            orig == Kind::PNP) {
            cc.nodes = {cc.nodes[1], cc.nodes[2], cc.nodes[0]};
        } else if (orig == Kind::OPAMP || orig == Kind::FDOPAMP) {
            cc.nodes = {cc.nodes[0], cc.nodes[1],
                        cc.nodes.size() > 2 ? cc.nodes[2] : cc.nodes[0]};
        } else {
            cc.nodes = {cc.nodes[0], cc.nodes[1], cc.nodes[0]};
        }
    }
    CardResult ideal;
    try {
        ideal = analyze_tf(ci, s);
    } catch (const std::exception& e) {
        cr.summary = std::string("nullor substitution failed: ") + e.what();
        cr.report = cr.summary + "\n";
        return cr;
    }

    // ---- 2. beta: drive the output with a test voltage, control opened ----
    Circuit ct = c;
    for (size_t i = 0; i < ct.comps.size(); ++i) {
        if (ct.comps[i].ref == s.probe_ref) {
            ct.comps.erase(ct.comps.begin() + i);
            break;
        }
    }
    {
        Component vt;
        vt.kind = Kind::V;
        vt.ref = "__TEST__";
        vt.nodes = {ports.op, ports.on};
        vt.value_text = "1";
        ct.comps.push_back(vt);
    }
    auto test_v = [&](const std::string& node) -> ex {
        if (node == "0" || node == "GND") return ex(0);
        RawTF t = raw_tf(ct, "__TEST__", "V(" + node + ")");
        return (t.num / t.den).normal();
    };
    ex beta;
    try {
        beta = (test_v(ports.cp) - test_v(ports.cn)).normal();
    } catch (const std::exception& e) {
        cr.summary = std::string("return-ratio test failed: ") + e.what();
        cr.report = cr.summary + "\n";
        return cr;
    }

    // A(s) = A / (1 + s*A/GBW)  (GBW_<ref> is the unity-gain angular frequency
    // in rad/s -- the user's Hz value converted once here); T(s) = -A(s)*beta.
    ex A_sym = pt.get("A_" + s.probe_ref);
    pt.set("A_" + s.probe_ref, ref->estimate(), UnitClass::Plain);
    ex gbw_sym = pt.get("GBW_" + s.probe_ref);
    double gbw_num = 2.0 * M_PI * ref->param_estimate("GBW"); // rad/s
    bool gbw_on = ref->param_enabled("GBW") && gbw_num > 0.0;
    pt.set("GBW_" + s.probe_ref, gbw_num, UnitClass::Plain);
    ex T = -A_sym * beta;
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
        double prev_f = f0, prev_m = mag_db_at(res, 2.0 * M_PI * f0);
        for (int i = 1; i <= N; ++i) {
            double t = double(i) / N;
            double f = f0 * std::pow(f1 / f0, t);
            double m = mag_db_at(res, 2.0 * M_PI * f);
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
    std::string pm_sym_txt, pm_sym_tex;
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
        pm_sym_txt = "PM = " + t + "   (omega_ug = " + wugb +
                     ", one atan per pole/zero)";
        pm_sym_tex = "\\mathrm{PM} = " + l +
                     ",\\quad \\omega_{ug} = \\mathrm{" + wugb + "}";
    }

    // H_0: the forward gain with the reference amplifier's gain set to zero.
    // A zero-gain voltage amplifier forces v(out) = 0, i.e. its output is a
    // virtual ground: model it as a 0 V source there (for a fully differential
    // amp, a 0 V source between out+ and out-). The surrounding network then
    // provides whatever feedforward the output sees with the amplifier off.
    ex H0 = 0;
    {
        Circuit c0 = c;
        bool ok = false;
        for (size_t i = 0; i < c0.comps.size(); ++i) {
            Component& cc = c0.comps[i];
            if (cc.ref != s.probe_ref) continue;
            std::string o1, o2;
            if (cc.kind == Kind::OPAMP || cc.kind == Kind::AMP) {
                o1 = cc.nodes.size() > 2 ? cc.nodes[2] : "0";
                o2 = "0";
            } else if (cc.kind == Kind::FDOPAMP) {
                o1 = cc.nodes.size() > 2 ? cc.nodes[2] : "0";
                o2 = cc.nodes.size() > 3 ? cc.nodes[3] : "0";
            } else {
                break;
            }
            Component off;
            off.kind = Kind::V;
            off.ref = "__AOFF__";
            off.nodes = {o1, o2};
            off.value_text = "0";
            cc = off;
            ok = true;
            break;
        }
        if (ok) {
            try {
                CardResult direct = analyze_tf(c0, s);
                H0 = (direct.transfer.num_raw / direct.transfer.den_raw)
                         .normal();
            } catch (const std::exception&) {
                H0 = 0;
            }
        }
    }

    // Phase margin is reported inside the gain/bandwidth block (there is no
    // separate "Stability" section). Its symbolic form is the arctangent sum
    // derived above.
    std::string pm_sym = pm_sym_txt, pm_sym_latex = pm_sym_tex;

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
    ex Hcl_formula = ((Hinf * Tle + H0) / (1 + Tle)).normal();
    // Factor each s-coefficient and recover any parallel atoms before
    // factoring, so a residual R1*R2/(R1+R2) collapses back to (R1||R2).
    ex Hcl_ratio = (Hcl_formula.numer() / Hcl_formula.denom()).normal();
    Pruned Hcl_p = prune_low_entropy(Hcl_ratio.numer(), Hcl_ratio.denom(), pt,
                                     o);

    // Publish the phase margin on the return-ratio result so the gain/bandwidth
    // metrics block can print it alongside the other characteristics.
    if (pm >= 0.0) {
        res.has_pm = true;
        res.ugf_hz = eng::format_hz(ugf);
        res.pm_deg = pm;
        res.pm_sym = pm_sym;
        res.pm_sym_latex = pm_sym_latex;
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
    cs.comps.push_back(vs);

    // current through the 0 V source = current into the node
    AnalysisSpec s2 = s;
    s2.output = "I(__SHORT__)";
    CardResult cr = analyze_tf(cs, s2);
    cr.kind = AnalysisKind::ShortCircuitCurrent;
    cr.title = "Short-circuit current at " + node;
    cr.summary = "Isc(" + node + ") = " + cr.text;
    cr.text = "Isc(" + node + ") = " + cr.text;
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
    // Find the input node from the configured source.
    const Component* in = c.find(s.input_ref);
    std::string node;
    if (in && !in->nodes.empty()) node = in->nodes[0];

    // Zero every independent source, inject 1 A into the input node, and read
    // the node voltage: Zin = V(node)/1A. With the SPICE convention
    // ({n+,n-}: current flows n+ -> n- through the source), a source that
    // pushes 1 A *into* `node` has its n- terminal there, so order it {0, node}.
    Circuit cs = c;
    for (auto& cc : cs.comps)
        if (is_independent_source(cc.kind)) cc.value_text = "0";
    Component it;
    it.kind = Kind::I;
    it.ref = "__ITEST__";
    it.nodes = {"0", node.empty() ? std::string("0") : node};
    it.value_text = "1";
    cs.comps.push_back(it);

    AnalysisSpec s2 = s;
    s2.input_ref = "__ITEST__";
    s2.output = "V(" + node + ")";
    CardResult isrc = analyze_tf(cs, s2);

    PruneOptions o = opts_of(s);
    ParamTable pt = isrc.transfer.params;
    // Zin = V/I with a 1 A drive is the node voltage itself.
    ex Z = (isrc.transfer.num_raw / isrc.transfer.den_raw).normal();
    Pruned p = prune_low_entropy(Z.numer(), Z.denom(), pt, o);

    CardResult cr;
    cr.kind = AnalysisKind::InputImpedance;
    cr.title = "Input impedance";
    cr.text = "Zin = " + p.text;
    std::string zl = p.latex;
    if (zl.rfind("H(s) = ", 0) == 0) zl = zl.substr(7);
    cr.latex = "Z_{in}(s) = " + zl;
    cr.summary = cr.text;
    std::string rep = "Input impedance seen by " + s.input_ref + "\n";
    rep += "----------------------------------------\n";
    rep += "  Zin(s) = " + p.text + "\n";
    cr.report = rep;
    // LaTeX report mirrors the text report.
    cr.latex_report = "Input impedance:\nZ_{in}(s) = " + zl + "\n";
    cr.has_transfer = true;
    {
        AnalysisResult res;
        res.input_desc = "I(test)";
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

    // zero the input source, add a 1 A test current *into* the node. With the
    // SPICE convention ({n+,n-}: current flows n+ -> n- through the source),
    // a source pushing 1 A into `node` has its n- terminal there: {0, node}.
    Circuit cs = c;
    for (auto& cc : cs.comps)
        if (cc.ref == s.input_ref) cc.value_text = "0";
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
                ex Rs = pt.get(cc.ref); // the resistor's own symbol
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
                ex gms = pt.get("gm_" + cc.ref);
                // thermal: 4kT*(2/3)*gm; flicker coefficient: (thermal)*fcn,
                // with fcn the 1/f corner frequency kept symbolic.
                ex i_th = 4 * kT_sym * (ex(2) / 3) * gms;
                ex fcn_sym = pt.get("fcn_" + cc.ref);
                pt.set("fcn_" + cc.ref, fcn, UnitClass::Plain);
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
            double gm = cc.param_estimate("gm");
            double id = gm * kT / q;
            double i2 = 2.0 * q * id + 4.0 * kT * gm * (2.0 / 3.0);
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
            const char* sym_th = d.voltage ? "e_{n,th}^2" : "i_{n,th}^2";
            const char* sym_fl = d.voltage ? "e_{n,1/f}^2" : "i_{n,1/f}^2";
            const char* unit = d.voltage ? "V" : "A";
            t << "    " << d.ref << " (thermal): " << sym << " = " << pretty(d.th)
              << " " << unit << "^2/Hz\n";
            x << "\\mathrm{" << d.ref << "}:\\quad " << sym_th << " = "
              << to_latex(d.th) << "\\ " << unit << "^2/Hz\n";
            if (!d.fl.is_zero()) {
                t << "    " << d.ref << " (flicker): " << sym << " = "
                  << pretty(d.fl) << "/f " << unit << "^2/Hz\n";
                x << "\\mathrm{" << d.ref << "}:\\quad " << sym_fl
                  << " = \\frac{" << to_latex(d.fl) << "}{f}\\ " << unit
                  << "^2/Hz\n";
            }
        }
        dens_txt = t.str();
        dens_tex = x.str();
    }
    // Symbolic OUTPUT density: S_v(f) = sum_j |H_j|^2 * (i_j,th + i_j,fl/f).
    std::string out_sym_txt, out_sym_tex;
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
        out_sym_txt = t.str();
        out_sym_tex = x.str();
    }
    // Symbolic INPUT density: S_i(f) = S_v(f)/|H|^2, so the device/resistor
    // currents drop their H(s)^2 and the amp's en becomes en^2/H(s)^2.
    std::string in_sym_txt, in_sym_tex;
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
        in_sym_txt = t.str();
        in_sym_tex = x.str();
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
        t << "  Density S_v(f) [V^2/Hz]:\n" << out_sym_txt;
        x << "\\mathrm{Density}\\ S_{v}(f):\n" << out_sym_tex;
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
        t << "  Density S_i(f) [" << in_unit << "^2/Hz]:\n" << in_sym_txt;
        x << "\\mathrm{Density}\\ S_{i}(f):\n" << in_sym_tex;
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
        res.noise_sym_text = out_sym_txt + in_sym_txt;
        res.noise_sym_latex = out_sym_tex + in_sym_tex;
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
