// Legacy adapter: the real low-entropy engine is in LowEntropy.cpp. This file
// keeps the original Pruned/PruneOptions API working for existing callers and
// tests by translating the engine's result into that struct.
#include "core/Prune.h"
#include "core/LowEntropy.h"
#include "core/Par.h"

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

    LowEntropy le = low_entropy(num, den, params, lo);

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
}

} // namespace syms
