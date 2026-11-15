// Legacy adapter: the real low-entropy engine is in LowEntropy.cpp. This file
// keeps the original Pruned/PruneOptions API working for existing callers and
// tests by translating the engine's result into that struct.
#include "core/Prune.h"
#include "core/LowEntropy.h"
#include "core/Par.h"

#include <cmath>
#include <complex>

namespace syms {

using GiNaC::ex;

Pruned prune_low_entropy(const ex& num, const ex& den, ParamTable& params,
                         const PruneOptions& opts) {
    LowEntropyOptions lo;
    lo.prune = opts.prune;
    lo.f0_hz = opts.f0_hz;
    lo.component_threshold_ratio = opts.component_threshold_ratio;
    lo.pole_zero_threshold_ratio = opts.pole_zero_threshold_ratio;
    lo.threshold_db = opts.threshold_db;
    lo.pole_zero_threshold_db = opts.pole_zero_threshold_db;
    lo.pole_ref = (opts.pole_ref == PruneOptions::PoleRef::UgBw)
                      ? LowEntropyOptions::PoleRef::UgBw
                      : LowEntropyOptions::PoleRef::Dominant;
    lo.ugbw_rads = opts.ugbw_rads;
    lo.global_ref = opts.global_ref;
    lo.use_parallel = opts.use_parallel;
    lo.approx_factor = opts.approx_factor;
    lo.normalize = opts.normalize;
    lo.band_lo_hz = opts.band_lo_hz;
    lo.band_hi_hz = opts.band_hi_hz;
    lo.octc = opts.octc;

    auto convert = [&](const LowEntropy& le) {
        Pruned R;
        R.gain = le.gain;
        R.num_poly = le.num_poly;
        R.den_poly = le.den_poly;
        R.text = le.text;
        R.text_poly = le.text_poly;
        R.latex = le.latex;
        R.exact = !opts.prune;
        R.numeric_factors = le.numeric_factors;
        for (const auto& f : le.num_factors)
            R.num_factors.push_back({f.expr, f.text, f.origin});
        for (const auto& f : le.den_factors)
            R.den_factors.push_back({f.expr, f.text, f.origin});
        for (const auto& d : le.dropped)
            R.dropped.push_back({d.where, d.term, d.db_rel});
        for (const auto& r : le.zeros)
            R.zeros.push_back({r.omega, r.tau, r.real, r.q, r.label, r.factor,
                               r.latex_label, r.latex_factor, r.factor_expr,
                               r.omega_expr});
        for (const auto& r : le.poles)
            R.poles.push_back({r.omega, r.tau, r.real, r.q, r.label, r.factor,
                               r.latex_label, r.latex_factor, r.factor_expr,
                               r.omega_expr});
        return R;
    };

    auto as_expr = [](const Pruned& p) {
        ex e = p.gain;
        for (const auto& f : p.num_factors) e *= f.expr;
        for (const auto& f : p.den_factors) e = e / f.expr;
        return e;
    };

    LowEntropy first = low_entropy(num, den, params, lo);
    Pruned R = convert(first);
    if (!opts.prune) return R;

    // A second pass can expose another cancellation or series/parallel
    // reduction. Accept it only when it is strictly shorter *and* still
    // numerically equal to the exact input. That gives a fixed-point pass
    // without letting a later approximation drift away from the raw result.
    ex exact = (num / den).normal();
    ex n = first.num_poly, d = first.den_poly;
    for (int iter = 0; iter < 3; ++iter) {
        LowEntropy le = low_entropy(n, d, params, lo);
        Pruned candidate = convert(le);
        if (candidate.text.size() >= R.text.size()) break;
        ex candidate_expr = as_expr(candidate);
        bool same = true;
        const double freqs[] = {1.0, 1e3, 1e6, 1e9};
        for (double w : freqs) {
            std::complex<double> a = eval_complex(candidate_expr, params, w);
            std::complex<double> b = eval_complex(exact, params, w);
            if (!std::isfinite(a.real()) || !std::isfinite(a.imag()) ||
                !std::isfinite(b.real()) || !std::isfinite(b.imag()) ||
                std::abs(a - b) > 1e-4 * std::max(std::abs(b), 1e-30)) {
                same = false;
                break;
            }
        }
        if (!same) break;
        R = std::move(candidate);
        n = le.num_poly;
        d = le.den_poly;
    }
    return R;
}

} // namespace syms
