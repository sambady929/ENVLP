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

struct DroppedTerm {
    std::string location; // "numerator, s^2" / "denominator, constant"
    std::string term;     // pretty text of the dropped term
    double db_rel;        // dB relative to the dominant term it was cut against
};

struct FactorInfo {
    GiNaC::ex expr;   // factor as printed (normalized: constant term == 1)
    std::string text; // pretty text: "1 + s*R1*C1" or "s"
    bool origin_s = false;
};

struct RootInfo {
    double omega = 0.0;  // rad/s (for real factors: 1/tau; origin: 0)
    double tau = 0.0;    // time constant (real linear factors only)
    bool real_root = true;
    double q = 0.0;      // quality factor for complex pairs
    std::string label;   // "R1*C1" when recognizable, else ""
    std::string factor_text;
};

struct Pruned {
    GiNaC::ex gain; // K (DC gain with s-normalized factors)
    std::vector<FactorInfo> num_factors;
    std::vector<FactorInfo> den_factors;
    GiNaC::ex num_poly; // pruned polynomials, denominator constant term = 1
    GiNaC::ex den_poly;
    std::vector<DroppedTerm> dropped;
    std::vector<RootInfo> poles, zeros;
    std::string text;      // H(s) factored low-entropy form
    std::string text_poly; // H(s) expanded (pruned) polynomial form
    std::string latex;     // H(s) in LaTeX (from the factored form)
    bool exact = false;    // true when pruning was disabled
};

struct PruneOptions {
    double f0_hz = 1e3;
    double threshold_db = 40.0;
    bool global_ref = false; // true => rank against the whole polynomial at f0
    bool prune = true;       // false => keep every symbolic term (exact)
    bool use_parallel = true; // rewrite R1*R2/(R1+R2) as R1||R2
};

// The low-entropy engine:
//  1. ranks every term of num/den by its magnitude at f0 using the user's
//     component estimates, drops everything below threshold_db
//  2. normalizes (denominator constant term = 1, extracts gain K)
//  3. factors into (1 + s*tau) pieces: degree-1 always exactly, higher
//     orders by matching physically-meaningful time constants (R*C, L/R,
//     C/gm, ...) and verifying exact symbolic division
//  4. produces pole/zero tables with component labels
Pruned prune_low_entropy(const GiNaC::ex& num, const GiNaC::ex& den,
                         ParamTable& params, const PruneOptions& opts);

// Roots of a real-coefficient polynomial (Durand-Kerner).
// coeff[k] is the coefficient of s^k (ascending order).
std::vector<std::complex<double>> poly_roots(std::vector<double> coeff);

// Numeric evaluation helpers (used by pruning, tests, and the Bode plot).
// Returns {re, im}; NaNs if the expression cannot be fully evaluated.
std::complex<double> eval_complex(const GiNaC::ex& e, const ParamTable& params,
                                  double omega);
double eval_mag_db(const GiNaC::ex& e, const ParamTable& params, double omega);
double eval_phase_deg(const GiNaC::ex& e, const ParamTable& params, double omega);

} // namespace syms
