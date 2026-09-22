#include "core/Analysis.h"
#include "core/Eng.h"
#include "core/LowEntropy.h"
#include "core/MNA.h"
#include "core/Par.h"
#include "core/Print.h"

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
    o.global_ref = s.global_ref;
    o.prune = s.prune;
    o.use_parallel = s.use_parallel;
    return o;
}

AnalysisRequest req_of(const AnalysisSpec& s, const std::string& in,
                       const std::string& out) {
    AnalysisRequest r;
    r.input_ref = in;
    r.output = out;
    r.f0_hz = s.f0_hz;
    r.threshold_db = s.threshold_db;
    r.global_ref = s.global_ref;
    r.prune = s.prune;
    r.use_parallel = s.use_parallel;
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
                         const std::string& title, bool use_parallel) {
    CardResult cr;
    cr.kind = s.kind;
    cr.title = title;
    cr.has_transfer = true;
    PruneOptions o = opts_of(s);
    o.use_parallel = use_parallel;

    AnalysisRequest req = req_of(s, "V1", "V(out)");
    // Rebuild an AnalysisResult so the GUI (Bode etc.) can use it directly.
    AnalysisResult res;
    res.input_desc = "source";
    res.output_desc = t.out_desc;
    res.num_raw = t.num;
    res.den_raw = t.den;
    res.params = t.params;
    res.opts = o;
    res.pruned = prune_low_entropy(t.num, t.den, res.params, o);
    res.report = format_report(res);

    cr.transfer = res;
    cr.text = res.pruned.text;
    cr.latex = res.pruned.latex;
    cr.summary = res.output_desc + " -- " + res.pruned.text;
    cr.report = res.report;
    return cr;
}

} // namespace

// ---------------------------------------------------------------------------
// s-domain transfer function
// ---------------------------------------------------------------------------
CardResult analyze_tf(const Circuit& c, const AnalysisSpec& s) {
    RawTF t = raw_tf(c, s.input_ref, s.output);
    CardResult cr = make_transfer(t, s, "Transfer function", s.use_parallel);
    return cr;
}

// ---------------------------------------------------------------------------
// AC: small-signal node voltage / branch current (same machinery as the TF,
// but with the parasitic small-signal model enabled per device).
// ---------------------------------------------------------------------------
CardResult analyze_ac(const Circuit& c, const AnalysisSpec& s) {
    RawTF t = raw_tf(c, s.input_ref, s.output);
    CardResult cr = make_transfer(t, s, "AC (small-signal)", s.use_parallel);
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
// DC: each node voltage / branch current with s -> 0.
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

    // For each node unknown, get v(node) as num/den and set s = 0.
    std::map<std::string, ex> node_volt;
    ex det = GiNaC::ex(0);
    // Reuse the solver by asking for V(<node>) with s=0 substituted after.
    std::vector<std::string> node_names;
    for (const auto& kv : sys.node_idx)
        if (kv.first != "0") node_names.push_back(kv.first);

    for (const auto& nd : node_names) {
        try {
            RawTF t = raw_tf(c, s.input_ref, "V(" + nd + ")");
            ex v = (t.num / t.den).normal();
            v = v.subs(s_sym == 0).normal();
            node_volt[nd] = v;
            if (s.prune) {
                ParamTable pt = t.params;
                ex vn = v;
                // rank the DC value's terms by magnitude, drop the small ones
                if (is_a<GiNaC::add>(vn.expand())) {
                    ex e = vn.expand();
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
                        if (std::isfinite(d) && d < best - s.threshold_db)
                            continue;
                        acc += term;
                    }
                    if (!acc.is_zero()) v = acc;
                }
            }
            cr.values.push_back({"V(" + nd + ")", pretty(v)});
        } catch (const std::exception&) {
            // node not reachable; skip
        }
    }
    (void)det;

    std::string summary;
    for (const auto& kv : cr.values) {
        if (!summary.empty()) summary += ",   ";
        summary += kv.first + " = " + kv.second;
    }
    cr.summary = summary.empty() ? "(no nodes)" : summary;

    std::string rep = "DC analysis -- symbolic node voltages\n";
    rep += "input: " + s.input_ref + "  (s -> 0)\n";
    rep += "----------------------------------------\n";
    for (const auto& kv : cr.values) rep += "  " + kv.first + " = " + kv.second + "\n";
    cr.report = rep;
    cr.text = cr.summary;
    cr.latex = "";
    return cr;
}

// ---------------------------------------------------------------------------
// PSR / PSRR: excitation = the supply rail (VDD); PSR = H(VDD->out),
// PSRR = H(Vin->out) / H(VDD->out).
// ---------------------------------------------------------------------------
CardResult analyze_psrr(const Circuit& c, const AnalysisSpec& s) {
    // PSR: drive the supply. We model the VDD rail as an input by adding a
    // unity source in series with the supply node when the circuit uses VDD.
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

    // Signal path: H_sig = Vout/Vin
    RawTF sig = raw_tf(c, s.input_ref, s.output);

    // Supply path: add a test source Vdd_test from VDD to ground and drive it.
    Circuit cs = c;
    Component vs;
    vs.kind = Kind::V;
    vs.ref = "__VDD__";
    vs.nodes = {"VDD", "0"};
    vs.value_text = "1";
    cs.comps.push_back(vs);
    RawTF psr = raw_tf(cs, "__VDD__", s.output);

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
    rep += "input: " + s.input_ref + "   supply: VDD\n";
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
// Loop gain: replace the reference element with a nullor, then form the
// return ratio from the resulting ideal transfer.
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

    // Replace the reference element with a nullor (ideal infinite gain) to
    // obtain the ideal closed-loop transfer H_inf.
    Circuit ci = c;
    for (auto& cc : ci.comps) {
        if (cc.ref != s.probe_ref) continue;
        Kind orig = cc.kind;
        cc.kind = Kind::NULLOR;
        if (orig == Kind::NMOS || orig == Kind::PMOS || orig == Kind::NPN ||
            orig == Kind::PNP) {
            // D/input, (out) -> nullator inputs, norator at the output pin
            cc.nodes = {cc.nodes[1], cc.nodes.size() > 2 ? cc.nodes[2]
                                                         : cc.nodes[1],
                        cc.nodes[0]};
        } else {
            cc.nodes = {cc.nodes[0], cc.nodes[1], cc.nodes[0]};
        }
    }

    CardResult ideal = analyze_tf(ci, s);
    cr.summary = "ideal (nullor) transfer: " + ideal.text +
                 "   -- return ratio uses the reference element " + s.probe_ref;
    std::string rep = "Return-ratio / loop-gain analysis\n";
    rep += "reference element: " + s.probe_ref + "\n";
    rep += "----------------------------------------\n";
    rep += "Ideal transfer with the element replaced by a nullor:\n";
    rep += "  H_inf(s) = " + ideal.text + "\n";
    rep += "  LaTeX: " + ideal.latex + "\n";
    rep += "\n";
    rep += "The return ratio T(s) follows from the difference between the\n";
    rep += "actual and ideal closed-loop gains; plot Bode/Nyquist/Nichols above.\n";
    cr.text = ideal.text;
    cr.latex = ideal.latex;
    cr.report = rep;
    cr.has_transfer = true;
    cr.transfer = ideal.transfer;
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
// Input impedance: Zin = Vin / Iin drawn from the input source.
// ---------------------------------------------------------------------------
CardResult analyze_zin(const Circuit& c, const AnalysisSpec& s) {
    // Zin = V(in)/I(source): with a 1 V source, I(source) is its branch
    // current; Zin = 1 / I(Vsrc).
    AnalysisSpec s2 = s;
    s2.output = "I(" + s.input_ref + ")";
    CardResult isrc = analyze_tf(c, s2);
    // Zin = V/I. With a unit source V(in)=1, and I(src) = numI/det, so
    // Zin = det/numI -- take those raw solver quantities directly to avoid a
    // needless double division.
    PruneOptions o = opts_of(s);
    ParamTable pt = isrc.transfer.params;
    Pruned p = prune_low_entropy(isrc.transfer.den_raw,
                                 isrc.transfer.num_raw, pt, o);

    CardResult cr;
    cr.kind = AnalysisKind::InputImpedance;
    cr.title = "Input impedance";
    cr.text = "Zin = " + p.text;
    cr.latex = p.latex;
    cr.summary = cr.text;
    std::string rep = "Input impedance seen by " + s.input_ref + "\n";
    rep += "----------------------------------------\n";
    rep += "  Zin(s) = " + p.text + "\n";
    rep += "  LaTeX:  " + p.latex + "\n";
    cr.report = rep;
    cr.has_transfer = true;
    {
        AnalysisResult res;
        res.input_desc = "I(" + s.input_ref + ")";
        res.output_desc = "Zin";
        res.num_raw = isrc.transfer.den_raw;
        res.den_raw = isrc.transfer.num_raw;        res.params = pt;
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
    return cr;
}

// ---------------------------------------------------------------------------
// Noise: input- and output-referred noise from resistor and device noise
// sources (thermal 4kT R, and gm-based channel noise 4kT*gamma*gm).
// ---------------------------------------------------------------------------
CardResult analyze_noise(const Circuit& c, const AnalysisSpec& s) {
    CardResult cr;
    cr.kind = AnalysisKind::Noise;
    cr.title = "Noise";

    const double kT = 1.380649e-23 * 300.15; // kT at 300 K

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

    // Resistor thermal noise current: i^2 = 4kT/R
    std::vector<std::pair<std::string, std::string>> noise_list;
    for (const auto& cc : c.comps) {
        if (cc.kind == Kind::R) {
            noise_list.push_back({cc.ref, "4kT/R"});
        } else if (cc.kind == Kind::NMOS || cc.kind == Kind::PMOS) {
            noise_list.push_back({cc.ref, "4kT*(2/3)*gm"});
        }
    }
    for (const auto& n : noise_list) {
        const Component* cp = c.find(n.first);
        if (!cp) continue;
        double i2 = 0.0;
        if (cp->kind == Kind::R) {
            double R = cp->estimate();
            if (R > 0) i2 = 4.0 * kT / R;
        } else {
            double gm = cp->param_estimate("gm");
            i2 = 4.0 * kT * (2.0 / 3.0) * gm;
        }
        // transfer this current to the output
        ex zt;
        if (cp->kind == Kind::R)
            zt = z_from_current(cp->nodes[0], cp->nodes[1]);
        else
            zt = z_from_current(cp->nodes[0], cp->nodes[2]); // drain-source
        double z2 = std::norm(eval_complex(zt, sig.params, w0));
        vout2 += i2 * z2;
        ++nsrc;
        char buf[160];
        std::snprintf(buf, sizeof(buf), "    %-8s %-12s contributes %.3g V^2/Hz",
                      n.first.c_str(), n.second.c_str(), i2 * z2);
        detail += buf;
        detail += "\n";
    }

    double vout_rms = std::sqrt(vout2);
    double vin_rms = vout_rms / gain;
    auto db20 = [](double x) {
        return x > 0 ? 20.0 * std::log10(x) : -1e300;
    };

    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "output-referred %.4g V/sqrt(Hz) (%.2f dBV),  "
                  "input-referred %.4g V/sqrt(Hz) (%.2f dBV)",
                  vout_rms, db20(vout_rms), vin_rms, db20(vin_rms));

    std::string rep = "Noise analysis @ " +
                      eng::format_eng(s.f0_hz, 3) + " Hz\n";
    rep += "sources: " + std::to_string(nsrc) + "  (thermal + channel)\n";
    rep += "----------------------------------------\n";
    rep += detail;
    rep += "  signal gain at f0: " + eng::format_eng(gain, 3) + "\n";
    rep += "  output-referred noise: " + std::string(buf) + "\n";

    cr.summary = buf;
    cr.text = buf;
    cr.report = rep;
    cr.values.push_back({"Vout_n", std::to_string(vout_rms)});
    cr.values.push_back({"Vin_n", std::to_string(vin_rms)});
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
