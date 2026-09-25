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
// Loop gain / return ratio (nullor-substitution / Rosenstark form).
//
// Reference element X is replaced by an ideal nullor to get the ideal
// closed-loop gain H_inf. With a nullor break the closed loop obeys
//     H(s) = H_inf * T/(1+T) + H_0/(1+T),
// so with no direct feedforward (H_0 = 0 -- true whenever the reference
// element is the only forward path through the loop) the return ratio is
//     T(s) = H / (H_inf - H).
// The direct-feedthrough term H_0 is not synthesised here; the report notes
// the assumption so the user can sanity-check the topology.
// ---------------------------------------------------------------------------
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
    if (ref->nodes.size() != 2 && ref->nodes.size() != 3) {
        cr.summary = "reference element must be a 2- or 3-terminal device";
        cr.report = cr.summary + "\n";
        return cr;
    }

    // Actual closed-loop gain with the element present.
    CardResult actual = analyze_tf(c, s);

    // Replace the reference element with a nullor (ideal infinite gain) to
    // obtain the ideal closed-loop transfer H_inf.
    Circuit ci = c;
    for (auto& cc : ci.comps) {
        if (cc.ref != s.probe_ref) continue;
        Kind orig = cc.kind;
        cc.kind = Kind::NULLOR;
        if (orig == Kind::NMOS || orig == Kind::PMOS || orig == Kind::NPN ||
            orig == Kind::PNP) {
            // gate/base = nullator input, drain/collector = norator output,
            // source/emitter = reference of the nullator
            cc.nodes = {cc.nodes[1], cc.nodes[2], cc.nodes[0]};
        } else if (orig == Kind::OPAMP || orig == Kind::FDOPAMP) {
            // in+, in-, out -- nullor across the input pair
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

    // T = H_inf/H - 1
    PruneOptions o = opts_of(s);
    ParamTable pt = actual.transfer.params;

    ex Hact = (actual.transfer.num_raw / actual.transfer.den_raw).normal();
    ex Hinf = (ideal.transfer.num_raw / ideal.transfer.den_raw).normal();
    ex T;
    bool ok = true;
    if (Hinf.is_zero()) {
        ok = false; // no forward path through the break: no loop
    } else if (Hact.is_zero()) {
        T = ex(0);
    } else {
        ex diff = (Hinf - Hact).normal();
        if (diff.is_zero()) {
            ok = false; // H == H_inf everywhere: no feedback to break
        } else {
            T = (Hact / diff).normal();
        }
    }

    Pruned Tp;
    if (ok && !T.is_zero()) {
        Tp = prune_low_entropy(T.numer(), T.denom(), pt, o);
    } else {
        Tp.text = ok ? "0" : "undefined (H == H_inf: nothing to break)";
    }
    if (Tp.text.empty()) Tp.text = "0";

    std::string rep = "Return-ratio / loop-gain analysis\n";
    rep += "reference element: " + s.probe_ref + "\n";
    rep += "----------------------------------------\n";
    rep += "Actual closed-loop gain:\n  H(s)     = " + actual.text + "\n";
    rep += "Ideal (nullor substitution):\n  H_inf(s) = " + ideal.text + "\n";
    rep += "  LaTeX: " + ideal.latex + "\n";
    rep += "\nReturn ratio  T(s) = H / (H_inf - H) :\n";
    rep += "  T(s) = " + Tp.text + "\n";
    rep += "  LaTeX: " + Tp.latex + "\n";
    rep += "\n";
    rep += "Assumes no direct feedthrough at the break (H_0 = 0), i.e. the\n";
    rep += "reference element is the only forward path through the loop.\n";
    rep += "Plot Bode / Nyquist / Nichols using the transfer function below\n";
    rep += "(the Bode tab shows T).\n";

    cr.text = "T(s) = " + Tp.text;
    cr.latex = Tp.latex;
    cr.summary = "H_inf = " + ideal.text + " ;  T = " + Tp.text;
    cr.report = rep;
    cr.has_transfer = true;
    if (ok) {
        AnalysisResult res;
        res.input_desc = "return ratio";
        res.output_desc = "T(" + s.probe_ref + ")";
        res.num_raw = T.numer();
        res.den_raw = T.denom();
        res.params = pt;
        res.opts = o;
        res.pruned = Tp;
        res.report = format_report(res);
        cr.transfer = res;
    } else {
        cr.transfer = actual.transfer;
    }
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
