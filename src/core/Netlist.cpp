#include "core/Netlist.h"
#include "core/Eng.h"

#include <set>
#include <stdexcept>

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
        case Kind::OTA: return "OTA";
        case Kind::GM: return "GM";
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
        case Kind::E: return "VCVS";
        case Kind::G: return "VCCS";
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
        case Kind::OTA: return "OTA";
        case Kind::GM: return "Gm";
        case Kind::FDOPAMP: return "Fully differential op-amp";
        case Kind::AMP: return "Amplifier";
        case Kind::IS: return "1/s block";
        case Kind::SBLK: return "s block";
        case Kind::CCCS: return "CCCS";
        case Kind::CCVS: return "CCVS";
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
        case Kind::OTA: return "U";
        case Kind::GM: return "A";
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
        Kind::OPAMP, Kind::OTA, Kind::FDOPAMP, Kind::AMP, Kind::GM,
        Kind::IS, Kind::SBLK,
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
        case Kind::GM:
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
        case Kind::OPAMP: return 3;
        case Kind::OTA: return 3;
        case Kind::NULLOR: return 4;
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
        case Kind::OTA:
        case Kind::AMP:
        case Kind::GM:
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
        case Kind::NULLOR: return {"in+", "in-", "out+", "out-"};
        case Kind::OPAMP: return {"in+", "in-", "out"};
        case Kind::OTA: return {"in+", "in-", "out"};
        case Kind::FDOPAMP: return {"in+", "in-", "out+", "out-"};        case Kind::AMP:
        case Kind::GM:
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
        {"ro", "100k", true, true, "Ohm",
         "output resistance (channel-length modulation); also the DC bias rds"},
        {"W", "10u", false, true, "m", "gate width (numeric DC mode)"},
        {"L", "1u", false, true, "m", "gate length (numeric DC mode)"},
        {"Cgs", "100f", true, false, "F", "gate-source capacitance"},
        {"Cgd", "20f", true, false, "F", "gate-drain (Miller) capacitance"},
        // Cds defaults to the same value as Cgd (see param_estimate); listed
        // right after Cgd so the editor reads Cgs / Cgd / Cds together.
        {"Cds", "20f", true, false, "F", "drain-source capacitance"},
        {"Cdb", "20f", true, false, "F", "drain junction capacitance"},
        {"Csb", "20f", true, false, "F", "source junction capacitance"},
        {"fcn", "10k", true, true, "Hz",
         "1/f noise corner frequency (flicker = thermal below this)"},
    };
    static const std::vector<ParamDef> pmos = {
        {"gm", "1m", false, true, "S", "transconductance"},
        {"ro", "100k", true, true, "Ohm",
         "output resistance (channel-length modulation); also the DC bias rds"},
        {"W", "10u", false, true, "m", "gate width (numeric DC mode)"},
        {"L", "1u", false, true, "m", "gate length (numeric DC mode)"},
        {"Cgs", "100f", true, false, "F", "gate-source capacitance"},
        {"Cgd", "20f", true, false, "F", "gate-drain (Miller) capacitance"},
        {"Cds", "20f", true, false, "F", "drain-source capacitance"},
        {"Cdb", "20f", true, false, "F", "drain junction capacitance"},
        {"Csb", "20f", true, false, "F", "source junction capacitance"},
        {"fcn", "10k", true, true, "Hz",
         "1/f noise corner frequency (flicker = thermal below this)"},
    };
    static const std::vector<ParamDef> npn = {
        {"gm", "40m", false, true, "S", "forward transconductance"},
        {"rpi", "2.5k", true, true, "Ohm",
         "base-emitter input resistance (= beta/gm)"},
        {"ro", "50k", true, true, "Ohm", "output resistance (Early effect)"},
        {"Cpi", "10p", true, false, "F", "base-emitter capacitance"},
        {"Cmu", "1p", true, false, "F", "base-collector (Miller) capacitance"},
        {"fcn", "1k", true, true, "Hz",
         "1/f noise corner frequency (flicker = thermal below this)"},
    };
    static const std::vector<ParamDef> pnp = npn;
    static const std::vector<ParamDef> diode = {
        {"rd", "1k", true, true, "Ohm",
         "small-signal (dynamic) resistance rd = n*VT/Id"},
        {"Cd", "10p", true, true, "F", "junction capacitance"},
        {"fcn", "1k", true, false, "Hz",
         "1/f noise corner frequency (flicker = thermal below this)"},
    };
    // Passive parasitics: all off by default so a component is ideal unless
    // the user asks otherwise. R: series lead inductance + parallel stray
    // capacitance. C: ESR + ESL in series, leakage in parallel. L: winding
    // resistance in series, self-capacitance / core loss in parallel.
    static const std::vector<ParamDef> resistor = {
        {"Cpar", "1p", true, false, "F", "parallel (stray) capacitance"},
        {"Lser", "1n", true, false, "H", "series lead inductance"},
    };
    static const std::vector<ParamDef> capacitor = {
        {"Rser", "1", true, false, "Ohm", "equivalent series resistance (ESR)"},
        {"Lser", "1n", true, false, "H", "equivalent series inductance (ESL)"},
        {"Rpar", "1G", true, false, "Ohm", "leakage (parallel) resistance"},
    };
    static const std::vector<ParamDef> inductor = {
        {"Rser", "1", true, false, "Ohm", "winding (DC) resistance"},
        {"Cpar", "1p", true, false, "F", "self (parallel) capacitance"},
        {"Rpar", "1G", true, false, "Ohm", "core-loss (parallel) resistance"},
    };
    // Transformer: the two winding inductances and the coupling coefficient,
    // plus (off by default) the winding resistances and the inter-winding
    // capacitance.
    static const std::vector<ParamDef> transformer = {
        {"Lp", "1m", false, true, "H", "primary inductance"},
        {"Ls", "4m", false, true, "H",
         "secondary inductance (turns ratio n = sqrt(Ls/Lp))"},
        {"k", "0.99", false, true, "", "coupling coefficient"},
        {"Rp", "1", true, false, "Ohm", "primary winding resistance"},
        {"Rs", "1", true, false, "Ohm", "secondary winding resistance"},
        {"Cps", "1p", true, false, "F", "inter-winding capacitance"},
    };
    // Op-amps / gain blocks: the DC gain lives in value_text (edited as
    // "Gain"); GBW is the gain-bandwidth product (Hz) that sets the single
    // dominant pole at w0 = 2*pi*GBW/gain. It is optional: switch it off for
    // an ideal (infinite-bandwidth) amplifier.
    //
    // The amplifier's own input and output impedance and capacitance are
    // parasitics: they are off by default, so an amplifier is ideal until the
    // user asks for the loading of the real thing.
    static const std::vector<ParamDef> opamp = {
        {"GBW", "1M", true, true, "Hz", "gain-bandwidth product"},
        // input-referred noise voltage density (V/sqrt(Hz)); the optional
        // white+1/f pair lets a noise analysis form the corner frequency.
        {"en", "10n", true, true, "V", "input voltage noise density"},
        {"en_flicker", "100n", true, false, "V",
         "1/f input voltage noise density at 1 Hz"},
        {"Rin", "1G", true, false, "Ohm", "input resistance"},
        {"Rout", "100", true, false, "Ohm", "output resistance"},
        {"Cin", "1p", true, false, "F", "input capacitance"},
        {"Cout", "1p", true, false, "F", "output capacitance"},
    };

    switch (k) {
        case Kind::NMOS: return nmos;
        case Kind::PMOS: return pmos;
        case Kind::NPN: return npn;
        case Kind::PNP: return pnp;
        case Kind::D: return diode;
        case Kind::K: return coupling;
        case Kind::R: return resistor;
        case Kind::C: return capacitor;
        case Kind::L: return inductor;
        case Kind::T: return transformer;
        case Kind::OPAMP:
        case Kind::OTA:
        case Kind::FDOPAMP:
        case Kind::AMP:
        case Kind::GM: return opamp;
        default: return none;
    }
}

UnitClass param_unit_class(Kind k, const std::string& p) {
    (void)k;
    if (p == "gm" || p == "gmb") return UnitClass::Siemens;
    if (p == "ro" || p == "rpi" || p == "rd" || p == "Rser" || p == "Rpar" ||
        p == "Rp" || p == "Rs")
        return UnitClass::Ohm;
    if (p == "Cgs" || p == "Cgd" || p == "Cds" || p == "Cdb" || p == "Csb" ||
        p == "Cpi" || p == "Cmu" || p == "Cd" || p == "Cpar" || p == "Cps")
        return UnitClass::Farad;
    if (p == "Lser" || p == "Lp" || p == "Ls") return UnitClass::Henry;
    return UnitClass::Plain;
}

UnitClass value_unit_class(Kind k) {
    switch (k) {
        case Kind::R: return UnitClass::Ohm;
        case Kind::C: return UnitClass::Farad;
        case Kind::L: return UnitClass::Henry;
        case Kind::G: return UnitClass::Siemens;
        case Kind::OTA:
        case Kind::GM: return UnitClass::Siemens;
        case Kind::V:
        case Kind::VDD: return UnitClass::Volt;
        // T, K, and the controlled blocks carry gains/ratios: keep them Plain
        // so they are not mistaken for R/C/L time-constant candidates.
        default: return UnitClass::Plain;
    }
}

// Which kinds actually use `value_text`. Everything else is described by its
// model parameters (transistors, diodes, transformers, coupling) or by its
// topology (nullor, ground/supply), so a generic value row would be meaningless
// there. The ideal s / 1/s blocks are the exception: their one value is the
// coefficient that sets their 0 dB crossing.
bool has_value(Kind k) {
    switch (k) {
        case Kind::R:
        case Kind::C:
        case Kind::L:
        case Kind::OPAMP:
        case Kind::OTA:
        case Kind::FDOPAMP:
        case Kind::AMP:
        case Kind::GM:
        case Kind::E:
        case Kind::G:
        case Kind::CCCS:
        case Kind::CCVS:
        // The ideal s and 1/s blocks carry one dimensionless/time coefficient:
        // SBLK: i = K*s*(v(in)-v(out)); IS: v(in)-v(out) = K*s*i. The value
        // sets their 0 dB crossing, and an empty value means K = 1.
        case Kind::IS:
        case Kind::SBLK:
            return true;
        default:
            return false;
    }
}

std::string value_label(Kind k) {
    switch (k) {
        case Kind::OPAMP:
        case Kind::FDOPAMP:
        case Kind::AMP:
        case Kind::E:
        case Kind::G:
        case Kind::CCCS:
        case Kind::CCVS:
            return "Gain";
        case Kind::OTA:
        case Kind::GM:
            return "gm";
        default:
            return "Value";
    }
}

const char* value_unit(Kind k) {
    switch (k) {
        case Kind::R: return "\xCE\xA9"; // ohm
        case Kind::C: return "F";
        case Kind::L: return "H";
        case Kind::G:
        case Kind::OTA:
        case Kind::GM: return "S";
        default: return "";
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
    // A mirror copy uses the materialised (scaled) override when present.
    auto ov = param_override.find(p);
    if (ov != param_override.end()) return ov->second;
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

// A passives-with-a-single-value component whose value can be copied/scaled:
// a resistor, capacitor, inductor, or the gain of a controlled/gain block.
// (A source has a DC/AC value pair and no single thing to scale; GND/VDD are
// rails.)
bool is_copyable(Kind k) {
    switch (k) {
    case Kind::R:
    case Kind::C:
    case Kind::L:
    case Kind::D:
    case Kind::OPAMP:
    case Kind::FDOPAMP:
    case Kind::AMP:
    case Kind::E:
    case Kind::G:
    // Independent sources copy too, so e.g. I2 can be a copy of I1 and the two
    // stay in a fixed ratio (a current mirror's reference/branch currents).
    case Kind::V:
    case Kind::I:
        return true;
    default:
        return false;
    }
}

bool can_mirror(const Component& unit, const Component& copy) {
    // Devices mirror (a MOSFET's model parameters all scale with the copy
    // count); a single-value passive or gain block also copies, and its one
    // value scales. The kinds must match exactly: an NMOS cannot copy a PMOS,
    // an NPN cannot copy a PNP, a resistor cannot copy a capacitor.
    if (!is_device(unit.kind) && !is_copyable(unit.kind)) return false;
    if (unit.kind != copy.kind) return false;
    // A copy of a copy is allowed (chains are flattened in resolve_mirrors), so
    // the unit may itself be a copy -- only a direct cycle is rejected.
    return unit.ref != copy.ref;
}

double scale_mirror_estimate(UnitClass uc, double unit_value, double ratio) {
    if (!(ratio > 0.0)) ratio = 1.0;
    switch (uc) {
        // transconductance and capacitance grow with the number of devices
        case UnitClass::Siemens:
        case UnitClass::Farad:
            return unit_value * ratio;
        // a resistance formed by `ratio` unit devices is divided (parallel for a
        // current source / series for a voltage), so it scales inversely
        case UnitClass::Ohm:
            return unit_value / ratio;
        // A gain block / controlled source: the copy's gain is the unit's times
        // the copy count (so an inverting amp's Rf = m*Rin gives a gain of -m).
        default:
            return unit_value * ratio;
    }
}

void resolve_mirrors(Circuit& c) {
    // Clear stale overrides so repeated calls are idempotent.
    for (auto& comp : c.comps) comp.param_override.clear();

    // Flatten copy chains first: if A copies B and B copies C, A becomes a copy
    // of C with the product ratio (so A's model collapses all the way to C).
    for (auto& copy : c.comps) {
        if (copy.mirror_ref.empty()) continue;
        long nm = copy.mirror_num(), dn = copy.mirror_dn();
        std::string cur = copy.mirror_ref;
        std::set<std::string> seen;
        seen.insert(copy.ref);
        while (true) {
            if (!seen.insert(cur).second)
                throw std::runtime_error(
                    "mirror: cycle through '" + cur + "'");
            const Component* u = c.find(cur);
            if (!u)
                throw std::runtime_error("mirror: unit device '" + cur +
                                         "' for '" + copy.ref + "' not found");
            if (u->mirror_ref.empty()) break;
            nm *= u->mirror_num();
            dn *= u->mirror_dn();
            cur = u->mirror_ref;
        }
        copy.mirror_ref = cur;
        // A long copy chain multiplies the counts, which can overflow to 0 or a
        // negative value. Clamp here so the *stored* fields are always a usable
        // ratio -- the accessors guard too, but the raw field is what the copy
        // control edits, what gets serialized, and what a division would use.
        copy.mirror_mult = (nm > 0 && nm <= 1000000L) ? int(nm) : 1;
        copy.mirror_den = (dn > 0 && dn <= 1000000L) ? int(dn) : 1;
    }

    for (auto& copy : c.comps) {
        if (copy.mirror_ref.empty()) continue;
        const Component* unit = c.find(copy.mirror_ref);
        if (!unit)
            throw std::runtime_error("mirror: unit device '" + copy.mirror_ref +
                                     "' for '" + copy.ref + "' not found");
        if (unit->mirror_ref == copy.ref)
            throw std::runtime_error("mirror: cycle between '" + copy.ref +
                                     "' and '" + copy.mirror_ref + "'");
        if (!can_mirror(*unit, copy))
            throw std::runtime_error(
                "mirror: '" + copy.ref + "' (" + kind_display(copy.kind) +
                ") cannot copy '" + copy.mirror_ref + "' (" +
                kind_display(unit->kind) + ")");
        const int rnum = copy.mirror_num(), rden = copy.mirror_dn();
        const double ratio = (rnum > 0 && rden > 0) ? double(rnum) / double(rden)
                                                    : 1.0;
        if (is_copyable(copy.kind)) {
            // An independent source copies its DC and AC values scaled by the
            // copy ratio (I2 = 2*I1), so the two stay in a fixed ratio.
            if (copy.kind == Kind::V || copy.kind == Kind::I) {
                auto scale_text = [&](const std::string& txt) {
                    double v = 0.0;
                    if (!eng::parse_value(txt, v)) v = 0.0;
                    return eng::format_eng(v * ratio, 4);
                };
                copy.dc_text = scale_text(unit->dc_text);
                copy.ac_text = scale_text(unit->ac_text);
                continue;
            }
            // A single-value copy scales its *value*: a copy is a *series*
            // multiple of the unit, so Rf = r*Rin, Cf = Cin/r, Lf = r*Lin, and
            // an amplifier gain is r*A. (Devices use the parallel rule above,
            // where the copy is r unit devices.)
            if (!unit->value_text.empty()) {
                double uv = unit->estimate();
                UnitClass uc = value_unit_class(copy.kind);
                double sv;
                switch (uc) {
                    case UnitClass::Ohm:
                    case UnitClass::Henry:
                    case UnitClass::Plain: sv = uv * ratio; break;
                    case UnitClass::Farad: sv = uv / ratio; break;
                    default: sv = uv * ratio; break;
                }
                copy.value_text = eng::format_eng(sv, 4);
            }
            continue;
        }
        for (const auto& d : param_defs(copy.kind)) {
            double uv = unit->param_estimate(d.name);
            copy.param_override[d.name] =
                scale_mirror_estimate(param_unit_class(copy.kind, d.name), uv,
                                      ratio);
            // carry the unit's enable state so the copy stamps the same model
            copy.param_on[d.name] = unit->param_enabled(d.name);
        }
    }
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
    // Ground and supply symbols are anonymous: GND and VDD keep a numbered
    // reference ("GND1", "VDD1") so several of them stay addressable in the
    // schematic (placements/links are keyed by reference). They are still
    // electrically one net each -- VDD is a single rail.
    std::string prefix = ref_prefix(k);
    int n = 1;
    while (c.find(prefix + std::to_string(n))) ++n;
    return prefix + std::to_string(n);
}

} // namespace syms
