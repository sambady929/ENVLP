#pragma once
#include "core/MNA.h"
#include "core/Netlist.h"

#include <ginac/ginac.h>

// CLN's As() macro corrupts wxWidgets' wxAny::As<T>(); undo it.
#ifdef As
#undef As
#endif

#include <string>

namespace syms {

// How the frequency sweep points are distributed between f_start and f_stop
// (standard SPICE AC sweep controls).
enum class SweepType { Decade, Octave, Linear };

// Points per interval (per decade / per octave / total for linear).
struct SweepSpec {
    double f_start_hz = 1.0;
    double f_stop_hz = 1e9;
    SweepType type = SweepType::Decade;
    int points_per_interval = 10;
};

struct AnalysisRequest {
    std::string input_ref;  // which ideal source drives the circuit
    std::string output;     // "V(node)" or "I(ref)"
    double f0_hz = 1000.0;  // frequency the low-entropy form is tuned to
    double threshold_db = 20.0; // series/parallel component reduction (dB)
    double pole_zero_threshold_db = 60.0; // pole/zero frequency reduction (dB)
    bool global_ref = false; // false => rank within each s-coefficient
                             // true => rank against whole polynomial
    bool prune = true;       // false => keep every symbolic term (exact)
    bool use_parallel = true; // express R1*R2/(R1+R2) as R1||R2
    bool approx_factor = true; // numeric factoring when exact factoring fails
    bool normalize = true; // normalize denominator DC term to 1
    // Frequency sweep used for ranking and for the plots. When `rank_omega`
    // is non-empty each term is ranked by its peak magnitude across the sweep;
    // otherwise the single frequency f0_hz is used.
    SweepSpec sweep;
};

struct Solved {
    GiNaC::ex num; // H = num / den, both polynomials in s
    GiNaC::ex den;
    std::string input_desc;   // "V1"
    std::string output_desc;  // "V(out)"
    ParamTable params;
};

// Throws std::runtime_error with a user-facing message.
Solved solve(const Circuit& c, const AnalysisRequest& req);

} // namespace syms
