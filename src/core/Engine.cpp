#include "core/Engine.h"
#include "core/Eng.h"
#include "core/LowEntropy.h"
#include "core/Print.h"

#include <cctype>
#include <cmath>
#include <cstdio>

namespace syms {

using GiNaC::ex;

AnalysisResult analyze(const Circuit& c, const AnalysisRequest& req) {
    Solved sv = solve(c, req);

    AnalysisResult r;
    r.input_desc = sv.input_desc;
    r.output_desc = sv.output_desc;
    r.num_raw = sv.num;
    r.den_raw = sv.den;
    r.params = std::move(sv.params);
    r.opts.f0_hz = req.f0_hz;
    r.opts.threshold_db = req.threshold_db;
    r.opts.pole_zero_threshold_db = req.pole_zero_threshold_db;
    r.opts.global_ref = req.global_ref;
    r.opts.prune = req.prune;
    r.opts.use_parallel = true; // hardcoded on (#6)
    r.opts.approx_factor = req.approx_factor;
    r.opts.normalize = req.normalize;
    r.opts.band_lo_hz = req.sweep.f_start_hz;
    r.opts.band_hi_hz = req.sweep.f_stop_hz;
    r.sweep = req.sweep;
    r.pruned = prune_low_entropy(r.num_raw, r.den_raw, r.params, r.opts);
    r.report = format_report(r);
    return r;
}

double mag_db_at(const AnalysisResult& r, double omega) {
    ex h = (r.num_raw / r.den_raw).normal();
    return eval_mag_db(h, r.params, omega);
}

double phase_deg_at(const AnalysisResult& r, double omega) {
    ex h = (r.num_raw / r.den_raw).normal();
    return eval_phase_deg(h, r.params, omega);
}

std::vector<double> sweep_hz(double f0, double f1, int npoints) {
    std::vector<double> out;
    if (npoints < 2) npoints = 2;
    if (f0 <= 0) f0 = 1.0;
    if (f1 <= f0) f1 = f0 * 1000.0;
    double log0 = std::log10(f0), log1 = std::log10(f1);
    for (int i = 0; i < npoints; ++i) {
        double t = static_cast<double>(i) / (npoints - 1);
        out.push_back(std::pow(10.0, log0 + t * (log1 - log0)));
    }
    return out;
}

std::vector<double> sweep_points(const SweepSpec& s) {
    std::vector<double> out;
    int ppi = s.points_per_interval > 0 ? s.points_per_interval : 10;
    if (s.type == SweepType::Linear) {
        double f0 = s.f_start_hz;
        double f1 = s.f_stop_hz > f0 ? s.f_stop_hz : f0 + 1.0;
        int n = ppi > 1 ? ppi : 2;
        for (int i = 0; i < n; ++i)
            out.push_back(f0 + (f1 - f0) * double(i) / double(n - 1));
        return out;
    }
    double f0 = s.f_start_hz > 0 ? s.f_start_hz : 1.0;
    double f1 = s.f_stop_hz > f0 ? s.f_stop_hz : f0 * 1000.0;
    // log-spaced: one interval per decade (or octave); SPICE-style total is
    // intervals * points_per_interval + 1.
    double per = (s.type == SweepType::Octave) ? std::log2(f1 / f0)
                                               : std::log10(f1 / f0);
    int total = int(std::lround(per * ppi)) + 1;
    if (total < 2) total = 2;
    double logf0 = std::log(f0), logf1 = std::log(f1);
    for (int i = 0; i < total; ++i)
        out.push_back(std::exp(logf0 + (logf1 - logf0) * double(i) /
                                            double(total - 1)));
    return out;
}

namespace {

std::string fmt_hz(double hz) {
    if (hz <= 0.0) return "0";
    // Engineering number + prefix ("159M"), then reattach as "159 MHz".
    std::string s = eng::format_eng(hz, 3);
    std::string prefix;
    if (!s.empty() && std::isalpha((unsigned char)s.back())) {
        prefix = s.substr(s.size() - 1);
        s = s.substr(0, s.size() - 1);
    }
    if (prefix.empty()) return s + " Hz";
    return s + " " + prefix + "Hz";
}

std::string fmt_rads(double w) {
    if (w == 0.0) return "0";
    return eng::format_si(w) + " rad/s";
}

std::string poles_zeros_text(const std::vector<RootInfo>& rs, bool is_pole) {
    if (rs.empty()) return "    none\n";
    std::string out;
    int i = 1;
    for (const auto& r : rs) {
        char buf[160];
        if (r.omega == 0.0) {
            std::snprintf(buf, sizeof(buf), "    %d) at the origin (DC)%s\n", i,
                          is_pole ? " -- pole" : " -- zero");
            out += buf;
            ++i;
            continue;
        }
        double hz = std::fabs(r.omega) / (2.0 * M_PI);
        if (r.real_root) {
            std::snprintf(buf, sizeof(buf),
                          "    %d) %s (%s)   tau = %s\n", i,
                          fmt_hz(hz).c_str(), fmt_rads(r.omega).c_str(),
                          r.label.empty() ? "-" : r.label.c_str());
        } else {
            std::snprintf(buf, sizeof(buf),
                          "    %d) %s (%s)   Q = %.2f   wn^2 = %s\n", i,
                          fmt_hz(hz).c_str(), fmt_rads(r.omega).c_str(), r.q,
                          r.label.empty() ? "-" : r.label.c_str());
        }
        out += buf;
        if (!r.factor_text.empty()) {
            out += "         factor: (";
            out += r.factor_text;
            out += ")\n";
        }
        ++i;
    }
    return out;
}

// Numeric gain/bandwidth metrics, computed from the *unpruned* transfer
// function over the sweep range: DC gain, the -3 dB corner, and the unity-gain
// (0 dB) crossover. A single log-spaced scan is reused for both crossings
// (with linear interpolation for sub-point precision).
} // namespace

// ---------------------------------------------------------------------------
// Gain / bandwidth metrics, numeric AND symbolic.
//
// The symbolic forms exploit the factored low-entropy form
//   H(s) = K / ((1 + s/wp0)(1 + s/wp1) ...).
// The dominant (lowest-frequency) pole gives the -3 dB bandwidth whenever it
// is far enough from the others that they barely move the corner. That
// "far enough" is deliberately looser than the 60 dB pole/zero rule: a pole
// 100x (40 dB) beyond the dominant one changes the -3 dB corner by a
// hundredth of a dB, so it is neglected in the *bandwidth* reading even though
// it is still listed in the pole table. Unity gain is more sensitive, so its
// symbolic form only appears when the system is effectively single-pole across
// the whole band (60 dB rule).
// ---------------------------------------------------------------------------
namespace {

constexpr double kBandwidthPoleDb = 40.0; // 100x: negligible for -3 dB

// LaTeX for an expression with an explicit \cdot between factors (the same
// convention the low-entropy engine uses for its factors).


// Symbolic corner-frequency lines omega_p0 = 1/tau for each pole, using the
// component time-constant labels the pruner already produced. The angular
// frequency is written as the Greek omega (both in the text and the LaTeX).
void pole_wp_lines(const AnalysisResult& r, std::vector<std::string>& text,
                   std::vector<std::string>& latex) {
    for (size_t i = 0; i < r.pruned.poles.size(); ++i) {
        const RootInfo& p = r.pruned.poles[i];
        std::string sub = std::to_string(i);
        std::string tag = "\xCF\x89_p" + sub; // UTF-8 omega
        std::string ltag = "\\omega_{p" + sub + "}";
        double hz = std::fabs(p.omega) / (2.0 * M_PI);
        if (p.omega == 0.0) {
            text.push_back(tag + " = 0");
            latex.push_back(ltag + " = 0");
        } else if (p.omega_expr.is_zero()) {
            text.push_back(tag + " = " + fmt_hz(hz));
            latex.push_back(ltag + " = \\mathrm{" + fmt_hz(hz) + "}");
        } else {
            text.push_back(tag + " = " + pretty(p.omega_expr));
            latex.push_back(ltag + " = " + to_latex(p.omega_expr));
        }
    }
}

// Is every pole other than pole 0 at least `db` above it in frequency?
bool others_far(const AnalysisResult& r, double db) {
    const auto& p = r.pruned.poles;
    if (p.empty() || p.front().omega == 0.0) return false;
    double f0 = std::fabs(p.front().omega) / (2.0 * M_PI);
    double lim = std::pow(10.0, db / 20.0);
    for (size_t i = 1; i < p.size(); ++i) {
        if (p[i].omega == 0.0) return false;
        if (std::fabs(p[i].omega) / (2.0 * M_PI) < f0 * lim) return false;
    }
    return true;
}

struct Metrics {
    std::string dc_db, dc_sym;         // DC gain (numeric dB, symbolic)
    std::string bw3, bw3_sym;          // -3 dB bandwidth
    std::string ugbw, ugbw_sym;        // unity-gain bandwidth
    std::string dc_sym_latex, bw3_sym_latex, ugbw_sym_latex;
};

Metrics compute_metrics(const AnalysisResult& r) {
    Metrics m;
    char buf[96];

    // ---- DC gain ----
    double dc = mag_db_at(r, 0.0);
    if (std::isfinite(dc)) {
        std::snprintf(buf, sizeof(buf), "%.2f dB", dc);
        m.dc_db = buf;
    } else {
        m.dc_db = "n/a";
    }
    ex K = r.pruned.gain;
    m.dc_sym = "K = " + pretty(K);
    m.dc_sym_latex = "K = " + to_latex(K);

    // ---- log-spaced magnitude scan for the numeric crossings ----
    double f0 = r.sweep.f_start_hz > 0 ? r.sweep.f_start_hz : 1.0;
    double f1 = r.sweep.f_stop_hz > f0 ? r.sweep.f_stop_hz : f0 * 1e6;
    const int N = 400;
    std::vector<double> f(N), mag(N);
    for (int i = 0; i < N; ++i) {
        double t = double(i) / (N - 1);
        f[i] = f0 * std::pow(f1 / f0, t);
        mag[i] = mag_db_at(r, 2.0 * M_PI * f[i]);
    }
    auto find_cross = [&](double target_db) -> double {
        if (std::isfinite(mag[0]) && mag[0] <= target_db) return f0; // below start
        for (int i = 1; i < N; ++i)
            if (std::isfinite(mag[i - 1]) && std::isfinite(mag[i]) &&
                mag[i - 1] > target_db && mag[i] <= target_db) {
                double frac = (mag[i - 1] - target_db) / (mag[i - 1] - mag[i]);
                return f[i - 1] + frac * (f[i] - f[i - 1]);
            }
        return -1.0;
    };
    double bw3 = std::isfinite(dc) ? find_cross(dc - 3.0) : -1.0;
    double ugbw = find_cross(0.0);
    auto fmt_bw = [&](double v) -> std::string {
        if (v < 0.0) return "> " + fmt_hz(f1);
        if (v <= f0) return "< " + fmt_hz(f0);
        return fmt_hz(v);
    };
    m.bw3 = fmt_bw(bw3);
    m.ugbw = ugbw < 0.0 ? "none within sweep" : fmt_bw(ugbw);

    // ---- symbolic -3 dB: the dominant pole (angular form, rad/s) ----
    const std::string W = "\xCF\x89"; // UTF-8 omega
    if (others_far(r, kBandwidthPoleDb) && !r.pruned.poles.empty()) {
        const RootInfo& p0 = r.pruned.poles.front();
        if (!p0.omega_expr.is_zero()) {
            m.bw3_sym = W + "_(-3dB) = " + W + "_p0 = " +
                        pretty(p0.omega_expr);
            m.bw3_sym_latex = "\\omega_{-3\\mathrm{dB}} = \\omega_{p0} = " +
                              to_latex(p0.omega_expr);
        }
    }

    // ---- symbolic unity gain: K*omega_p0 (angular, rad/s) when single-pole ----
    if (others_far(r, r.opts.pole_zero_threshold_db) &&
        !r.pruned.poles.empty() && !r.pruned.poles.front().omega_expr.is_zero()) {
        const RootInfo& p0 = r.pruned.poles.front();
        m.ugbw_sym = W + "_0dB = K*" + W + "_p0 = (" + pretty(K) + ")*(" +
                     pretty(p0.omega_expr) + ")";
        m.ugbw_sym_latex = "\\omega_{0\\mathrm{dB}} = K\\,\\omega_{p0} = " +
                           to_latex(K) + "\\cdot " +
                           to_latex(p0.omega_expr);
    }
    return m;
}

std::string metrics_text(const AnalysisResult& r) {
    Metrics m = compute_metrics(r);
    std::string out;
    out += "  DC Gain: " + m.dc_db;
    if (!m.dc_sym.empty()) out += "   [" + m.dc_sym + "]";
    out += "\n";
    out += "  -3 dB Bandwidth: " + m.bw3;
    if (!m.bw3_sym.empty()) out += "   [" + m.bw3_sym + "]";
    out += "\n";
    out += "  Unity-Gain (0 dB) Bandwidth: " + m.ugbw;
    if (!m.ugbw_sym.empty()) out += "   [" + m.ugbw_sym + "]";
    out += "\n";
    // Symbolic pole corner frequencies (omega_p0 = 1/tau_0, ...).
    std::vector<std::string> ptxt, ptex;
    pole_wp_lines(r, ptxt, ptex);
    if (!ptxt.empty()) {
        out += "  Pole Corner Frequencies:\n";
        for (const auto& line : ptxt) out += "    " + line + "\n";
    }
    return out;
}

std::string metrics_latex(const AnalysisResult& r) {
    Metrics m = compute_metrics(r);
    std::string out;
    out += "Gain / Bandwidth:\n";
    out += "\\mathrm{DC\\ Gain} = " + (m.dc_sym_latex.empty()
                                          ? "\\mathrm{" + m.dc_db + "}"
                                          : m.dc_sym_latex) +
           "\\quad = \\mathrm{" + m.dc_db + "}\n";
    out += "\\mathrm{-3\\ dB\\ Bandwidth} = " +
           (m.bw3_sym_latex.empty() ? "\\mathrm{" + m.bw3 + "}"
                                    : m.bw3_sym_latex) +
           "\\quad = \\mathrm{" + m.bw3 + "}\n";
    out += "\\mathrm{Unity-Gain\\ Bandwidth} = " +
           (m.ugbw_sym_latex.empty() ? "\\mathrm{" + m.ugbw + "}"
                                     : m.ugbw_sym_latex) +
           "\\quad = \\mathrm{" + m.ugbw + "}\n";
    std::vector<std::string> ptxt, ptex;
    pole_wp_lines(r, ptxt, ptex);
    for (const auto& line : ptex) out += line + "\n";
    return out;
}

} // namespace

// Plain-text report shown in the Results tab. This is deliberately NOT
// LaTeX: no \frac, no \cdot, no \parallel -- just readable ASCII/math text.
// The Math tab renders the LaTeX form (AnalysisResult::pruned.latex)
// separately, so each view does one job well.
std::string format_report(const AnalysisResult& r) {
    std::string out;
    out += "H(s) = " + r.pruned.text + "\n";
    if (r.pruned.numeric_factors)
        out += "(one or more factors are approximate: the exact denominator "
               "does not factor symbolically, so numeric estimate-based roots "
               "were used)\n";
    out += "\nGain / Bandwidth:\n";
    out += metrics_text(r);
    out += "\nPoles:\n";
    out += poles_zeros_text(r.pruned.poles, true);
    out += "Zeros:\n";
    out += poles_zeros_text(r.pruned.zeros, false);
    if (!r.pruned.dropped.empty()) {
        out += "\nNeglected terms:\n";
        for (const auto& d : r.pruned.dropped) {
            char dbbuf[48];
            std::snprintf(dbbuf, sizeof(dbbuf), "%6.1f dB", d.db_rel);
            out += "  " + d.term + "   " + dbbuf + "\n";
        }
    }
    (void)r.sweep;
    return out;
}

// One pole/zero line in LaTeX: the numeric corner frequency as \mathrm{}
// text, the time-constant expression typeset with \cdot and \parallel, and
// the (1 + s*tau) factor on a second indented line (matching the plain-text
// view).
std::string pole_zero_latex(const RootInfo& r, int i) {
    std::string line = std::to_string(i) + ")\\ ";
    if (r.omega == 0.0) {
        line += "\\mathrm{0\\ Hz}\\ \\mathrm{(origin)}";
    } else {
        double hz = std::fabs(r.omega) / (2.0 * M_PI);
        // Hz first (engineering notation), rad/s in parentheses (exponent).
        line += "\\mathrm{" + fmt_hz(hz) + "}";
        line += "\\ \\mathrm{(" + fmt_rads(r.omega) + ")}";
        if (!r.latex_label.empty())
            line += ",\\quad \\tau = " + r.latex_label;
    }
    if (!r.latex_factor.empty())
        line += "\n\\quad \\mathrm{factor:}\\ " + r.latex_factor;
    return line;
}

std::string format_report_latex(const AnalysisResult& r) {
    std::string out;
    out += metrics_latex(r);
    out += "\nPoles:\n";
    if (r.pruned.poles.empty()) {
        out += "\\mathrm{none}\n";
    } else {
        for (size_t i = 0; i < r.pruned.poles.size(); ++i)
            out += pole_zero_latex(r.pruned.poles[i], int(i + 1)) + "\n";
    }
    out += "Zeros:\n";
    if (r.pruned.zeros.empty()) {
        out += "\\mathrm{none}\n";
    } else {
        for (size_t i = 0; i < r.pruned.zeros.size(); ++i)
            out += pole_zero_latex(r.pruned.zeros[i], int(i + 1)) + "\n";
    }
    return out;
}

} // namespace syms
