#pragma once
#include "core/MNA.h"
#include "core/Netlist.h"

#include <ginac/ginac.h>

// CLN's As() macro corrupts wxWidgets' wxAny::As<T>(); undo it.
#ifdef As
#undef As
#endif

#include <set>
#include <string>
#include <map>
#include <vector>

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

// Large-signal DC model for MOSFETs. Two modes:
//   GmOverId  : idealised. gm is a per-device small-signal parameter; the
//               operating point is solved symbolically in gm (vgs = 2*Id/gm +
//               Vth).
//   SquareLaw : symbolic square law. uCox is a global process value and W/L is
//               a per-device *symbol* (W_M1/L_M1); everything is symbolic in
//               the overdrive.
enum class DcMode { GmOverId, SquareLaw };

// Process/technology values for the large-signal DC model. These are universal
// (one set per analysis), set from the toolbar's "DC settings" dialog, and may
// be any continuous value (not locked to the component editor's 1/3/10 steps).
struct TechParams {
    double vthn = 0.6;   // NMOS threshold voltage (V)
    double vthp = 0.6;   // PMOS threshold voltage magnitude (V)
    double is = 1e-16;   // diode/BJT saturation current (A), for future DC of
                         // diodes and BJTs (unused by the MOSFET model)

    // Supply-rail voltage for every VDD symbol. The rail is a single anonymous
    // net, so its value belongs to the analysis (set here), not to an
    // individual symbol on the schematic.
    double vdd = 5.0;

    DcMode dc_mode = DcMode::GmOverId;

    // Square-law process values (Mode 2). uN*Cox / uP*Cox, in A/V^2.
    double uncox = 200e-6;
    double upcox = 100e-6;

    // Mode 2: trust that every device is in saturation (fast, symbolic). When
    // off, a numerical pre-solve decides each device's region and triode
    // devices are reported with the triode relation.
    bool assume_saturation = true;

    // Include channel-length modulation, Id = 1/2*uCox*(W/L)*(Vgs-Vth)^2 *
    // (1 + lambda*Vds). Mode 1 substitutes lambda = 1/(ro*Id) (so the result is
    // in ro); mode 2 carries `lambda` as a process symbol (like Vth).
    bool include_clm = false;
    // A rough channel-length-modulation coefficient used by mode 2 (not user
    // settable -- it stands in for the process value).
    double lambda = 0.05;
};

struct AnalysisRequest {
    std::string input_ref;  // which ideal source drives the circuit
    std::string output;     // "V(node)" or "I(ref)"
    // Nodes the caller will explicitly probe (e.g. the output node, or every
    // node for a DC sweep). A node in this set must stay a real MNA unknown --
    // never an internal node of a folded series group -- so `V(node)` resolves.
    // Empty means "only the output node is used".
    std::set<std::string> used_nodes;
    // Process values for the large-signal DC model (Vth, Is). Only used when
    // the DC analysis runs.
    TechParams tech;
    double f0_hz = 1000.0;  // frequency the low-entropy form is tuned to
    // Pruning thresholds as *ratios* (10 = "10x and above" = 20 dB):
    // component_threshold_ratio collapses series/parallel pairs;
    // pole_zero_threshold_ratio drops poles/zeros beyond a multiple of the
    // reference frequency. 0 falls back to the legacy dB fields below.
    double component_threshold_ratio = 10.0;
    double pole_zero_threshold_ratio = 1000.0;
    // Deprecated dB forms (kept for compatibility).
    double threshold_db = 20.0; // series/parallel component reduction (dB)
    double pole_zero_threshold_db = 60.0; // pole/zero frequency reduction (dB)
    // Pole/zero reference: the dominant pole/zero, or the unity-gain bandwidth.
    enum class PoleRef { Dominant, UgBw };
    PoleRef pole_ref = PoleRef::Dominant;
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

// Every unknown of the large-signal DC system, solved symbolically: the node
// voltages and, for each MOSFET, its operating drain current Id. Used by the
// DC analysis, which needs the whole operating point at once.
struct DcSolution {
    ParamTable params;
    std::map<std::string, GiNaC::ex> node_v;  // node -> V(node)
    std::map<std::string, GiNaC::ex> id;      // MOSFET ref -> Id
    std::vector<std::string> mosfets;         // MOSFET refs, circuit order
    std::map<std::string, GiNaC::ex> vgs;     // MOSFET ref -> Vgs
    std::map<std::string, GiNaC::ex> vds;     // MOSFET ref -> Vds
    std::map<std::string, GiNaC::ex> vov;     // MOSFET ref -> Vov (= 2*Id/gm)
    // Top-down presentation: node voltages expressed linearly in the device
    // current symbols I_<ref> (a second solve with the devices as current
    // sources). `have_td` is false when that system is singular.
    std::map<std::string, GiNaC::ex> node_v_td;
    bool have_td = false;
    bool td_used_ro = false; // the Norton (1/ro) fallback was needed
};
DcSolution solve_dc(const Circuit& c, const TechParams& tech = TechParams{});

// Throws std::runtime_error with a user-facing message.
Solved solve(const Circuit& c, const AnalysisRequest& req);

} // namespace syms
