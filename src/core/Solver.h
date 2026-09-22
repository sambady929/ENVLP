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

struct AnalysisRequest {
    std::string input_ref;  // which ideal source drives the circuit
    std::string output;     // "V(node)" or "I(ref)"
    double f0_hz = 1000.0;  // frequency the low-entropy form is tuned to
    double threshold_db = 40.0; // drop terms more than this far below dominant
    bool global_ref = false; // false => rank within each s-coefficient
                             // true => rank against whole polynomial (uses f0)
    bool prune = true;       // false => keep every symbolic term (exact)
    bool use_parallel = true; // express R1*R2/(R1+R2) as R1||R2
    bool gm_ro_assume = true; // assume gm*ro >> 1 when idealizing
    bool approx_factor = true; // numeric factoring when exact factoring fails
    bool normalize = true; // normalize denominator DC term to 1
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
