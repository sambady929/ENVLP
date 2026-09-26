#pragma once
#include "core/ParamTable.h"

#include <ginac/ginac.h>

// CLN's As() macro corrupts wxWidgets' wxAny::As<T>(); undo it.
#ifdef As
#undef As
#endif

#include <complex>
#include <string>
#include <vector>

namespace syms {

// A term that was dropped by magnitude pruning.
struct Dropped {
    std::string where; // "denominator, s^1"
    std::string term;  // pretty text of the dropped term
    double db_rel;     // dB below the term it was ranked against
};

// One (1 + s*tau) / s / higher-order factor.
struct Factor {
    GiNaC::ex expr;
    std::string text;
    bool origin = false; // pure s^k factor
};

// A pole or zero with its location and (when recognisable) its component
// expression, e.g. tau = (R1||R2)*C1.
struct Root {
    double omega = 0.0; // rad/s, 0 = at the origin
    double tau = 0.0;
    double f_hz = 0.0;
    bool real = true;
    double q = 0.0;
    std::string label;  // "(R1||R2)*C1"
    std::string factor;
    GiNaC::ex omega_expr;     // corner frequency 1/tau (simplified)
    std::string latex_label;  // LaTeX for the time-constant expression
    std::string latex_factor; // LaTeX for the (1 + s*tau) factor
    GiNaC::ex factor_expr;    // the (1 + s*tau) / higher-order factor itself
};

struct LowEntropy {
    GiNaC::ex gain;
    GiNaC::ex num_poly, den_poly;      // pruned polynomials, den c0 == 1
    std::vector<Factor> num_factors, den_factors;
    std::vector<Root> zeros, poles;
    std::vector<Dropped> dropped;
    std::string text;      // factored form with || and · (UTF-8)
    std::string text_poly; // expanded (pruned) form
    std::string latex;     // H(s) in LaTeX
    bool numeric_factors = false; // some factors use numeric (approx) roots
    bool exact = false;    // true when pruning was disabled
};

struct LowEntropyOptions {
    // When false, every symbolic term is kept (exact brute-force result).
    // When true, terms far below the dominant one at f0 are dropped and
    // parallel combinations are collapsed into || form.
    bool prune = true;
    double f0_hz = 1e3;
    // Series/parallel component reduction threshold: collapse R1||R2 and
    // R1+R2 when one argument is more than this far (dB) below the other in
    // magnitude. This is a *structural* rule that only ever fires on genuine
    // parallel/series resistor pairs, so a small (20 dB) margin is right.
    double threshold_db = 20.0;
    // Pole/zero reduction threshold: a pole (or zero) is dropped only when it
    // lies more than this far (dB) along the frequency axis from the dominant
    // one. 60 dB == a 1000x frequency ratio, so a 100 kHz pole dominates a
    // 100 MHz pole, but a 10 MHz pole (100x, 40 dB) is still kept. This is
    // the threshold used by magnitude pruning of the s-polynomial terms that
    // determine the poles/zeros.
    double pole_zero_threshold_db = 60.0;
    bool global_ref = false; // rank against the whole polynomial at f0
    bool use_parallel = true; // rewrite R1*R2/(R1+R2) as R1||R2
    // When the exact denominator/ numerator does not factor symbolically,
    // estimate each coefficient numerically and factor the resulting real
    // polynomial, then rebuild component-labelled factors. Bounded by the
    // pruning threshold so a factor is only accepted when its root is real
    // (within a few % of its imaginary part) and its contribution at f0 is
    // meaningful.
    bool approx_factor = true;
    // Normalize the denominator to a constant term of 1 (pulling the DC gain
    // out front). Ideal for transfer functions -- it is what makes the output
    // read as (1 + s*tau1)(1 + s*tau2) -- but for an impedance whose
    // denominator constant is itself a symbolic sum it would turn the numerator
    // into nested fractions, so it is disabled for those analyses.
    bool normalize = true;
    // Term ranking band. When band_hi_hz >= band_lo_hz > 0 each term is ranked
    // by its peak magnitude across the band (the standard SPICE sweep range);
    // otherwise the single frequency f0_hz is used.
    double band_lo_hz = 1.0;
    double band_hi_hz = 0.0; // 0 = unset -> use f0_hz
};

// The low-entropy engine. This is the heart of SymCirc: it turns a raw
// numerator/denominator pair from MNA into a compact, factored,
// design-readable form:
//   1. (optional) magnitude pruning of negligible terms using estimates
//   2. parallel/series structural simplification (R1||R2, ...)
//   3. factor extraction by matching physical time constants (TTC style)
//   4. pole/zero tables with component labels
LowEntropy low_entropy(const GiNaC::ex& num, const GiNaC::ex& den,
                       ParamTable& params, const LowEntropyOptions& opts);

// Roots of a real-coefficient polynomial in ascending order (Durand-Kerner).
std::vector<std::complex<double>> poly_roots(std::vector<double> coeff);

// numeric evaluation helpers
std::complex<double> eval_complex(const GiNaC::ex& e, const ParamTable& params,
                                  double omega);
double eval_mag_db(const GiNaC::ex& e, const ParamTable& params, double omega);
double eval_phase_deg(const GiNaC::ex& e, const ParamTable& params, double omega);

// LaTeX for an arbitrary expression (uses GiNaC's print_latex; par() prints
// with \parallel).
std::string to_latex(const GiNaC::ex& e);
// LaTeX for the low-entropy result, e.g. H(s) = \frac{...}{...}
std::string low_entropy_latex(const LowEntropy& le);

} // namespace syms
