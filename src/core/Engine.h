#pragma once
#include "core/Netlist.h"
#include "core/Prune.h"
#include "core/Solver.h"
#include <ginac/ginac.h>

// CLN's As() macro corrupts wxWidgets' wxAny::As<T>(); undo it.
#ifdef As
#undef As
#endif

#include <string>
#include <vector>

namespace syms {

struct AnalysisResult {
    std::string input_desc;   // "V1"
    std::string output_desc;  // "V(out)"
    GiNaC::ex num_raw, den_raw;
    Pruned pruned;
    ParamTable params;
    PruneOptions opts;
    SweepSpec sweep;          // plotting / ranking sweep
    std::string report;       // full human-readable report (results panel)

    // Loop-gain extras (present only for the return-ratio analysis): the
    // unity-gain frequency and phase margin, in numeric and symbolic form.
    bool has_pm = false;
    std::string ugf_hz;       // formatted unity-gain frequency
    double pm_deg = 0.0;
    std::string pm_sym;       // text form, e.g. "180 - atan(w_ug/w_p0) ..."
    std::string pm_sym_latex; // LaTeX form

    // Noise extras (present only for the noise analysis): the output-referred
    // voltage noise density spectrum (Hz -> V/sqrt(Hz)), plus the input-referred
    // current noise when the excitation is a current source.
    bool has_noise = false;
    std::vector<double> noise_f_hz;
    std::vector<double> noise_vout;   // V/sqrt(Hz)
    double noise_vout_total = 0.0;    // integrated over the band (V rms)
    bool noise_input_is_current = false;
    double noise_iin_total = 0.0;     // input-referred current noise (A rms)
};

// Throws std::runtime_error with a user-facing message.
AnalysisResult analyze(const Circuit& c, const AnalysisRequest& req);

// Numeric response of the *unpruned* transfer function.
double mag_db_at(const AnalysisResult& r, double omega);
double phase_deg_at(const AnalysisResult& r, double omega);

// Log-spaced frequencies in Hz (legacy helper).
std::vector<double> sweep_hz(double f0, double f1, int npoints);

// Frequencies in Hz for a SPICE-style sweep (decade / octave / linear).
std::vector<double> sweep_points(const SweepSpec& s);

std::string format_report(const AnalysisResult& r);
// LaTeX report for the poles/zeros (typeset form for the Math tab): the
// time-constant and factor expressions rendered with \cdot and \parallel,
// with the numeric corner frequency kept as plain \mathrm{} text.
std::string format_report_latex(const AnalysisResult& r);

} // namespace syms
