#pragma once
#include <map>
#include <string>
#include <vector>

namespace syms {

// ---------------------------------------------------------------------------
// Component kinds and their pin/node conventions
// ---------------------------------------------------------------------------
//   R, C, L : 2 pins  [a, b]
//   V       : 2 pins  [+ , -]        (branch current unknown)
//   I       : 2 pins  [head, tail]   (current exits head, flows through the
//                                     external circuit, re-enters at tail)
//   E (VCVS): 4 pins  [out+, out-, ctrl+, ctrl-]   (branch current unknown)
//   G (VCCS): 4 pins  [out+, out-, ctrl+, ctrl-]
//   NMOS/PMOS: 3 pins [D, G, S]      (no body terminal)
//   NPN/PNP : 3 pins  [C, B, E]      (optional internal node when rb is on)
//   GND     : 1 pin   always node "0"
//   VDD     : 1 pin   always net "VDD" (supply; PSR/PSRR excitation)
//   D       : 2 pins  [A, K]         diode small-signal equivalent
//   T       : 4 pins  [p+, p-, s+, s-]  transformer (two coupled windings)
//   K       : 0 pins  inductor coupling; `links` names the two inductors
//   NULLOR  : 4 pins  [in+, in-, out+, out-]  ideal two-port (nullator input
//             port shunted to a norator output port: infinite gain)
//   OPAMP   : 3 pins  [in+, in-, out]   finite-gain VCVS (value = A)
//   FDOPAMP : 4 pins  [in+, in-, out+, out-]  fully differential op-amp
//   AMP     : 2 pins  [in, out]      gain block, inverting side grounded
//   IS      : 2 pins  [in, out]      ideal 1/s block  (integrator)
//   SBLK    : 2 pins  [in, out]      ideal s block    (differentiator)
//   CCCS(F) : 4 pins  [ctrl+, ctrl-, out+, out-]  current-controlled
//   CCVS(H) : 4 pins  [ctrl+, ctrl-, out+, out-]  current-controlled
//             (input port is a short; the control current is sensed there)
// ---------------------------------------------------------------------------
enum class Kind {
    R, C, L, V, I, E, G, NMOS, PMOS, NPN, PNP, GND,
    VDD, D, T, K, NULLOR, OPAMP, FDOPAMP, AMP, IS, SBLK, CCCS, CCVS
};

std::string kind_token(Kind k);   // "R", "NMOS", ...
std::string kind_display(Kind k); // "Resistor", "N-MOSFET", ...
std::string ref_prefix(Kind k);   // "R", "C", "M", "Q", "GND", ...
bool kind_from_token(const std::string& t, Kind& out);

int pin_count(Kind k);
std::vector<std::string> pin_names(Kind k);

// Branch-current unknowns (KVL rows). FDOPAMP and T own two each.
int branch_count(Kind k);
std::vector<std::string> branch_suffixes(Kind k); // {""}, {"p","n"}, ...
std::string branch_key(const std::string& ref, const std::string& suffix);

inline bool is_device(Kind k) {
    return k == Kind::NMOS || k == Kind::PMOS || k == Kind::NPN ||
           k == Kind::PNP || k == Kind::D;
}
inline bool is_independent_source(Kind k) {
    // VDD is now an ideal voltage source (single-pin, supply rail to
    // ground) and can be the analysis excitation just like a V source.
    return k == Kind::V || k == Kind::I || k == Kind::VDD;
}
inline bool is_ground(Kind k) { return k == Kind::GND; }
inline bool is_supply(Kind k) { return k == Kind::VDD; }
inline bool has_branch_current(Kind k) { return branch_count(k) > 0; }

// ---------------------------------------------------------------------------
// Model parameters (transistor parasitics, etc.)
// ---------------------------------------------------------------------------
struct ParamDef {
    std::string name;        // key: "gm", "ro", "Cgs", ...
    std::string default_text; // default order-of-magnitude estimate
    bool parasitic;           // true => can be switched on/off by the user
    bool default_on;
    std::string unit;         // "S", "Ohm", "F" (ASCII, display only)
    std::string desc;
};

const std::vector<ParamDef>& param_defs(Kind k);

enum class UnitClass { Ohm, Farad, Henry, Siemens, Volt, Plain };
UnitClass param_unit_class(Kind k, const std::string& param);
UnitClass value_unit_class(Kind k);

// ---------------------------------------------------------------------------
// Component
// ---------------------------------------------------------------------------
struct Component {
    std::string ref;                     // "R1", "M2", ...
    Kind kind = Kind::R;
    std::vector<std::string> nodes;      // size == pin_count(kind)
    std::string value_text;              // "10k", "4.7u"; "" => symbolic/unity
    int size_db = 0;                     // order-of-magnitude offset, 10 dB steps
    // Independent V/I sources carry a DC value and an AC value, used by the
    // large-signal DC analysis and the (superposition) AC analysis respectively.
    // Defaults: DC = 0, AC = 1 (so a plain source is a standard AC stimulus and
    // a DC short/open). Ignored for non-source components.
    std::string dc_text = "0";
    std::string ac_text = "1";
    std::map<std::string, bool> param_on;          // parasitic toggles
    std::map<std::string, std::string> param_text; // per-parameter estimate
    std::map<std::string, int> param_db;           // per-parameter 10 dB offset
    std::vector<std::string> links;      // K: the two coupled inductors
    // Device multiplicity / mirroring (MOSFETs and BJTs only):
    //   mirror_ref: the ref of the unit device this one is a copy of ("" = it
    //               is its own unit device). The copy follows the unit's model
    //               parameters, scaled by `mirror_mult`.
    //   mirror_mult: how many copies of the unit device this is (>= 1). A
    //               device with mult = m has gm, Cgs, ... scaled by m.
    std::string mirror_ref;
    int mirror_mult = 1;
    // Per-parameter numeric overrides materialised when the circuit's mirrors
    // are resolved (a copy's parameters are the unit's, scaled).
    std::map<std::string, double> param_override;

    bool param_enabled(const std::string& p) const;
    double estimate() const;                       // SI value for pruning
    double param_estimate(const std::string& p) const;
    std::string value_symbol() const { return ref; } // main value symbol name

    // Effective multiplicity of this device (1 unless it mirrors another).
    int multiplicity() const { return mirror_ref.empty() || mirror_mult < 1
                                          ? 1
                                          : mirror_mult; }
};

// Symbol name for a device parameter: "gm_M1", "Cgd_M2", ...
std::string param_symbol(const Component& c, const std::string& p);

// Whether `copy` may mirror `unit`: both must be the same kind, and both must
// be a mirrorable device (MOSFET or BJT) or a single-value copyable passive /
// gain block (R, C, L, D, op-amp, amp, controlled source).
bool can_mirror(const Component& unit, const Component& copy);

// Whether a component kind's single value can be copied/scaled (a resistor,
// capacitor, inductor, gain block, ...). Devices mirror their model parameters
// instead, so they are excluded here.
bool is_copyable(Kind k);

// Scale a mirrored parameter's estimate for `mult` copies of the unit device.
// Transconductances and capacitances scale with the copy count; resistances
// scale inversely (m parallel/series unit devices divide the resistance).
double scale_mirror_estimate(UnitClass uc, double unit_value, int mult);



// Hidden internal node name used when a BJT's rb is enabled.
std::string bjt_internal_node(const Component& c);

// ---------------------------------------------------------------------------
// Circuit
// ---------------------------------------------------------------------------
struct Circuit {
    std::vector<Component> comps;

    const Component* find(const std::string& ref) const;

    // Structural validation. Returns false and fills `err` on problems:
    // duplicate refs, wrong pin counts, empty node names, missing ground.
    bool validate(std::string& err) const;
};

// Generate the next free reference for a kind: "R1", "R2", "M1", ...
std::string next_ref(const Circuit& c, Kind k);

// Materialise every mirror copy's parameters from its unit device, scaled by
// the copy count, into `param_override`. Run once after loading a circuit (and
// before analysis) so MNA and the pruner see consistent values. Also propagates
// the unit's parasitic toggles. Throws std::runtime_error on an invalid mirror
// (mismatched kinds) or a missing/cyclic unit.
void resolve_mirrors(Circuit& c);

} // namespace syms
