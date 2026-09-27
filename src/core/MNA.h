#pragma once
#include "core/Netlist.h"
#include "core/ParamTable.h"

#include <ginac/ginac.h>

// CLN's As() macro corrupts wxWidgets' wxAny::As<T>(); undo it.
#ifdef As
#undef As
#endif

#include <map>
#include <set>
#include <string>
#include <vector>

namespace syms {

// Symbolic Modified-Nodal-Analysis system for one circuit and one driven
// input source. The input source is always driven with1 (per-unit
// transfer function), all other independent sources are zeroed.
struct MnaSystem {
    GiNaC::matrix Y;      // n x n
    GiNaC::matrix b;      // n x 1
    std::vector<std::string> var_names;      // "v(out)", "i(V1)", ...
    std::map<std::string, int> node_idx;     // node name -> unknown (no ground)
    std::map<std::string, int> branch_idx;   // component ref -> unknown
    int n = 0;
    ParamTable params;
};

// Throws std::runtime_error with a user-facing message on:
//   invalid circuit, unknown/invalid input source, missing ground.
// `used_nodes` are nodes the caller will probe; they are excluded from the
// structural series fold so `V(node)` still resolves. The 2-argument form
// protects only the empty set (callers that probe nothing extra).
MnaSystem build_mna(const Circuit& c, const std::string& input_ref);
MnaSystem build_mna(const Circuit& c, const std::string& input_ref,
                    const std::set<std::string>& used_nodes);

// Compute one time constant per reactive element (capacitors and inductors) of
// `c`. Amplifier gain-bandwidth poles are added by the caller (they are not
// physical reactive elements). `used_nodes` must match the main analysis's set
// so the structural series fold is identical (a mismatched fold would break the
// time-constant/denominator identity).
std::vector<TimeConstant> open_circuit_time_constants(
    const Circuit& c, const std::string& input_ref, ParamTable& params);
std::vector<TimeConstant> open_circuit_time_constants(
    const Circuit& c, const std::string& input_ref, ParamTable& params,
    const std::set<std::string>& used_nodes);

// Zero-value (open-circuit) time-constant computation. For the network with
// every independent source zeroed, a test current is injected across one
// reactive element's terminals to read the resistance it sees; the element's
// time constant is R*C (capacitor) or L/R (inductor). `open_caps` names the
// capacitor symbols left open (all of them except the one under test) and
// `short_inds` the inductor refs replaced by a short.
struct MnaOctc {
    std::set<std::string> open_caps;   // capacitor symbol names to omit
    std::set<std::string> short_inds;  // inductor refs replaced by a short
    std::string inj_a, inj_b;          // nodes the 1A test current flows between
    GiNaC::ex test = GiNaC::ex(1);     // injected current
    std::string probe_node;            // node whose voltage is the resistance
};

// Return-ratio test mode (Rosenstark): all independent sources are zeroed and
// the reference amplifier's output branch is driven by a fixed test voltage,
// with the amplifier's control coupling and gain-bandwidth pole removed. The
// returned control-port voltage is then the feedback factor beta, and the
// return ratio is T = A(s)*beta. This never breaks the loop -- it is a
// stamp-level substitution.
struct MnaTest {
    std::string amp_ref;         // reference amplifier component
    GiNaC::ex value = GiNaC::ex(1); // test voltage applied at the output
};
MnaSystem build_mna(const Circuit& c, const std::string& input_ref,
                    const MnaTest* test);

} // namespace syms
