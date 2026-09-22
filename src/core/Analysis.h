#pragma once
#include "core/Engine.h"
#include "core/Netlist.h"
#include "core/Solver.h"

#include <ginac/ginac.h>

// CLN's As() macro corrupts wxWidgets' wxAny::As<T>(); undo it.
#ifdef As
#undef As
#endif

#include <string>
#include <vector>

namespace syms {

// Which analysis card to run.
enum class AnalysisKind {
    TransferFunction, // H(s) = Vout/Vin (or Iout/Iin): the s-domain TF
    AC,               // AC node voltage / branch current (small-signal)
    DC,               // DC node voltages / branch currents
    PSRR,             // PSR = H(VDD->out); PSRR = H(Vin->out)/H(VDD->out)
    LoopGain,         // return ratio T(s) by nullor substitution
    ShortCircuitCurrent, // current when the node is shorted to gnd via 0 V
    InputImpedance,   // Zin seen by the input source
    OutputImpedance,  // Zout seen at the output node
    Noise,            // input/output referred noise
};

struct AnalysisSpec {
    AnalysisKind kind = AnalysisKind::TransferFunction;
    std::string input_ref;   // excitation source (Vin)
    std::string output;      // "V(node)" or "I(ref)"
    std::string probe_ref;   // reference element for loop gain
    double f0_hz = 1e3;
    double threshold_db = 40.0;
    bool global_ref = false;
    bool prune = true;
    bool use_parallel = true;
    bool gm_ro_assume = true; // assume gm*ro >> 1 when idealizing
    bool approx_factor = true; // numeric factoring when exact factoring fails
    bool normalize = true; // normalize denominator DC term to 1 (TFs only)

    // DC model selection for MOSFETs.
    enum class MosDc { SquareLaw, GmOverId } mos_dc = MosDc::SquareLaw;
};

struct CardResult {
    AnalysisKind kind = AnalysisKind::TransferFunction;
    std::string title;
    std::string summary; // one-line description of the result
    std::string text;    // low-entropy form (or DC value)
    std::string latex;   // LaTeX
    std::string report;  // full multi-line report
    // only meaningful for transfer-like analyses
    bool has_transfer = false;
    AnalysisResult transfer;
    std::vector<std::pair<std::string, std::string>> values; // name -> expr
};

// Runs one analysis card. Throws std::runtime_error on user-facing errors.
CardResult run_analysis(const Circuit& c, const AnalysisSpec& spec);

// Convenience wrappers (also used by tests).
CardResult analyze_dc(const Circuit& c, const AnalysisSpec& spec);
CardResult analyze_ac(const Circuit& c, const AnalysisSpec& spec);
CardResult analyze_tf(const Circuit& c, const AnalysisSpec& spec);
CardResult analyze_psrr(const Circuit& c, const AnalysisSpec& spec);
CardResult analyze_loop_gain(const Circuit& c, const AnalysisSpec& spec);
CardResult analyze_short_circuit(const Circuit& c, const AnalysisSpec& spec);
CardResult analyze_zin(const Circuit& c, const AnalysisSpec& spec);
CardResult analyze_zout(const Circuit& c, const AnalysisSpec& spec);
CardResult analyze_noise(const Circuit& c, const AnalysisSpec& spec);

} // namespace syms
