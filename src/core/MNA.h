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

// Large-signal DC ("supply") mode. When present, every independent source is
// stamped with its *DC value* (Component::dc_text) on the right-hand side
// instead of the per-unit AC excitation, and MOSFETs are stamped as their
// large-signal saturation model rather than the linearised small-signal VCCS.
// This is what makes the DC operating point a real bias solve. The caller
// checks each device is in saturation.
struct MnaDcSupply {
    double vth = 0.6;  // MOSFET threshold voltage, from the DC tech settings
    // GmOverId (default): Id is an unknown and the gate row is
    // vgs = 2*Id/gm + Vth. SquareLaw: Id = 1/2*uCox*(W/L)*Vov^2 is a symbolic
    // current source (Vov, W, L are symbols); no Id unknown.
    bool square_law = false;
    double uncox = 200e-6; // uN*Cox (A/V^2), Mode 2
    double upcox = 100e-6; // uP*Cox (A/V^2), Mode 2
};
MnaSystem build_mna(const Circuit& c, const std::string& input_ref,
                    const std::set<std::string>& used_nodes,
                    const MnaDcSupply* dc_supply);

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

// Register a device parameter symbol and return the expression to use in the
// matrices. For a mirrored device this is NOT an independent symbol: it is the
// unit device's symbol scaled by the copy count (gm_M2 -> gm_M1, gm_M4 ->
// 4*gm_M1, ro_M2 -> ro_M1/mult), so the solver can cancel ratios and factor the
// copy out. Exposed so the noise sources use the same substitution.
GiNaC::ex reg_param(ParamTable& pt, const Component& c,
                    const std::string& p);

// The symbolic value of a passive component, mirror-aware: a copy of a unit
// contributes `mult * value_unit`, so a copied resistor is named by its unit's
// symbol (Rf -> 6*Rin). Exposed so the noise sources collapse a "copy of" the
// same way the MNA stamps do.
GiNaC::ex value_symbol_scaled(ParamTable& pt, const Circuit& circ,
                              const Component& c);

} // namespace syms
