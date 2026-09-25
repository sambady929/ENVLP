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
