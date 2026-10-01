// envlp integration harness: runs the full analysis suite on the
// common-source amplifier example and dumps text / LaTeX, verifying that every
// analysis produces a valid, non-empty result. Built as test_integration and
// run by CTest.
#include "core/Analysis.h"
#include "core/Engine.h"
#include "core/LowEntropy.h"
#include "core/Netlist.h"
#include "core/Par.h"
#include "core/Print.h"
#include "core/Solver.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace syms;
using GiNaC::ex;

static int g_fail = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            ++g_fail;                                                         \
            std::printf("  FAIL line %d: %s\n", __LINE__, #c);                \
        }                                                                     \
    } while (0)

#define CHECK_CLOSE(a, b, tol)                                                \
    do {                                                                      \
        double va_ = (a), vb_ = (b);                                          \
        if (!(std::fabs(va_ - vb_) <= (tol))) {                               \
            ++g_fail;                                                        \
            std::printf("  FAIL line %d: %s (=%.6g) != %s (=%.6g)\n",          \
                        __LINE__, #a, va_, #b, vb_);                          \
        }                                                                     \
    } while (0)

static Component comp(Kind k, const std::string& ref,
                      std::vector<std::string> nodes, const std::string& v = "") {
    Component c;
    c.kind = k;
    c.ref = ref;
    c.nodes = std::move(nodes);
    c.value_text = v;
    return c;
}

// The requested test circuit: common source amp, resistor load Rd, output
// capacitor CL, NMOS small-signal model with ro / Cgs / Cgd.
static Circuit common_source(bool with_rs = false) {
    Circuit c;
    c.comps.push_back(comp(Kind::V, "V1", {"in", "0"}, "1"));
    std::string g = "in";
    if (with_rs) {
        c.comps.push_back(comp(Kind::R, "Rs", {"in", "g"}, "10k"));
        c.comps.push_back(comp(Kind::R, "Rg", {"g", "0"}, "100k"));
        g = "g";
    }
    Component m = comp(Kind::NMOS, "M1", {"out", g, "0"}, "");
    m.param_on["ro"] = true;
    m.param_on["Cgs"] = true;
    m.param_on["Cgd"] = true;
    m.param_text["gm"] = "1m";
    m.param_text["ro"] = "100k";
    m.param_text["Cgs"] = "100f";
    m.param_text["Cgd"] = "20f";
    c.comps.push_back(m);
    c.comps.push_back(comp(Kind::R, "Rd", {"out", "0"}, "10k"));
    c.comps.push_back(comp(Kind::C, "CL", {"out", "0"}, "1p"));
    c.comps.push_back(comp(Kind::GND, "G1", {"0"}));
    return c;
}

// A hand-built DC transfer: gm*Rd gives the low-frequency gain.
static void test_tf_low_entropy() {
    Circuit c = common_source(false);
    AnalysisRequest req;
    req.input_ref = "V1";
    req.output = "V(out)";
    req.f0_hz = 1e5;
    AnalysisResult r = analyze(c, req);

    // low-entropy form is factored, with || and a labelled pole
    CHECK(r.pruned.text.find("||") != std::string::npos);
    CHECK(r.pruned.poles.size() == 1);
    CHECK(r.pruned.zeros.size() == 1);
    CHECK(r.pruned.latex.find("\\frac") != std::string::npos);
    CHECK(r.pruned.latex.find("\\parallel") != std::string::npos);
    std::printf("H(s) = %s\n", r.pruned.text.c_str());

    // DC gain: |H(0)| = gm*(Rd||ro)
    double dc = syms::mag_db_at(r, 0.0);
    double expect = 1e-3 * (10e3 * 100e3) / (10e3 + 100e3);
    CHECK_CLOSE(dc, 20.0 * std::log10(expect), 0.01);

    // exact vs pruned agreement within 0.5 dB across the band
    ex Hle = r.pruned.gain;
    for (const auto& f : r.pruned.num_factors) Hle = Hle * f.expr;
    ex Dle = ex(1);
    for (const auto& f : r.pruned.den_factors) Dle = Dle * f.expr;
    Hle = (Hle / Dle).normal();
    for (double f : {1e2, 1e4, 1e6, 1e7, 1e8}) {
        double w = 2 * M_PI * f;
        CHECK_CLOSE(syms::eval_mag_db(Hle, r.params, w), syms::mag_db_at(r, w),
                    0.5);
    }
}

static void test_tf_low_entropy_with_rs() {
    Circuit c = common_source(true);
    AnalysisRequest req;
    req.input_ref = "V1";
    req.output = "V(out)";
    req.f0_hz = 1e6;
    AnalysisResult r = analyze(c, req);
    std::printf("H(s) [Rs] = %s\n", r.pruned.text.c_str());
    // two poles, but the denominator does not factor exactly -> approximate
    CHECK(r.pruned.poles.size() == 2);
    CHECK(r.pruned.den_factors.size() == 2);
    // exact/pruned agreement within 1 dB
    ex Hle = r.pruned.gain;
    for (const auto& f : r.pruned.num_factors) Hle = Hle * f.expr;
    ex Dle = ex(1);
    for (const auto& f : r.pruned.den_factors) Dle = Dle * f.expr;
    Hle = (Hle / Dle).normal();
    for (double f : {1e3, 1e5, 1e6, 1e7, 1e8}) {
        double w = 2 * M_PI * f;
        CHECK_CLOSE(syms::eval_mag_db(Hle, r.params, w), syms::mag_db_at(r, w),
                    1.0);
    }
}

static void run_kind(const char* name, AnalysisKind kind, const Circuit& c,
                     const std::string& output = "V(out)",
                     const std::string& probe = "") {
    AnalysisSpec sp;
    sp.kind = kind;
    sp.input_ref = "V1";
    sp.output = output;
    sp.probe_ref = probe;
    sp.f0_hz = 1e5;
    std::printf("\n===== %s =====\n", name);
    try {
        CardResult cr = run_analysis(c, sp);
        std::printf("%s\n", cr.report.c_str());
        CHECK(!cr.summary.empty());
        CHECK(!cr.report.empty());
    } catch (const std::exception& e) {
        ++g_fail;
        std::printf("  FAIL %s threw: %s\n", name, e.what());
    }
}

int main() {
    test_tf_low_entropy();
    test_tf_low_entropy_with_rs();

    Circuit c = common_source(false);
    run_kind("DC", AnalysisKind::DC, c);
    run_kind("AC", AnalysisKind::AC, c);
    run_kind("Transfer function", AnalysisKind::TransferFunction, c);
    run_kind("Input impedance", AnalysisKind::InputImpedance, c);
    run_kind("Output impedance", AnalysisKind::OutputImpedance, c);
    run_kind("Short-circuit current", AnalysisKind::ShortCircuitCurrent, c);
    run_kind("Noise", AnalysisKind::Noise, c);

    std::printf("\n%s (%d failure(s))\n", g_fail ? "INTEGRATION FAILED" : "integration ok",
                g_fail);
    return g_fail == 0 ? 0 : 1;
}
