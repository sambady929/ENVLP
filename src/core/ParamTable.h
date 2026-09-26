#pragma once
#include <ginac/ginac.h>

// CLN (GiNaC's bignum backend) defines a function-like macro As() that
// corrupts wxWidgets' wxAny::As<T>(); undo it as soon as GiNaC is in.
#ifdef As
#undef As
#endif

#include "Netlist.h"
#include <map>
#include <string>

namespace syms {

// UnitClass (defined in Netlist.h, next to the functions producing it)
// classifies a symbol's physical unit -- used by the low-entropy pruner
// to build physically meaningful time-constant candidates
// (R*C, L/R, 1/(gm*C), ...) for pole/zero labeling.

// Registry of every symbol that appears in the MNA matrices, its numeric
// estimate (the user's order-of-magnitude guess) and its unit class.
struct ParamTable {
    std::map<std::string, double> est;    // symbol name -> SI estimate
    std::map<std::string, UnitClass> cls; // symbol name -> unit class
    std::map<std::string, GiNaC::symbol> syms;

    GiNaC::ex get(const std::string& name) {
        auto it = syms.find(name);
        if (it != syms.end()) return it->second;
        GiNaC::symbol s = intern(name);
        syms.emplace(name, s);
        return s;
    }

    // Symbols are interned process-wide: two analyses mentioning "gm_M1"
    // must share one GiNaC symbol object, otherwise expressions from
    // different runs are algebraically incomparable (and cross-analysis
    // results such as PSRR = H_sig / H_sup cannot be formed).
    static GiNaC::symbol intern(const std::string& name) {
        static std::map<std::string, GiNaC::symbol> registry;
        auto it = registry.find(name);
        if (it != registry.end()) return it->second;
        return registry.emplace(name, GiNaC::symbol(name)).first->second;
    }

    void set(const std::string& name, double estimate, UnitClass c) {
        est[name] = estimate;
        cls[name] = c;
        get(name); // make sure the symbol exists too
    }

    // Substitute every registered symbol with its estimate (and s if given).
    GiNaC::ex eval_real(const GiNaC::ex& e) const {
        GiNaC::exmap m;
        for (const auto& kv : est) {
            auto it = syms.find(kv.first);
            if (it != syms.end()) m[it->second] = GiNaC::ex(kv.second);
        }
        GiNaC::ex r = e.subs(m);
        // Fold floating-point constants (e.g. Pi) so callers that test
        // is_a<numeric> see a number; symbolic variables are unaffected.
        return r.evalf();
    }
};

} // namespace syms
