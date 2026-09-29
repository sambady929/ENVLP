#pragma once
#include "core/Engine.h"
#include "core/Netlist.h"
#include "core/Solver.h"

#include <ginac/ginac.h>

// CLN's As() macro corrupts wxWidgets' wxAny::As<T>(); undo it.
#ifdef As
#undef As
#endif

#include <map>
#include <string>
#include <utility>
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
    Differential,     // Adm / Acm / CMRR from a differential input port
};

struct AnalysisSpec {
    AnalysisKind kind = AnalysisKind::TransferFunction;
    std::string input_ref;   // excitation source (Vin)
    std::string output;      // "V(node)", "I(ref)", or a port such as "V(a)-V(b)"
    // Differential input port (Differential only): the two nodes the input is
    // applied across. A differential drive puts +1 on `input_port_p` and -1 on
    // `input_port_n`.
    std::string input_port_p;
    std::string input_port_n;
    std::string probe_ref;   // reference element for loop gain
    double f0_hz = 1e3;      // legacy tuning frequency (kept for compatibility)
    double threshold_db = 20.0; // series/parallel component reduction (dB)
    double pole_zero_threshold_db = 60.0; // pole/zero frequency reduction (dB)
    bool global_ref = false;
    bool prune = true;
    bool use_parallel = true;
    bool approx_factor = true; // numeric factoring when exact factoring fails
    bool normalize = true; // normalize denominator DC term to 1 (TFs only)

    // Standard SPICE-style frequency sweep: start/stop, interval type
    // (decade / octave / linear) and points per interval. Used for the plots
    // and for term ranking.
    SweepSpec sweep;

    // Process values for the large-signal DC model (Vth, Is), set from the
    // toolbar's DC settings dialog. Continuous values, not the component
    // editor's fixed steps.
    TechParams tech;

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
    std::string latex_report; // LaTeX report for the Math tab (poles/zeros)
    // only meaningful for transfer-like analyses
    bool has_transfer = false;
    AnalysisResult transfer;
    std::vector<std::pair<std::string, std::string>> values; // name -> expr
    // Small-signal parameter overrides extracted from a numeric DC operating
    // point (Mode 3 with the override checkbox): ref -> {param -> value}. A
    // caller running several cards back-to-back applies these to the circuit
    // before the following cards, so later symbolic analyses reflect the real
    // bias (e.g. a device's Cds shrinks and gets pruned by the threshold rule).
    std::map<std::string, std::map<std::string, double>> ss_overrides;
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
