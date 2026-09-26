#include "core/Analysis.h"
#include "core/Eng.h"
#include "core/LowEntropy.h"
#include "core/MNA.h"
#include "core/Par.h"
#include "core/Print.h"

#include <algorithm>
#include <cmath>
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
    // Impedance analyses: the denominator's DC value can be a symbolic sum,
    // and normalizing by it would push that sum into the numerator as nested
    // fractions. Transfer-like analyses keep normalization (it is what yields
    // (1+s*tau) factors).
    o.normalize = !(s.kind == AnalysisKind::InputImpedance ||
                    s.kind == AnalysisKind::OutputImpedance);
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
};

RawTF raw_tf(const Circuit& c, const std::string& input_ref,
             const std::string& output) {
    AnalysisRequest r;
    r.input_ref = input_ref;
    r.output = output;
    Solved sv = solve(c, r);
    RawTF t;
    t.num = sv.num;
    t.den = sv.den;
    t.params = std::move(sv.params);
    t.out_desc = sv.output_desc;
    return t;
}

CardResult make_transfer(const RawTF& t, const AnalysisSpec& s,
                         const std::string& title) {
    CardResult cr;
    cr.kind = s.kind;
    cr.title = title;
    cr.has_transfer = true;
    PruneOptions o = opts_of(s); // use_parallel is hardcoded on inside

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
// AC: small-signal node voltage / branch current (same machinery as the TF,
// but with the parasitic small-signal model enabled per device).
// ---------------------------------------------------------------------------
CardResult analyze_ac(const Circuit& c, const AnalysisSpec& s) {
    RawTF t = raw_tf(c, s.input_ref, s.output);
    CardResult cr = make_transfer(t, s, "AC (small-signal)");
    // AC also reports numeric gain at f0
    double w0 = 2.0 * M_PI * s.f0_hz;
    double mag = eval_mag_db((t.num / t.den), t.params, w0);
    double ph = eval_phase_deg((t.num / t.den), t.params, w0);
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%.3g dB @ %.4g Hz, phase %.3g deg", mag,
                  s.f0_hz, ph);
    cr.values.push_back({"at f0", buf});
    cr.summary = cr.transfer.output_desc + " = " + cr.text + "   (" + buf + ")";
    return cr;
}

// ---------------------------------------------------------------------------
// DC: every node voltage and branch current with s -> 0.
// ---------------------------------------------------------------------------
CardResult analyze_dc(const Circuit& c, const AnalysisSpec& s) {
    // DC uses the same MNA but evaluates at s = 0. Devices keep their DC
    // transconductance (gm); capacitors open, inductors short (handled by
    // taking the limit s -> 0 of the symbolic result).
    MnaSystem sys = build_mna(c, s.input_ref);
    ex s_sym = sys.params.get("s");

    CardResult cr;
    cr.kind = AnalysisKind::DC;
    cr.title = "DC operating point (symbolic)";

    // Prune one DC value: rank its terms by magnitude with the user estimates
    // and drop everything more than threshold_db below the dominant term.
    auto dc_value = [&](const ParamTable& pt, ex v) -> ex {
        if (!s.prune) return v;
        ex e = v.expand();
        if (!is_a<GiNaC::add>(e)) return v;
        double best = -1e300;
        std::vector<ex> terms;
        for (size_t i = 0; i < e.nops(); ++i) {
            terms.push_back(e.op(i));
            std::complex<double> z = eval_complex(e.op(i), pt, 0.0);
            double d = 20.0 * std::log10(std::hypot(z.real(), z.imag()));
            if (std::isfinite(d)) best = std::max(best, d);
        }
        ex acc = 0;
        for (const ex& term : terms) {
            std::complex<double> z = eval_complex(term, pt, 0.0);
            double d = 20.0 * std::log10(std::hypot(z.real(), z.imag()));
            if (std::isfinite(d) && d < best - s.threshold_db) continue;
            acc += term;
        }
        return acc.is_zero() ? v : acc;
    };

    // helper: DC value of V(node)
    (void)raw_tf(c, s.input_ref, "V(out)");

    std::vector<ex> latex_vals;
    std::vector<std::string> latex_names;

    std::vector<std::string> node_names;
    for (const auto& kv : sys.node_idx)
        if (kv.first != "0") node_names.push_back(kv.first);
    std::sort(node_names.begin(), node_names.end());

    for (const auto& nd : node_names) {
        try {
            RawTF t = raw_tf(c, s.input_ref, "V(" + nd + ")");
            ex v = (t.num / t.den).normal().subs(s_sym == 0).normal();
            ex vp = dc_value(t.params, v);
            cr.values.push_back({"V(" + nd + ")", pretty(vp)});
            latex_vals.push_back(vp);
            latex_names.push_back("V(" + nd + ")");
        } catch (const std::exception&) {
            // node not reachable; skip
        }
    }

    // branch currents: R, L (short), V sources -- the most useful ones
    for (const auto& cc : c.comps) {
        std::string spec;
        if (cc.kind == Kind::R || cc.kind == Kind::V || cc.kind == Kind::L)
            spec = "I(" + cc.ref + ")";
        else
            continue;
        try {
            RawTF t = raw_tf(c, s.input_ref, spec);
            ex v = (t.num / t.den).normal().subs(s_sym == 0).normal();
            ex vp = dc_value(t.params, v);
            cr.values.push_back({spec, pretty(vp)});
            latex_vals.push_back(vp);
            latex_names.push_back(spec);
        } catch (const std::exception&) {
            // branch current not available; skip
        }
    }

    std::string summary;
    for (const auto& kv : cr.values) {
        if (!summary.empty()) summary += ",   ";
        summary += kv.first + " = " + kv.second;
    }
    cr.summary = summary.empty() ? "(no nodes)" : summary;

    // LaTeX: an aligned block of every DC value
    {
        std::ostringstream lx;
        lx << "\\begin{aligned}";
        for (size_t i = 0; i < latex_vals.size(); ++i) {
            lx << (i ? "\\\\" : "");
            // V(x) -> \mathrm{V}(x), I(x) -> \mathrm{I}(x)
            lx << "\\mathrm{" << latex_names[i].substr(0, 1) << "}"
               << "(" << latex_names[i].substr(2, latex_names[i].size() - 3)
               << ") &= " << to_latex(latex_vals[i]);
        }
        lx << "\\end{aligned}";
        cr.latex = lx.str();
    }

    std::string rep = "DC analysis -- symbolic node voltages and currents\n";
    rep += "input source: " + s.input_ref + "  (s -> 0)\n";
    rep += "f0 is not used for DC; pruning threshold = " +
           std::to_string(int(s.threshold_db)) + " dB\n";
    rep += "----------------------------------------\n";
    for (const auto& kv : cr.values)
        rep += "  " + kv.first + " = " + kv.second + "\n";
    rep += "\nLaTeX (s = 0):\n  " + cr.latex + "\n";
    cr.report = rep;
    cr.text = cr.summary;
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

    // Phase margin: at the unity-gain crossover, PM = 180 + angle(T).
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

    // Stability (item 7): keep it up with the other headline results, before
    // the gain/bandwidth block.
    std::string stab;
    if (pm >= 0.0) {
        char b[160];
        std::snprintf(b, sizeof(b),
                      "  unity-gain at %s, phase margin = %.1f deg\n",
                      eng::format_hz(ugf).c_str(), pm);
        stab = b;
    } else {
        stab = "  T never crosses 0 dB within the sweep (no unity-gain "
               "frequency)\n";
    }

    // Closed-loop gain assembled from the asymptotic-gain formula, so the
    // printed expression is exactly what the formula evaluates to:
    //   H = H_inf*T/(1+T) + H_0/(1+T) = (H_inf*T + H_0)/(1+T).
    ex Hinf = (ideal.transfer.num_raw / ideal.transfer.den_raw).normal();
    ex Hcl_formula = ((Hinf * T + H0) / (1 + T)).normal();
    Pruned Hcl_p = prune_low_entropy(Hcl_formula.numer(), Hcl_formula.denom(),
                                     pt, o);

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
    rep += "\nStability:\n" + stab;
    // The return ratio's own gain/bandwidth and poles/zeros, with the leading
    // "H(s) = ..." line stripped (T is already shown above).
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
    lrep += "\nStability:\n";
    if (pm >= 0.0) {
        char b[192];
        std::snprintf(b, sizeof(b),
                      "\\mathrm{unity-gain\\ at}\\ \\mathrm{%s}"
                      ",\\quad \\mathrm{phase\\ margin} = %.1f\\degree\n",
                      eng::format_hz(ugf).c_str(), pm);
        lrep += b;
    } else {
        lrep += "\\mathrm{T\\ never\\ crosses\\ 0\\ dB\\ within\\ the\\ sweep}\n";
    }
    lrep += "\n";
    lrep += format_report_latex(res);

    cr.text = "T(s) = " + Tp.text;
    cr.latex = "T(s) = " + latex_rhs(Tp.latex);
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
// Input impedance: Zin = V(in)/I(in) with the input source at 1 V.
// ---------------------------------------------------------------------------
CardResult analyze_zin(const Circuit& c, const AnalysisSpec& s) {
    // I(Vsrc) with a 1 V drive gives Y = I/V = numI/det, so
    //   Zin = V/I = det/numI = den_H / num_H.
    AnalysisSpec s2 = s;
    s2.output = "I(" + s.input_ref + ")";
    CardResult isrc = analyze_tf(c, s2);

    PruneOptions o = opts_of(s);
    ParamTable pt = isrc.transfer.params;
    // reduce den/num into one rational before pruning
    ex Z = (isrc.transfer.den_raw / isrc.transfer.num_raw).normal();
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
    rep += "  LaTeX:  " + cr.latex + "\n";
    cr.report = rep;
    cr.has_transfer = true;
    {
        AnalysisResult res;
        res.input_desc = "I(" + s.input_ref + ")";
        res.output_desc = "Zin";
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

    // zero the input source, add a 1 A test current into the node
    Circuit cs = c;
    for (auto& cc : cs.comps)
        if (cc.ref == s.input_ref) cc.value_text = "0";
    Component it;
    it.kind = Kind::I;
    it.ref = "__ITEST__";
    it.nodes = {node, "0"};
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
// Noise: input- and output-referred noise from resistor and device noise
// sources (thermal 4kT/R for resistors, 4kT*(2/3)*gm channel noise for
// MOSFETs; a BJT's base shot noise 2q*Ib and collector shot noise 2q*Ic are
// included via gm). Each source is injected at its physical terminals and
// propagated to the selected output with a 1 A test current.
// ---------------------------------------------------------------------------
CardResult analyze_noise(const Circuit& c, const AnalysisSpec& s) {
    CardResult cr;
    cr.kind = AnalysisKind::Noise;
    cr.title = "Noise";

    const double kT = 1.380649e-23 * 300.15; // kT at 300.15 K
    const double q = 1.602176634e-19;        // electron charge

    // Sum the output noise contributions: for each noisy element add a
    // current/voltage source, propagate it to the output, and add powers.
    RawTF sig = raw_tf(c, s.input_ref, s.output);
    ex s_sym = sig.params.get("s");

    // signal gain magnitude at f0 (for input referral)
    double w0 = 2.0 * M_PI * s.f0_hz;
    ex Hsig = (sig.num / sig.den).normal();
    double gain = std::abs(eval_complex(Hsig, sig.params, w0));
    if (!(gain > 0.0)) gain = 1e-30;

    double vout2 = 0.0;
    int nsrc = 0;
    std::string detail;
    std::vector<std::pair<std::string, double>> per_source;

    // helper: transfer from a 1A current injected at `node` to V(out)
    auto z_from_current = [&](const std::string& node, const std::string& gnd)
        -> ex {
        Circuit ct = c;
        Component it;
        it.kind = Kind::I;
        it.ref = "__INOISE__";
        it.nodes = {node, gnd};
        it.value_text = "1";
        ct.comps.push_back(it);
        AnalysisRequest r;
        r.input_ref = "__INOISE__";
        r.output = s.output;
        Solved sv = solve(ct, r);
        return (sv.num / sv.den).normal();
    };

    // A 1 A current between the two terminals of a noisy element is the
    // natural dual of its thermal / shot noise current. (For a MOSFET the
    // channel noise sits between drain and source.)
    struct NoiseSrc {
        std::string ref;
        std::string mech;
        double i2;        // A^2/Hz
        std::string node_a, node_b;
    };
    std::vector<NoiseSrc> noise;
    for (const auto& cc : c.comps) {
        if (cc.kind == Kind::R) {
            double R = cc.estimate();
            if (R > 0.0)
                noise.push_back({cc.ref, "4kT/R", 4.0 * kT / R, cc.nodes[0],
                                 cc.nodes[1]});
        } else if (cc.kind == Kind::NMOS || cc.kind == Kind::PMOS) {
            double gm = cc.param_estimate("gm");
            if (gm > 0.0)
                noise.push_back({cc.ref, "4kT*(2/3)*gm",
                                 4.0 * kT * (2.0 / 3.0) * gm, cc.nodes[0],
                                 cc.nodes[2]});
        } else if (cc.kind == Kind::NPN || cc.kind == Kind::PNP) {
            double gm = cc.param_estimate("gm");
            double beta = 100.0; // typical; base shot noise is 2q*Ic/beta
            double ic = gm * kT / q; // Ic = gm*VT
            double ib = ic / beta;
            double i2 = 2.0 * q * (ic + ib);
            if (i2 > 0.0)
                noise.push_back({cc.ref, "2q*(Ic+Ib)",
                                 i2, cc.nodes[0], cc.nodes[2]});
        } else if (cc.kind == Kind::D) {
            double gm = cc.param_estimate("gm");
            double id = gm * kT / q;
            double i2 = 2.0 * q * id + 4.0 * kT * gm * (2.0 / 3.0);
            if (i2 > 0.0)
                noise.push_back({cc.ref, "2q*Id + 4kT*gm",
                                 i2, cc.nodes[0], cc.nodes[1]});
        }
    }

    for (const auto& n : noise) {
        ex zt = z_from_current(n.node_a, n.node_b);
        double z2 = std::norm(eval_complex(zt, sig.params, w0));
        double contrib = n.i2 * z2;
        vout2 += contrib;
        per_source.push_back({n.ref, contrib});
        ++nsrc;
        char buf[192];
        std::snprintf(buf, sizeof(buf),
                      "    %-8s %-16s i_n^2=%.3g A^2/Hz   |Zout|^2=%.3g   "
                      "%.3g V^2/Hz",
                      n.ref.c_str(), n.mech.c_str(), n.i2, z2, contrib);
        detail += buf;
        detail += "\n";
    }

    if (nsrc == 0) {
        cr.summary = "no noise sources (no resistors or devices found)";
        cr.report = cr.summary + "\n";
        return cr;
    }

    double vout_rms = std::sqrt(vout2);
    double vin_rms = vout_rms / gain;
    auto db20 = [](double x) {
        return x > 0 ? 20.0 * std::log10(x) : -1e300;
    };
    auto db10 = [](double x) {
        return x > 0 ? 10.0 * std::log10(x) : -1e300;
    };

    std::string rep = "Noise analysis @ " + eng::format_eng(s.f0_hz, 3) +
                      " Hz   (" + std::to_string(nsrc) +
                      " sources: thermal + channel/shot)\n";
    rep += "----------------------------------------\n";
    rep += detail;
    rep += "  signal gain |H(f0)|: " + eng::format_eng(gain, 3) + "  (" +
           eng::format_db(db20(gain), 2) + ")\n";
    rep += "\n  Integrated output noise density:\n";
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "    output-referred: %.4g V/sqrt(Hz)  (%.2f dBV, %.2f "
                  "dBV^2/Hz)\n"
                  "    input-referred:  %.4g V/sqrt(Hz)  (%.2f dBV)\n",
                  vout_rms, db20(vout_rms), db10(vout2), vin_rms,
                  db20(vin_rms));
    rep += buf;
    rep += "\n  Per-source contribution to the output noise power:\n";
    // sort descending by contribution
    std::sort(per_source.begin(), per_source.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    double total = vout2 > 0 ? vout2 : 1e-300;
    for (const auto& ps : per_source) {
        char sb[160];
        std::snprintf(sb, sizeof(sb), "    %-8s %8.2f %%   %.3g V^2/Hz\n",
                      ps.first.c_str(), 100.0 * ps.second / total, ps.second);
        rep += sb;
    }

    cr.summary = buf;
    cr.text = buf;
    cr.report = rep;
    cr.values.push_back({"Vout_n", eng::format_si(vout_rms, 8)});
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
