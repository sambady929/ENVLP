#include "core/Netlist.h"
#include "core/Eng.h"

#include <set>

namespace syms {

std::string kind_token(Kind k) {
    switch (k) {
        case Kind::R: return "R";
        case Kind::C: return "C";
        case Kind::L: return "L";
        case Kind::V: return "V";
        case Kind::I: return "I";
        case Kind::E: return "E";
        case Kind::G: return "G";
        case Kind::NMOS: return "NMOS";
        case Kind::PMOS: return "PMOS";
        case Kind::NPN: return "NPN";
        case Kind::PNP: return "PNP";
        case Kind::GND: return "GND";
        case Kind::VDD: return "VDD";
        case Kind::D: return "D";
        case Kind::T: return "T";
        case Kind::K: return "K";
        case Kind::NULLOR: return "NULLOR";
        case Kind::OPAMP: return "OPAMP";
        case Kind::FDOPAMP: return "FDOPAMP";
        case Kind::AMP: return "AMP";
        case Kind::IS: return "IS";
        case Kind::SBLK: return "SBLK";
        case Kind::CCCS: return "CCCS";
        case Kind::CCVS: return "CCVS";
    }
    return "?";
}

std::string kind_display(Kind k) {
    switch (k) {
        case Kind::R: return "Resistor";
        case Kind::C: return "Capacitor";
        case Kind::L: return "Inductor";
        case Kind::V: return "Voltage source";
        case Kind::I: return "Current source";
        case Kind::E: return "VCVS (E)";
        case Kind::G: return "VCCS (G)";
        case Kind::NMOS: return "N-MOSFET";
        case Kind::PMOS: return "P-MOSFET";
        case Kind::NPN: return "NPN BJT";
        case Kind::PNP: return "PNP BJT";
        case Kind::GND: return "Ground";
        case Kind::VDD: return "Supply rail";
        case Kind::D: return "Diode";
        case Kind::T: return "Transformer";
        case Kind::K: return "Inductor coupling";
        case Kind::NULLOR: return "Nullor";
        case Kind::OPAMP: return "Op-amp";
        case Kind::FDOPAMP: return "Fully differential op-amp";
        case Kind::AMP: return "Amplifier";
        case Kind::IS: return "1/s block";
        case Kind::SBLK: return "s block";
        case Kind::CCCS: return "CCCS (F)";
        case Kind::CCVS: return "CCVS (H)";
    }
    return "?";
}

std::string ref_prefix(Kind k) {
    switch (k) {
        case Kind::R: return "R";
        case Kind::C: return "C";
        case Kind::L: return "L";
        case Kind::V: return "V";
        case Kind::I: return "I";
        case Kind::E: return "E";
        case Kind::G: return "G";
        case Kind::NMOS:
        case Kind::PMOS: return "M";
        case Kind::NPN:
        case Kind::PNP: return "Q";
        case Kind::GND: return "GND";
        case Kind::VDD: return "VDD";
        case Kind::D: return "D";
        case Kind::T: return "T";
        case Kind::K: return "K";
        case Kind::NULLOR: return "X";
        case Kind::OPAMP: return "U";
        case Kind::FDOPAMP: return "U";
        case Kind::AMP: return "A";
        case Kind::IS: return "IS";
        case Kind::SBLK: return "S";
        case Kind::CCCS: return "F";
        case Kind::CCVS: return "H";
    }
    return "X";
}

bool kind_from_token(const std::string& t, Kind& out) {
    static const Kind all[] = {
        Kind::R,    Kind::C,   Kind::L,     Kind::V,  Kind::I,  Kind::E,
        Kind::G,    Kind::NMOS, Kind::PMOS, Kind::NPN, Kind::PNP, Kind::GND,
        Kind::VDD,  Kind::D,   Kind::T,     Kind::K,  Kind::NULLOR,
        Kind::OPAMP, Kind::FDOPAMP, Kind::AMP, Kind::IS, Kind::SBLK,
        Kind::CCCS, Kind::CCVS};
    for (Kind k : all) {
        if (kind_token(k) == t) {
            out = k;
            return true;
        }
    }
    return false;
}

int pin_count(Kind k) {
    switch (k) {
        case Kind::R:
        case Kind::C:
        case Kind::L:
        case Kind::V:
        case Kind::I:
        case Kind::D:
        case Kind::AMP:
        case Kind::IS:
        case Kind::SBLK: return 2;
        case Kind::E:
        case Kind::G:
        case Kind::CCCS:
        case Kind::CCVS: return 4;
        case Kind::NMOS:
        case Kind::PMOS: return 3;
        case Kind::NPN:
        case Kind::PNP: return 3;
        case Kind::GND:
        case Kind::VDD: return 1;
        case Kind::T: return 4;
        case Kind::K: return 0;
        case Kind::NULLOR:
        case Kind::OPAMP: return 3;
        case Kind::FDOPAMP: return 4;
    }
    return 0;
}

int branch_count(Kind k) {
    switch (k) {
        case Kind::V:
        case Kind::VDD:
        case Kind::L:
        case Kind::E:
        case Kind::IS:
        case Kind::SBLK:
        case Kind::OPAMP:
        case Kind::AMP:
        case Kind::NULLOR:
        case Kind::T:
        case Kind::FDOPAMP:
        case Kind::CCCS:
        case Kind::CCVS: return 1;
        default: return 0;
    }
}

std::vector<std::string> branch_suffixes(Kind k) {
    if (k == Kind::T || k == Kind::FDOPAMP) return {"p", "n"};
    return {""};
}

std::string branch_key(const std::string& ref, const std::string& suffix) {
    return suffix.empty() ? ref : ref + ":" + suffix;
}

std::vector<std::string> pin_names(Kind k) {
    switch (k) {
        case Kind::R:
        case Kind::C:
        case Kind::L: return {"a", "b"};
        case Kind::V: return {"+", "-"};
        case Kind::I: return {"head", "tail"};
        case Kind::E:
        case Kind::G: return {"out+", "out-", "ctrl+", "ctrl-"};
        case Kind::NMOS:
        case Kind::PMOS: return {"D", "G", "S"};
        case Kind::NPN:
        case Kind::PNP: return {"C", "B", "E"};
        case Kind::GND: return {"GND"};
        case Kind::VDD: return {"VDD"};
        case Kind::D: return {"A", "K"};
        case Kind::T: return {"p+", "p-", "s+", "s-"};
        case Kind::K: return {};
        case Kind::CCCS:
        case Kind::CCVS: return {"ctrl+", "ctrl-", "out+", "out-"};
        case Kind::NULLOR:
        case Kind::OPAMP: return {"in+", "in-", "out"};
        case Kind::FDOPAMP: return {"in+", "in-", "out+", "out-"};
        case Kind::AMP:
        case Kind::IS:
        case Kind::SBLK: return {"in", "out"};
    }
    return {};
}

const std::vector<ParamDef>& param_defs(Kind k) {
    static const std::vector<ParamDef> none;

    static const std::vector<ParamDef> coupling = {
        {"K", "0.9", false, true, "", "coupling coefficient"},
    };

    static const std::vector<ParamDef> nmos = {
        {"gm", "1m", false, true, "S", "transconductance"},
        {"ro", "100k", true, true, "Ohm", "channel-length-modulation output resistance"},
        {"Cgs", "100f", true, true, "F", "gate-source capacitance"},
        {"Cgd", "20f", true, true, "F", "gate-drain (Miller) capacitance"},
        // Cds defaults to the same value as Cgd (see param_estimate); listed
        // right after Cgd so the editor reads Cgs / Cgd / Cds together.
        {"Cds", "20f", true, true, "F", "drain-source capacitance"},
        {"Cdb", "20f", true, false, "F", "drain junction capacitance"},
        {"Csb", "20f", true, false, "F", "source junction capacitance"},
    };
    static const std::vector<ParamDef> pmos = {
        {"gm", "1m", false, true, "S", "transconductance"},
        {"ro", "100k", true, true, "Ohm", "channel-length-modulation output resistance"},
        {"Cgs", "100f", true, true, "F", "gate-source capacitance"},
        {"Cgd", "20f", true, true, "F", "gate-drain (Miller) capacitance"},
        {"Cds", "20f", true, true, "F", "drain-source capacitance"},
        {"Cdb", "20f", true, false, "F", "drain junction capacitance"},
        {"Csb", "20f", true, false, "F", "source junction capacitance"},
    };
    static const std::vector<ParamDef> npn = {
        {"gm", "40m", false, true, "S", "transconductance"},
        {"rpi", "2.5k", true, true, "Ohm", "base-emitter (input) resistance"},
        {"rb", "100", true, false, "Ohm", "base spreading resistance"},
        {"ro", "50k", true, true, "Ohm", "output resistance (Early effect)"},
        {"Cpi", "10p", true, true, "F", "base-emitter capacitance"},
        {"Cmu", "1p", true, true, "F", "base-collector (Miller) capacitance"},
    };
    static const std::vector<ParamDef> pnp = npn;
    static const std::vector<ParamDef> diode = {
        {"gm", "40m", false, true, "S", "small-signal conductance"},
        {"rd", "1k", true, false, "Ohm", "series/ohmic resistance"},
        {"Cd", "10p", true, true, "F", "junction capacitance"},
    };
    // Op-amps / gain blocks: the DC gain lives in value_text (edited as
    // "Gain"); GBW is the gain-bandwidth product (Hz) that sets the single
    // dominant pole at w0 = 2*pi*GBW/gain.
    static const std::vector<ParamDef> opamp = {
        {"GBW", "1M", false, true, "Hz", "gain-bandwidth product"},
    };

    switch (k) {
        case Kind::NMOS: return nmos;
        case Kind::PMOS: return pmos;
        case Kind::NPN: return npn;
        case Kind::PNP: return pnp;
        case Kind::D: return diode;
        case Kind::K: return coupling;
        case Kind::OPAMP:
        case Kind::FDOPAMP:
        case Kind::AMP: return opamp;
        default: return none;
    }
}

UnitClass param_unit_class(Kind k, const std::string& p) {
    (void)k;
    if (p == "gm" || p == "gmb") return UnitClass::Siemens;
    if (p == "ro" || p == "rpi" || p == "rb" || p == "rd") return UnitClass::Ohm;
    if (p == "Cgs" || p == "Cgd" || p == "Cds" || p == "Cdb" || p == "Csb" ||
        p == "Cpi" || p == "Cmu" || p == "Cd")
        return UnitClass::Farad;
    return UnitClass::Plain;
}

UnitClass value_unit_class(Kind k) {
    switch (k) {
        case Kind::R: return UnitClass::Ohm;
        case Kind::C: return UnitClass::Farad;
        case Kind::L: return UnitClass::Henry;
        case Kind::G: return UnitClass::Siemens;
        case Kind::V:
        case Kind::VDD: return UnitClass::Volt;
        // T, K, and the controlled blocks carry gains/ratios: keep them Plain
        // so they are not mistaken for R/C/L time-constant candidates.
        default: return UnitClass::Plain;
    }
}

bool Component::param_enabled(const std::string& p) const {
    auto it = param_on.find(p);
    if (it != param_on.end()) return it->second;
    for (const auto& d : param_defs(kind)) {
        if (d.name == p) return d.default_on;
    }
    return false;
}

double Component::estimate() const {
    if (kind == Kind::GND) return 0.0;
    double v = 1.0;
    if (!value_text.empty()) {
        if (!eng::parse_value(value_text, v)) v = 1.0; // unparseable => unity
    }
    return v * eng::db_to_factor(size_db);
}

double Component::param_estimate(const std::string& p) const {
    double v = 1.0;
    auto it = param_text.find(p);
    if (it != param_text.end() && !it->second.empty()) {
        if (!eng::parse_value(it->second, v)) v = 1.0;
    } else if (p == "Cds") {
        // Cds follows Cgd unless the user sets it explicitly (matches the
        // usual assumption Cdg == Cds for a symmetric device).
        return param_estimate("Cgd");
    } else {
        for (const auto& d : param_defs(kind)) {
            if (d.name == p) {
                if (!eng::parse_value(d.default_text, v)) v = 1.0;
                break;
            }
        }
    }
    auto dbi = param_db.find(p);
    int db = dbi != param_db.end() ? dbi->second : 0;
    return v * eng::db_to_factor(db);
}

std::string param_symbol(const Component& c, const std::string& p) {
    return p + "_" + c.ref; // gm_M1, Cgd_M2, ...
}

std::string bjt_internal_node(const Component& c) {
    return c.ref + "_bi"; // Q1_bi
}

const Component* Circuit::find(const std::string& ref) const {
    for (const auto& c : comps) {
        if (c.ref == ref) return &c;
    }
    return nullptr;
}

bool Circuit::validate(std::string& err) const {
    std::set<std::string> refs;
    bool has_ground = false;

    for (const auto& c : comps) {
        if (c.ref.empty()) {
            err = "component with empty reference";
            return false;
        }
        // Ground and supply symbols are anonymous: several may share the ref
        // "GND" / "VDD" without being duplicates.
        bool anonymous = (c.kind == Kind::GND || c.kind == Kind::VDD);
        if (!anonymous && !refs.insert(c.ref).second) {
            err = "duplicate reference: " + c.ref;
            return false;
        }
        int pc = pin_count(c.kind);
        if (static_cast<int>(c.nodes.size()) != pc) {
            err = c.ref + ": expected " + std::to_string(pc) + " nodes, got " +
                  std::to_string(c.nodes.size());
            return false;
        }
        for (const auto& n : c.nodes) {
            if (n.empty()) {
                err = c.ref + ": empty node name";
                return false;
            }
        }
        if (c.kind == Kind::GND) has_ground = true;
        // A node literally named "0" or "GND" also counts as ground.
        for (const auto& n : c.nodes) {
            if (n == "0" || n == "GND") has_ground = true;
        }
        if (c.kind == Kind::K && c.links.size() != 2) {
            err = c.ref + ": inductor coupling needs two linked inductors";
            return false;
        }
    }
    if (comps.empty()) {
        err = "circuit is empty";
        return false;
    }
    if (!has_ground) {
        err = "no ground reference (place a Ground symbol or name a node \"0\")";
        return false;
    }
    return true;
}

std::string next_ref(const Circuit& c, Kind k) {
    // VDD is a singleton marker: it always reads "VDD" (there is no
    // "VDD1"). validate() treats it as anonymous so several VDD symbols may
    // share the same reference. GND keeps its numbered form so multiple
    // ground symbols stay addressable in the schematic.
    if (k == Kind::VDD) return "VDD";
    std::string prefix = ref_prefix(k);
    int n = 1;
    while (c.find(prefix + std::to_string(n))) ++n;
    return prefix + std::to_string(n);
}

} // namespace syms
