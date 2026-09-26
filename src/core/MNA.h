#pragma once
#include "core/Netlist.h"
#include "core/ParamTable.h"

#include <ginac/ginac.h>

// CLN's As() macro corrupts wxWidgets' wxAny::As<T>(); undo it.
#ifdef As
#undef As
#endif

#include <map>
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
MnaSystem build_mna(const Circuit& c, const std::string& input_ref);

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
