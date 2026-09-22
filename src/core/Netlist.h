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
//   NMOS/PMOS: 4 pins [D, G, S, B]
//   NPN/PNP : 3 pins  [C, B, E]      (optional internal node when rb is on)
//   GND     : 1 pin   always node "0"
//   VDD     : 1 pin   always net "VDD" (supply; PSR/PSRR excitation)
//   D       : 2 pins  [A, K]         diode small-signal equivalent
//   T       : 4 pins  [p+, p-, s+, s-]  transformer (two coupled windings)
//   K       : 0 pins  inductor coupling; `links` names the two inductors
//   NULLOR  : 3 pins  [in+, in-, out]   ideal op-amp (infinite gain)
//   OPAMP   : 3 pins  [in+, in-, out]   finite-gain VCVS (value = A)
//   FDOPAMP : 4 pins  [in+, in-, out+, out-]  fully differential op-amp
//   AMP     : 2 pins  [in, out]      gain block, inverting side grounded
//   IS      : 2 pins  [in, out]      ideal 1/s block  (integrator)
//   SBLK    : 2 pins  [in, out]      ideal s block    (differentiator)
// ---------------------------------------------------------------------------
enum class Kind {
    R, C, L, V, I, E, G, NMOS, PMOS, NPN, PNP, GND,
    VDD, D, T, K, NULLOR, OPAMP, FDOPAMP, AMP, IS, SBLK
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
inline bool is_independent_source(Kind k) { return k == Kind::V || k == Kind::I; }
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
    std::map<std::string, bool> param_on;          // parasitic toggles
    std::map<std::string, std::string> param_text; // per-parameter estimate
    std::map<std::string, int> param_db;           // per-parameter 10 dB offset
    std::vector<std::string> links;      // K: the two coupled inductors

    bool param_enabled(const std::string& p) const;
    double estimate() const;                       // SI value for pruning
    double param_estimate(const std::string& p) const;
    std::string value_symbol() const { return ref; } // main value symbol name
};

// Symbol name for a device parameter: "gm_M1", "Cgd_M2", ...
std::string param_symbol(const Component& c, const std::string& p);

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

} // namespace syms
