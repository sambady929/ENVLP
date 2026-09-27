#pragma once
#include <map>
#include <string>
#include <vector>

namespace syms {

// A parsed SPICE MOSFET model card (.model NAME NMOS/PMOS LEVEL=n ...). Only the
// parameters that participate in the square-law (level 1) and semi-empirical
// (level 3) drain-current equations, plus the overlap/junction capacitances the
// small-signal override needs, are kept.
struct MosModel {
    std::string name;          // model card name, e.g. "nmos_1p8"
    bool pmos = false;         // PMOS vs NMOS
    int level = 1;             // 1 or 3
    double vto = 0.5;          // VTO, threshold (signed as written)
    double kp = 200e-6;        // KP = u*Cox (A/V^2)
    double lambda = 0.0;       // channel-length modulation (1/V)
    double gamma = 0.0;        // body effect (V^0.5)
    double phi = 0.7;          // surface potential (V)
    // Level 3 extras (0 when absent).
    double theta = 0.0;        // mobility degradation
    double eta = 0.0;          // static feedback / DIBL
    double vmax = 0.0;         // maximum carrier velocity (m/s)
    // Capacitances used by the override (per unit width / area / perimeter).
    double cgso = 0.0;         // gate-source overlap cap per metre width (F/m)
    double cgdo = 0.0;         // gate-drain overlap cap per metre width (F/m)
    double cj = 0.0;           // bottom junction cap per area (F/m^2)
    double cjsw = 0.0;         // sidewall junction cap per perimeter (F/m)
    double tox = 0.0;          // oxide thickness (m), if given
};

// Parse a SPICE model file: returns every .model card found (both NMOS and
// PMOS). Lines beginning with '*' are comments; a '.model' card may span
// continuation lines starting with '+'. Unknown parameters are ignored.
std::vector<MosModel> parse_spice_models(const std::string& path,
                                         std::string& err);

// Pick the model named `name` from `models` (case-insensitive); returns nullptr
// when absent.
const MosModel* find_model(const std::vector<MosModel>& models,
                           const std::string& name);

} // namespace syms
