#include "core/Engine.h"
#include "core/Eng.h"
#include "core/Print.h"

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
    r.opts.global_ref = req.global_ref;
    r.opts.prune = req.prune;
    r.opts.use_parallel = true; // hardcoded on (#6)
    r.opts.gm_ro_assume = req.gm_ro_assume;
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
    return eng::format_si(hz) + " Hz";
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
                          "    %d) w = %-16s (%s)   tau = %s\n", i,
                          fmt_rads(r.omega).c_str(), fmt_hz(hz).c_str(),
                          r.label.empty() ? "-" : r.label.c_str());
        } else {
            std::snprintf(buf, sizeof(buf),
                          "    %d) wn = %-14s (%s)   Q = %.2f   wn^2 = %s\n", i,
                          fmt_rads(r.omega).c_str(), fmt_hz(hz).c_str(), r.q,
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

} // namespace

std::string format_report(const AnalysisResult& r) {
    std::string out;
    out += "SymCirc analysis -- " + r.output_desc + " per " + r.input_desc + "\n";
    // frequency-sweep line, in the same terms the analysis card uses
    const char* typ = r.sweep.type == SweepType::Linear
                          ? "linear"
                          : r.sweep.type == SweepType::Octave ? "octave"
                                                              : "decade";
    char hdr[320];
    std::snprintf(hdr, sizeof(hdr),
                  "sweep: %s .. %s, %s, %d pts/interval   "
                  "(ignore terms below %.0f dB)\n",
                  fmt_hz(r.sweep.f_start_hz).c_str(),
                  fmt_hz(r.sweep.f_stop_hz).c_str(), typ,
                  r.sweep.points_per_interval, r.opts.threshold_db);
    out += hdr;
    out += "----------------------------------------------------------------\n";
    out += "Low-entropy transfer function:\n";
    out += "  H(s) = " + r.pruned.text + "\n";
    out += "\nExpanded (pruned) form:\n";
    out += "  H(s) = " + r.pruned.text_poly + "\n";
    out += "\nLaTeX (copy into a paper/slides):\n";
    out += "  " + r.pruned.latex + "\n";
    if (r.pruned.numeric_factors)
        out += "\nNote: one or more factors are approximate -- the exact "
               "denominator/numerator\n      does not factor symbolically, so "
               "numeric (estimate-based) roots were used.\n";
    out += "\nPoles:\n";
    out += poles_zeros_text(r.pruned.poles, true);
    out += "Zeros:\n";
    out += poles_zeros_text(r.pruned.zeros, false);
    if (!r.pruned.dropped.empty()) {
        out += "\nIgnored terms (" + std::to_string(r.pruned.dropped.size()) +
               "), each below the dominant term by more than " +
               std::to_string(static_cast<int>(r.opts.threshold_db)) + " dB:\n";
        for (const auto& d : r.pruned.dropped) {
            char dbbuf[48];
            std::snprintf(dbbuf, sizeof(dbbuf), "%7.1f dB", d.db_rel);
            out += "    [" + d.location + "]  " + d.term + "   " + dbbuf + "\n";
        }
    } else {
        out += "\nNo terms ignored (everything is within the margin).\n";
    }
    return out;
}

} // namespace syms
