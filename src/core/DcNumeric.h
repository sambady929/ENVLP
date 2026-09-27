#pragma once
#include "core/Netlist.h"
#include "core/SpiceModel.h"

#include <map>
#include <string>
#include <vector>

namespace syms {

// One MOSFET's numeric operating point from the DC solve.
struct MosOpPoint {
    std::string ref;
    double vgs = 0.0, vds = 0.0, vov = 0.0, id = 0.0;
    double gm = 0.0, gds = 0.0;   // small-signal transconductance / output cond.
    bool saturated = false;
    bool in_triode = false;
};

struct NumericDcResult {
    std::map<std::string, double> node_v;   // node -> volts
    std::vector<MosOpPoint> mos;            // one per MOSFET, circuit order
    bool ok = false;
    std::string error;                      // set when !ok
};

// Solve the large-signal DC operating point numerically with Newton iteration.
// Every independent source uses its DC value; capacitors open, inductors short.
// Each MOSFET uses the given level-1/-3 model with its per-device W/L. Throws
// nothing -- errors are reported in the result.
NumericDcResult solve_dc_numeric(const Circuit& c, const MosModel& nmos_model,
                                 const MosModel& pmos_model);

} // namespace syms
