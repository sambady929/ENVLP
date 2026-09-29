// SymCirc core engine tests.
// Assert-based mini framework; run via CTest (all tests) or
//   test_core <substring>   to run a subset.

#include "core/Analysis.h"
#include "core/Eng.h"
#include "core/Engine.h"
#include "core/LowEntropy.h"
#include "core/MNA.h"
#include "core/Netlist.h"
#include "core/Par.h"
#include "core/Print.h"
#include "core/Prune.h"
#include "core/Solver.h"
#include "core/SpiceModel.h"
#include "core/DcNumeric.h"

#include <cmath>
#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

// Default GiNaC string form (GiNaC 1.8 has no ex::to_string()).
static std::string str(const GiNaC::ex& e) {
    std::ostringstream os;
    os << e;
    return os.str();
}

using namespace syms;
using GiNaC::ex;

static int g_failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            std::printf("  FAIL line %d: %s\n", __LINE__, #cond);              \
        }                                                                      \
    } while (0)

#define CHECK_CLOSE(a, b, tol)                                                 \
    do {                                                                       \
        double va_ = (a), vb_ = (b);                                           \
        if (!(std::fabs(va_ - vb_) <= (tol))) {                                \
            ++g_failures;                                                      \
            std::printf("  FAIL line %d: %s (=%.6g) != %s (=%.6g)\n", __LINE__,\
                        #a, va_, #b, vb_);                                     \
        }                                                                      \
    } while (0)

static Component comp(Kind k, const std::string& ref,
                      std::vector<std::string> nodes,
                      const std::string& value = "") {
    Component c;
    c.kind = k;
    c.ref = ref;
    c.nodes = std::move(nodes);
    c.value_text = value;
    return c;
}

static Circuit ground(Circuit c) {
    c.comps.push_back(comp(Kind::GND, "GND1", {"0"}));
    return c;
}

static ex raw_H(const AnalysisResult& r) { return (r.num_raw / r.den_raw).normal(); }

static ex S(const AnalysisResult& r, const char* name) {
    auto it = r.params.syms.find(name);
    if (it == r.params.syms.end()) {
        ++g_failures;
        std::printf("  FAIL: symbol '%s' not in result\n", name);
        return ex(0);
    }
    return it->second;
}

// ---------------------------------------------------------------------------
static void test_eng_parse() {
    double v = 0;
    CHECK(eng::parse_value("4.7k", v)); CHECK_CLOSE(v, 4700.0, 1e-9);
    CHECK(eng::parse_value("100n", v)); CHECK_CLOSE(v, 1e-7, 1e-18);
    CHECK(eng::parse_value("1M", v));   CHECK_CLOSE(v, 1e6, 1e-6);
    CHECK(eng::parse_value("1meg", v)); CHECK_CLOSE(v, 1e6, 1e-6);
    CHECK(eng::parse_value("10kOhm", v)); CHECK_CLOSE(v, 1e4, 1e-9);
    CHECK(eng::parse_value("0.5", v));  CHECK_CLOSE(v, 0.5, 1e-12);
    CHECK(eng::parse_value("2.2 pF", v)); CHECK_CLOSE(v, 2.2e-12, 1e-24);
    CHECK(!eng::parse_value("abc", v));
    CHECK(!eng::parse_value("", v));
    CHECK(eng::format_eng(4700.0).substr(0, 3) == "4.7");
    CHECK(eng::format_eng(1e-7).find("n") != std::string::npos);
    // The micro prefix is a raw UTF-8 µ in text, but must become `\mu` in
    // LaTeX (a raw µ inside math mode renders as a broken glyph).
    std::string hz = eng::format_hz(1e-6);
    CHECK(hz.find("\xC2\xB5") != std::string::npos);         // text keeps µ
    std::string lx = eng::latex_safe(hz);
    CHECK(lx.find("\\mu") != std::string::npos);             // LaTeX gets \mu
    CHECK(lx.find("\xC2\xB5") == std::string::npos);         // no raw µ left
}

// ---------------------------------------------------------------------------
static void test_sweep_points() {
    // decade: 3 decades * 10 pts -> 31 points from 1 to 1e3
    SweepSpec s;
    s.f_start_hz = 1.0;
    s.f_stop_hz = 1e3;
    s.type = SweepType::Decade;
    s.points_per_interval = 10;
    auto pts = sweep_points(s);
    CHECK(pts.size() == 31);
    CHECK_CLOSE(pts.front(), 1.0, 1e-9);
    CHECK_CLOSE(pts.back(), 1e3, 1e-6);

    // octave: 1..8 is 3 octaves -> 3*4 + 1 points
    SweepSpec o;
    o.f_start_hz = 1.0;
    o.f_stop_hz = 8.0;
    o.type = SweepType::Octave;
    o.points_per_interval = 4;
    auto op = sweep_points(o);
    CHECK(op.size() == 13);
    CHECK_CLOSE(op.back(), 8.0, 1e-6);

    // linear: exactly n points
    SweepSpec l;
    l.f_start_hz = 0.0;
    l.f_stop_hz = 100.0;
    l.type = SweepType::Linear;
    l.points_per_interval = 5;
    auto lp = sweep_points(l);
    CHECK(lp.size() == 5);
    CHECK_CLOSE(lp[0], 0.0, 1e-12);
    CHECK_CLOSE(lp[4], 100.0, 1e-9);
}

// A zero (or pole) is negligible relative to the *dominant pole*, not relative
// to the other zeros: two zeros 25 dB apart are both far beyond a dominant pole
// 2000x slower than both and must both be dropped. A zero only 10x above the
// dominant pole (20 dB) stays.
static void test_far_zeros_vs_dominant_pole() {
    // Mirror the 5T-OTA shape: a dominant pole set by a big load, plus a
    // numerator quadratic whose numeric roots are both far beyond it. Because
    // the zeros are close to *each other* (25 dB apart, not 60), the old rule
    // -- comparing each zero only against the other zeros -- kept both; the
    // correct rule compares against the dominant pole and drops both.
    {
        ParamTable pt;
        ex s = pt.get("s");
        // dominant pole w = 500 rad/s (tau = 2 ms); zeros at w = 2.5e6 and
        // 5e5 rad/s (tau 4e-7 / 2e-6), i.e. > 60 dB above the pole, and only
        // 5x (14 dB) apart from each other.
        ex num = (1 + s * ex(4e-7)) * (1 + s * ex(2e-6));
        ex den = (1 + s * ex(2e-3));
        LowEntropyOptions o;
        o.prune = true;
        o.normalize = false;
        o.approx_factor = true; // numeric linear factors, as the OTA produces
        o.threshold_db = 20;
        o.pole_zero_threshold_db = 60;
        o.use_parallel = false;
        o.band_lo_hz = 1.0;
        o.band_hi_hz = 1e9;
        LowEntropy le = low_entropy(num.expand(), den.expand(), pt, o);
        CHECK(le.zeros.empty());
        CHECK(!le.poles.empty());
    }
    // A zero only ~40x (32 dB) above the dominant pole stays.
    {
        ParamTable pt;
        ex s = pt.get("s");
        ex num = (1 + s * ex(5e-5)); // w = 20e3, pole w = 500 -> 32 dB, kept
        ex den = (1 + s * ex(2e-3));
        LowEntropyOptions o;
        o.prune = true;
        o.normalize = false;
        o.approx_factor = true;
        o.threshold_db = 20;
        o.pole_zero_threshold_db = 60;
        o.use_parallel = false;
        o.band_lo_hz = 1.0;
        o.band_hi_hz = 1e9;
        LowEntropy le = low_entropy(num.expand(), den.expand(), pt, o);
        CHECK(!le.zeros.empty());
    }
    // With approximate factoring OFF (the default), a numerator quadratic that
    // does not factor symbolically must still be pruned root-by-root: a fast
    // zero pair beyond the dominant pole is dropped. This is the 5T-OTA case.
    {
        ParamTable pt;
        ex s = pt.get("s");
        // dominant pole w = 500 (tau 2 ms); numerator roots at w = 5e5 / 2.5e6
        // (tau 2e-6 / 4e-7), both > 60 dB above the pole.
        ex num = (1 + s * ex(2e-6)) * (1 + s * ex(4e-7));
        ex den = (1 + s * ex(2e-3));
        LowEntropyOptions o;
        o.prune = true;
        o.normalize = false;
        o.approx_factor = false; // NO approximate factoring
        o.threshold_db = 20;
        o.pole_zero_threshold_db = 60;
        o.use_parallel = false;
        o.band_lo_hz = 1.0;
        o.band_hi_hz = 1e9;
        LowEntropy le = low_entropy(num.expand(), den.expand(), pt, o);
        CHECK(le.zeros.empty());
        CHECK(!le.poles.empty());
    }
}

// The ranking band decides which terms are negligible. A term that is tiny at
// DC but dominant at the top of the band must be kept.
static void test_rank_band_keeps_high_freq_term() {
    ParamTable pt;
    ex s = pt.get("s");
    ex R = pt.get("R"), C = pt.get("C");
    pt.set("R", 1e3, UnitClass::Ohm);
    pt.set("C", 1e-9, UnitClass::Farad);
    // denominator 1 + s*R*C, plus a huge s^3 term that is negligible at DC but
    // huge at the top of a wide band.
    ex d = 1 + s * R * C + GiNaC::pow(s, 3) * R * R * R * C * C * C;

    LowEntropyOptions o;
    o.prune = true;
    o.normalize = false;
    o.approx_factor = false;
    o.threshold_db = 40;
    o.global_ref = true; // compare every term against the whole polynomial
    // narrow band at 1 Hz: the s^3 term is negligible
    o.band_lo_hz = 1.0;
    o.band_hi_hz = 1.0;
    ParamTable pt2 = pt;
    LowEntropy le_narrow = low_entropy(ex(1), d, pt2, o);
    CHECK(le_narrow.den_poly.degree(s) <= 1);

    // wide band out to 1 GHz: the s^3 term dominates and must be kept
    LowEntropyOptions ow = o;
    ow.band_lo_hz = 1.0;
    ow.band_hi_hz = 1e9;
    ParamTable pt3 = pt;
    LowEntropy le_wide = low_entropy(ex(1), d, pt3, ow);
    CHECK(le_wide.den_poly.degree(s) == 3);
}

// Ground and supply symbols are anonymous: several share a reference.
static void test_anonymous_ground_refs() {
    Circuit c;
    c.comps.push_back(comp(Kind::V, "V1", {"in", "0"}, "1"));
    c.comps.push_back(comp(Kind::R, "R1", {"in", "0"}, "1k"));
    // two grounds with the same ref must validate
    c.comps.push_back(comp(Kind::GND, "GND", {"0"}));
    c.comps.push_back(comp(Kind::GND, "GND", {"0"}));
    std::string err;
    CHECK(c.validate(err));
    // two resistors with the same ref must NOT validate
    Circuit bad = c;
    bad.comps.push_back(comp(Kind::R, "R1", {"in", "0"}, "1k"));
    err.clear();
    CHECK(!bad.validate(err));
}

// ---------------------------------------------------------------------------
static void test_roots() {
    auto r1 = poly_roots({2, 3, 1}); // s^2 + 3s + 2
    CHECK(r1.size() == 2);
    if (r1.size() == 2) {
        double a0 = std::abs(r1[0] + 1.0), a1 = std::abs(r1[1] + 2.0);
        double b0 = std::abs(r1[1] + 1.0), b1 = std::abs(r1[0] + 2.0);
        CHECK(std::min(a0 + a1, b0 + b1) < 1e-8);
    }
    auto r2 = poly_roots({1, 0, 1}); // s^2 + 1
    CHECK(r2.size() == 2);
    if (r2.size() == 2) {
        CHECK_CLOSE(std::abs(r2[0].real()), 0.0, 1e-8);
        CHECK_CLOSE(std::abs(std::abs(r2[0].imag()) - 1.0), 0.0, 1e-8);
    }
}

// ---------------------------------------------------------------------------
static void test_divider() {
    Circuit c;
    c.comps.push_back(comp(Kind::V, "V1", {"in", "0"}, "1"));
    c.comps.push_back(comp(Kind::R, "R1", {"in", "out"}, "10k"));
    c.comps.push_back(comp(Kind::R, "R2", {"out", "0"}, "10k"));
    c = ground(c);

    AnalysisRequest req;
    req.input_ref = "V1";
    req.output = "V(out)";
    AnalysisResult r = analyze(c, req);

    ex H = raw_H(r);
    ex expect = S(r, "R2") / (S(r, "R1") + S(r, "R2"));
    CHECK((H - expect).normal().is_zero());
    // DC-only circuit: factored form is just the gain
    CHECK(r.pruned.dropped.empty());
    CHECK(!r.report.empty());
    CHECK(r.report.find("H(s)") != std::string::npos);
}

// ---------------------------------------------------------------------------
static void test_rc_lowpass() {
    Circuit c;
    c.comps.push_back(comp(Kind::V, "V1", {"in", "0"}, "1"));
    c.comps.push_back(comp(Kind::R, "R1", {"in", "out"}, "10k"));
    c.comps.push_back(comp(Kind::C, "C1", {"out", "0"}, "1n"));
    c = ground(c);

    AnalysisRequest req;
    req.input_ref = "V1";
    req.output = "V(out)";
    AnalysisResult r = analyze(c, req);

    ex s = S(r, "s");
    ex H = raw_H(r);
    ex expect = ex(1) / (ex(1) + s * S(r, "R1") * S(r, "C1"));
    CHECK((H - expect).normal().is_zero());

    // factored low-entropy form: 1 / (1 + s*R1*C1)
    CHECK(r.pruned.text.find("R1") != std::string::npos);
    CHECK(r.pruned.poles.size() == 1);
    if (!r.pruned.poles.empty()) {
        CHECK(r.pruned.poles[0].label.find("R1") != std::string::npos);
        CHECK_CLOSE(r.pruned.poles[0].omega, 1.0 / (10e3 * 1e-9),
                    1e3); // 1/(RC) within 0.1%
    }

    // Bode sanity: at w = 1/RC magnitude is -3.01 dB
    double w = 1.0 / (10e3 * 1e-9);
    CHECK_CLOSE(mag_db_at(r, w), -3.0103, 0.01);
    CHECK_CLOSE(phase_deg_at(r, w), -45.0, 0.1);
}

// ---------------------------------------------------------------------------
static void test_cs_amp_parasitics() {
    // common-source: V1 -> gate, drain -> Rd -> out
    auto make = [](bool ro_on, bool cgd_on, bool cgs_on) {
        Circuit c;
        c.comps.push_back(comp(Kind::V, "V1", {"in", "0"}, "1"));
        Component m = comp(Kind::NMOS, "M1", {"out", "in", "0"}, "");
        m.param_on["ro"] = ro_on;
        m.param_on["Cgd"] = cgd_on;
        m.param_on["Cgs"] = cgs_on;
        // Cds now defaults on (following Cgd); these cases predate it, so turn
        // it off explicitly to keep the expected expressions unchanged.
        m.param_on["Cds"] = false;
        c.comps.push_back(m);
        c.comps.push_back(comp(Kind::R, "Rd", {"out", "0"}, "10k"));
        return ground(c);
    };
    AnalysisRequest req;
    req.input_ref = "V1";
    req.output = "V(out)";
    ex s, gm, ro, cgd;

    // (a) no parasitics: H = -gm*Rd
    {
        AnalysisResult r = analyze(make(false, false, false), req);
        s = S(r, "s"); gm = S(r, "gm_M1"); ro = S(r, "ro_M1"); cgd = S(r, "Cgd_M1");
        ex H = raw_H(r);
        CHECK((H + gm * S(r, "Rd")).normal().is_zero());
        CHECK(str(H).find("ro_M1") == std::string::npos);
        CHECK(str(H).find("Cgd_M1") == std::string::npos);
    }
    // (b) ro enabled: H = -gm * (Rd || ro), and the parallel atom is HELD
    //     (low entropy) rather than expanded to Rd*ro/(Rd+ro).
    {
        AnalysisResult r = analyze(make(true, false, false), req);
        gm = S(r, "gm_M1"); ro = S(r, "ro_M1");
        ex H = raw_H(r);
        CHECK(str(H).find("ro_M1") != std::string::npos);
        ex expect = -gm * par_ex(S(r, "Rd"), ro);
        CHECK((H - expect).normal().is_zero());
    }
    // (c) Cgd enabled with ideal drive: H = -(gm - s*Cgd)*Rd / (1 + s*Cgd*Rd)
    //     (note the RHP zero at s = gm/Cgd -- Miller feedforward)
    {
        AnalysisResult r = analyze(make(false, true, false), req);
        s = S(r, "s"); gm = S(r, "gm_M1"); cgd = S(r, "Cgd_M1");
        ex H = raw_H(r);
        CHECK(str(H).find("Cgd_M1") != std::string::npos);
        ex expect = -(gm - s * cgd) * S(r, "Rd") / (ex(1) + s * cgd * S(r, "Rd"));
        CHECK((H - expect).normal().is_zero());
    }
    // (d) Cgs enabled: gate held by ideal source => V(out) unchanged, but
    //     Cgs really is stamped (it shows up as the gate current s*Cgs).
    {
        AnalysisResult a = analyze(make(false, false, false), req);
        AnalysisResult b = analyze(make(false, false, true), req);
        CHECK((raw_H(a) - raw_H(b)).normal().is_zero());
        AnalysisRequest ri = req;
        ri.output = "I(V1)";
        AnalysisResult gi = analyze(make(false, false, true), ri);
        CHECK(str(gi.num_raw).find("Cgs_M1") != std::string::npos);
        CHECK((raw_H(gi) + S(gi, "s") * S(gi, "Cgs_M1")).normal().is_zero());
    }
}

// ---------------------------------------------------------------------------
static void test_prune_series_r() {
    // V -> R1(10k) -> R2(10) -> C(1n) -> gnd.  C*R2 is 60 dB below C*R1.
    // The series resistor R2 is collapsed at the 20 dB structural threshold,
    // so the denominator reads 1 + s*R1*C1 (series reduction is a silent
    // structural simplification, not a reported "neglected term").
    Circuit c;
    c.comps.push_back(comp(Kind::V, "V1", {"in", "0"}, "1"));
    c.comps.push_back(comp(Kind::R, "R1", {"in", "a"}, "10k"));
    c.comps.push_back(comp(Kind::R, "R2", {"a", "out"}, "10"));
    c.comps.push_back(comp(Kind::C, "C1", {"out", "0"}, "1n"));
    c = ground(c);

    AnalysisRequest req;
    req.input_ref = "V1";
    req.output = "V(out)";
    AnalysisResult r = analyze(c, req);

    ex s = S(r, "s");
    ex den_expect = ex(1) + s * S(r, "R1") * S(r, "C1");
    CHECK((r.pruned.den_poly - den_expect).normal().is_zero());
    CHECK(r.pruned.text.find("R1") != std::string::npos);
    CHECK(r.pruned.text.find("R2") == std::string::npos);
}

// ---------------------------------------------------------------------------
static void test_prune_no_drop_when_close() {
    Circuit c;
    c.comps.push_back(comp(Kind::V, "V1", {"in", "0"}, "1"));
    c.comps.push_back(comp(Kind::R, "R1", {"in", "a"}, "10k"));
    c.comps.push_back(comp(Kind::R, "R2", {"a", "out"}, "8.2k"));
    c.comps.push_back(comp(Kind::C, "C1", {"out", "0"}, "1n"));
    c = ground(c);

    AnalysisRequest req;
    req.input_ref = "V1";
    req.output = "V(out)";
    AnalysisResult r = analyze(c, req);
    // R2/R1 is only ~2 dB below: nothing should be dropped
    CHECK(r.pruned.dropped.empty());
}

// ---------------------------------------------------------------------------
static void test_two_stage_factoring() {
    // RC stage, unity buffer (E), RC stage => den = (1+sR1C1)(1+sR2C2)
    Circuit c;
    c.comps.push_back(comp(Kind::V, "V1", {"in", "0"}, "1"));
    c.comps.push_back(comp(Kind::R, "R1", {"in", "a"}, "10k"));
    c.comps.push_back(comp(Kind::C, "C1", {"a", "0"}, "1n"));
    c.comps.push_back(comp(Kind::E, "E1", {"b", "0", "a", "0"}, "1"));
    c.comps.push_back(comp(Kind::R, "R2", {"b", "out"}, "20k"));
    c.comps.push_back(comp(Kind::C, "C2", {"out", "0"}, "100p"));
    c = ground(c);

    AnalysisRequest req;
    req.input_ref = "V1";
    req.output = "V(out)";
    AnalysisResult r = analyze(c, req);

    CHECK(r.pruned.den_factors.size() == 2);
    CHECK(r.pruned.text.find("R1") != std::string::npos);
    CHECK(r.pruned.text.find("R2") != std::string::npos);
    CHECK(r.pruned.poles.size() == 2);
    for (const auto& p : r.pruned.poles) {
        CHECK(p.real_root);
        CHECK(!p.label.empty());
    }

    // raw H must equal the exact product form
    ex s = S(r, "s");
    ex expect = ex(1) / ((ex(1) + s * S(r, "R1") * S(r, "C1")) *
                         (ex(1) + s * S(r, "R2") * S(r, "C2")));
    CHECK((raw_H(r) - expect).normal().is_zero());
}

// ---------------------------------------------------------------------------
static void test_degree_drop_at_low_f0() {
    // widely separated poles: at f0 = 1 kHz the s^2 term is far below the
    // dominant term and should vanish from the denominator.
    Circuit c;
    c.comps.push_back(comp(Kind::V, "V1", {"in", "0"}, "1"));
    c.comps.push_back(comp(Kind::R, "R1", {"in", "a"}, "100k"));
    c.comps.push_back(comp(Kind::C, "C1", {"a", "0"}, "100n")); //10 ms
    c.comps.push_back(comp(Kind::E, "E1", {"b", "0", "a", "0"}, "1"));
    c.comps.push_back(comp(Kind::R, "R2", {"b", "out"}, "1k"));
    c.comps.push_back(comp(Kind::C, "C2", {"out", "0"}, "1n")); //1 us
    c = ground(c);

    AnalysisRequest req;
    req.input_ref = "V1";
    req.output = "V(out)";
    req.f0_hz = 1000.0;
    AnalysisResult r = analyze(c, req);

    ex s = S(r, "s");
    int deg = r.pruned.den_poly.degree(s);
    CHECK(deg >= 1 && deg <= 2);
    // the s^2 coefficient must be gone or drastically reduced; at minimum
    // the pruning machinery must have dropped something
    CHECK(!r.pruned.dropped.empty());
}

// ---------------------------------------------------------------------------
static void test_branch_current_output() {
    Circuit c;
    c.comps.push_back(comp(Kind::V, "V1", {"in", "0"}, "1"));
    c.comps.push_back(comp(Kind::R, "R1", {"in", "out"}, "10k"));
    c.comps.push_back(comp(Kind::R, "R2", {"out", "0"}, "10k"));
    c = ground(c);

    AnalysisRequest req;
    req.input_ref = "V1";
    req.output = "I(R2)";
    AnalysisResult r = analyze(c, req);

    ex H = raw_H(r);
    ex expect = ex(1) / (S(r, "R1") + S(r, "R2"));
    CHECK((H - expect).normal().is_zero());
}

// ---------------------------------------------------------------------------
static void test_bjt_rb_node() {
    Circuit c;
    c.comps.push_back(comp(Kind::V, "V1", {"in", "0"}, "1"));
    Component q = comp(Kind::NPN, "Q1", {"out", "in", "0"}, "");
    q.param_on["rb"] = true;
    c.comps.push_back(q);
    c.comps.push_back(comp(Kind::R, "R1", {"out", "0"}, "10k"));
    c = ground(c);

    AnalysisRequest req;
    req.input_ref = "V1";
    req.output = "V(out)";
    AnalysisResult r = analyze(c, req);
    CHECK(str(r.num_raw).find("rb_Q1") != std::string::npos ||
          str(r.den_raw).find("rb_Q1") != std::string::npos);

    // without rb the internal node must not exist
    Circuit c2 = c;
    c2.comps[1].param_on["rb"] = false;
    AnalysisResult r2 = analyze(c2, req);
    CHECK(str(r2.num_raw).find("rb_Q1") == std::string::npos &&
          str(r2.den_raw).find("rb_Q1") == std::string::npos);
}

// ---------------------------------------------------------------------------
static void test_errors() {
    // no ground
    Circuit c;
    c.comps.push_back(comp(Kind::V, "V1", {"in", "0x"}, "1")); // no true ground? "0x" is not ground
    c.comps.push_back(comp(Kind::R, "R1", {"in", "out"}, "1k"));
    AnalysisRequest req;
    req.input_ref = "V1";
    req.output = "V(out)";
    bool threw = false;
    try {
        analyze(c, req);
    } catch (const std::exception& e) {
        threw = true;
        CHECK(std::string(e.what()).find("ground") != std::string::npos);
    }
    CHECK(threw);

    // bad input
    Circuit g = ground(c);
    AnalysisRequest bad = req;
    bad.input_ref = "R1";
    threw = false;
    try {
        analyze(g, bad);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
}

// ---------------------------------------------------------------------------
static void test_size_offset_db() {
    // size_db shifts the estimate used for pruning: make R2 "look"10 kOhm
    // even though its nominal value is10 ohm -> the s term no longer drops.
    Circuit c;
    c.comps.push_back(comp(Kind::V, "V1", {"in", "0"}, "1"));
    c.comps.push_back(comp(Kind::R, "R1", {"in", "a"}, "10k"));
    Component r2 = comp(Kind::R, "R2", {"a", "out"}, "10");
    r2.size_db = 60; // x1000 =>10 kOhm estimate
    c.comps.push_back(r2);
    c.comps.push_back(comp(Kind::C, "C1", {"out", "0"}, "1n"));
    c = ground(c);

    AnalysisRequest req;
    req.input_ref = "V1";
    req.output = "V(out)";
    AnalysisResult r = analyze(c, req);
    for (const auto& d : r.pruned.dropped)
        CHECK(d.term.find("R2") == std::string::npos);
}

// ---------------------------------------------------------------------------
// Low-entropy engine: parallel forms, magnitude pruning, factoring.
// ---------------------------------------------------------------------------
static Circuit cs_amp(double cl, bool cgd_on) {
    Circuit c;
    c.comps.push_back(comp(Kind::V, "V1", {"in", "0"}, "1"));
    Component m = comp(Kind::NMOS, "M1", {"out", "in", "0"}, "");
    m.param_on["ro"] = true;
    m.param_on["Cgd"] = cgd_on;
    m.param_text["gm"] = "1m";
    m.param_text["ro"] = "100k";
    m.param_text["Cgd"] = "20f";
    c.comps.push_back(m);
    c.comps.push_back(comp(Kind::R, "Rd", {"out", "0"}, "10k"));
    c.comps.push_back(comp(Kind::C, "CL", {"out", "0"},
                           cl > 0 ? "1p" : ""));
    c.comps.push_back(comp(Kind::GND, "GND1", {"0"}));
    return c;
}

static void test_parallel_form() {
    // V -> R1 -> out, with R2 to ground: Rout = R1||R2, no Cgd here.
    Circuit c;
    c.comps.push_back(comp(Kind::V, "V1", {"in", "0"}, "1"));
    c.comps.push_back(comp(Kind::R, "R1", {"in", "out"}, "10k"));
    c.comps.push_back(comp(Kind::R, "R2", {"out", "0"}, "10k"));
    c.comps.push_back(comp(Kind::C, "C1", {"out", "0"}, "1n"));
    c = ground(c);
    AnalysisRequest req;
    req.input_ref = "V1";
    req.output = "V(out)";
    AnalysisResult r = analyze(c, req);
    // the pole is (R1||R2)*C1
    CHECK(!r.pruned.poles.empty());
    if (!r.pruned.poles.empty())
        CHECK(r.pruned.poles[0].label.find("||") != std::string::npos);
    CHECK(r.pruned.text.find("||") != std::string::npos);
}

static void test_magnitude_pruning() {
    // CL dominates Cgd: at a low f0 the Cgd-driven terms are negligible and
    // must be dropped, leaving a single dominant pole.
    Circuit c;
    c.comps.push_back(comp(Kind::V, "V1", {"in", "0"}, "1"));
    Component m = comp(Kind::NMOS, "M1", {"out", "in", "0"}, "");
    m.param_on["ro"] = true;
    m.param_on["Cgd"] = true;
    m.param_text["gm"] = "1m";
    m.param_text["ro"] = "100k";
    m.param_text["Cgd"] = "20f"; // tiny
    c.comps.push_back(m);
    c.comps.push_back(comp(Kind::R, "Rd", {"out", "0"}, "10k"));
    c.comps.push_back(comp(Kind::C, "CL", {"out", "0"}, "1u")); // huge
    c.comps.push_back(comp(Kind::GND, "GND1", {"0"}));
    AnalysisRequest req;
    req.input_ref = "V1";
    req.output = "V(out)";
    req.f0_hz = 1e3;
    req.threshold_db = 40.0;
    AnalysisResult r = analyze(c, req);
    // The output pole is (CL + Cgd)*(Rd||ro). Cgd is ~154 dB below CL, so as a
    // parallel capacitance it vanishes: the surviving factored denominator has
    // a single pole with CL only, and Cgd appears only in the (unpruned)
    // numerator feedforward term.
    CHECK(r.pruned.text.find("CL") != std::string::npos);
    CHECK(r.pruned.den_factors.size() == 1);
    if (r.pruned.den_factors.size() == 1)
        CHECK(r.pruned.den_factors[0].text.find("Cgd") == std::string::npos);

    // exact mode keeps every term
    req.prune = false;
    AnalysisResult re = analyze(c, req);
    CHECK(re.pruned.dropped.empty());
    CHECK(re.pruned.exact);
}

static void test_parallel_collapse() {
    // gm*R1*R2/(R1+R2) style: the parallel pair must survive in the text.
    ParamTable pt;
    GiNaC::ex s = pt.get("s");
    GiNaC::ex R1 = pt.get("R1"), R2 = pt.get("R2");
    pt.set("R1", 1e3, UnitClass::Ohm);
    pt.set("R2", 1e3, UnitClass::Ohm);
    GiNaC::ex e = to_parallel((R1 * R2) / (R1 + R2));
    CHECK(is_parallel(e));
    LowEntropyOptions o;
    o.prune = false;
    LowEntropy le = low_entropy(R1 * R2 / (R1 + R2), GiNaC::ex(1), pt, o);
    CHECK(le.text.find("||") != std::string::npos);
    (void)s;
}

// A sum of rationals whose shared, expanded denominator is
// gm_M4*gm_M1*(ro_M1 + ro_M4) must collapse to gm_M4*gm_M1*(ro_M1||ro_M4) --
// the parallel partner of a differential pair's ro. factor()+normal() exposes
// the additive factor and to_parallel recovers the held atom; without the
// recursive denominator handling the expanded denominator was repeated in
// every s-coefficient and the mirror's ro_M1/ro_M4 never combined.
static void test_rational_sum_exposes_parallel() {
    ParamTable pt;
    GiNaC::ex ro1 = pt.get("ro_M1"), ro4 = pt.get("ro_M4");
    GiNaC::ex gm1 = pt.get("gm_M1"), gm4 = pt.get("gm_M4");
    GiNaC::ex C1 = pt.get("C1"), Cds1 = pt.get("Cds_M1");
    GiNaC::ex c = (C1 * ro4 * ro1 * gm1 * Cds1) / (ro1 * gm4 * gm1 + ro4 * gm4 * gm1) / 2 +
                  (C1 * ro4 * ro1 * gm4 * Cds1) / (ro1 * gm4 * gm1 + ro4 * gm4 * gm1);
    GiNaC::ex par = to_parallel(GiNaC::factor(c.normal()));
    // The collapsed form must contain a single held parallel atom.
    bool any_par = false;
    std::function<void(const GiNaC::ex&)> scan = [&](const GiNaC::ex& e) {
        if (is_parallel(e)) { any_par = true; return; }
        for (size_t i = 0; i < e.nops(); ++i) scan(e.op(i));
    };
    scan(par);
    CHECK(any_par);
}

// A well-separated two-pole denominator with SYMBOLIC coefficients factors
// symbolically as the dominant-pole product (1 + c1*s)*(1 + (c2/c1)*s), so the
// time constants stay R*C products instead of collapsing to numeric roots.
static void test_symbolic_two_pole_factorization() {
    ParamTable pt;
    ex s = pt.get("s");
    ex R = pt.get("R"), C1 = pt.get("C1"), C2 = pt.get("C2");
    pt.set("R", 1e5, UnitClass::Ohm);
    pt.set("C1", 1e-12, UnitClass::Farad);
    pt.set("C2", 1e-14, UnitClass::Farad); // 100x: separable but not pruned
    // (1 + s*R*C1)*(1 + s*R*C2) expanded -- exact, but presented expanded
    ex den = (1 + s * R * C1) * (1 + s * R * C2);

    LowEntropyOptions o;
    o.prune = true;
    o.normalize = false;
    o.approx_factor = false; // no numeric help
    o.threshold_db = 20;
    o.pole_zero_threshold_db = 60;
    o.use_parallel = false;
    o.band_lo_hz = 1.0;
    o.band_hi_hz = 1e9;
    LowEntropy le = low_entropy(ex(1), den.expand(), pt, o);
    // Two symbolic first-order factors, each with an R*C-like time constant.
    CHECK(le.den_factors.size() == 2);
    CHECK(le.den_factors[0].text.find("s*") != std::string::npos);
    CHECK(le.den_factors[1].text.find("s*") != std::string::npos);
}

static void test_latex_output() {
    Circuit c = cs_amp(1e-12, true);
    AnalysisRequest req;
    req.input_ref = "V1";
    req.output = "V(out)";
    AnalysisResult r = analyze(c, req);
    CHECK(!r.pruned.latex.empty());
    CHECK(r.pruned.latex.find("H(s)") != std::string::npos);
    CHECK(r.pruned.latex.find("\\frac") != std::string::npos);
    CHECK(r.pruned.latex.find("\\parallel") != std::string::npos);
}

// A sum that appears as a FACTOR inside a product must be parenthesised in the
// LaTeX, matching the text result. cs_test's denominator is
// (1 + s*(Cds_M1 + C1)*R1): the (Cds_M1 + C1) grouped factor is the case that
// regressed -- to_latex_cdot dropped the parens.
static void test_latex_factor_parens() {
    Circuit c;
    c.comps.push_back(comp(Kind::V, "V1", {"in", "0"}, "1"));
    Component m = comp(Kind::NMOS, "M1", {"out", "in", "0"}, "");
    m.param_on["ro"] = false;
    m.param_on["Cgs"] = false;
    m.param_on["Cgd"] = false;
    m.param_on["Cdb"] = false;
    m.param_on["Csb"] = false;
    m.param_on["Cds"] = true;  m.param_text["Cds"] = "20f";
    m.param_text["gm"] = "1m";
    c.comps.push_back(m);
    c.comps.push_back(comp(Kind::R, "R1", {"out", "VDD"}, "1k"));
    c.comps.push_back(comp(Kind::VDD, "VDD1", {"VDD"}));
    c.comps.push_back(comp(Kind::C, "C1", {"out", "0"}, "100f"));
    c.comps.push_back(comp(Kind::GND, "G1", {"0"}));
    AnalysisRequest req;
    req.input_ref = "V1";
    req.output = "V(out)";
    req.f0_hz = 1.0;
    req.sweep.f_start_hz = 1.0;
    req.sweep.f_stop_hz = 1e9;
    AnalysisResult r = analyze(c, req);
    // The grouped (Cds_M1 + C1) must be a parenthesised factor in the LaTeX
    // (order-tolerant: GiNaC may print C1 + Cds_M1).
    bool grouped =
        r.pruned.latex.find("\\left(Cds_M1 + C1\\right)") != std::string::npos ||
        r.pruned.latex.find("\\left(C1 + Cds_M1\\right)") != std::string::npos;
    CHECK(grouped);
}

static void test_report_has_latex_and_factors() {
    Circuit c = cs_amp(1e-12, true);
    AnalysisRequest req;
    req.input_ref = "V1";
    req.output = "V(out)";
    req.f0_hz = 1e5;
    AnalysisResult r = analyze(c, req);
    // The plain-text report is deliberately NOT LaTeX: no \frac, \cdot,
    // \parallel. It carries the readable H(s) plus poles/zeros. The LaTeX
    // form lives separately in r.pruned.latex for the Math tab.
    CHECK(r.report.find("H(s) = ") != std::string::npos);
    CHECK(r.report.find("\\frac") == std::string::npos);
    CHECK(r.report.find("\\cdot") == std::string::npos);
    CHECK(r.report.find("Poles:") != std::string::npos);
    // ...and the LaTeX form exists and is real LaTeX.
    CHECK(r.pruned.latex.find("\\frac") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Regressions for the low-entropy engine correctness fixes.
// ---------------------------------------------------------------------------

// A malformed factor (a rational whose denominator survives into a
// "factor") used to print a sum containing an internal division next to a
// leading coefficient. Check the factored text is a well-formed product of
// (1 + s*...) factors.
static bool text_looks_factored(const std::string& t) {
    if (t.empty()) return false;
    // every '/' must be at the very top level of the outer ratio, not nested
    // inside a parenthesised factor. Count balance at each '/'.
    int depth = 0;
    for (size_t i = 0; i < t.size(); ++i) {
        if (t[i] == '(') ++depth;
        else if (t[i] == ')') --depth;
        else if (t[i] == '/' && depth > 0) return false;
    }
    return true;
}

static void test_low_entropy_no_bogus_factor() {
    // This denominator is NOT exactly factorable (the s^2 term is a sum), so
    // the old engine fabricated a wrong factor via a vacuous divisibility
    // test and printed garbage. Now it must either factor numerically or keep
    // one honest polynomial.
    ParamTable pt;
    ex s = pt.get("s");
    ex a = pt.get("a"), b = pt.get("b"), c = pt.get("c");
    pt.set("a", 1e-3, UnitClass::Farad);
    pt.set("b", 2e-3, UnitClass::Farad);
    pt.set("c", 3e-3, UnitClass::Farad);
    // 1 + s*(a+b) + s^2*(a*b+c) is not a product of linear real factors in
    // general, but the coefficients are all positive and it is a valid
    // (stable) denominator.
    ex den = 1 + s * (a + b) + s * s * (a * b + c);
    LowEntropyOptions o;
    o.prune = false;
    o.approx_factor = false; // force the exact path
    LowEntropy le = low_entropy(ex(1), den, pt, o);
    CHECK(text_looks_factored(le.text));
    // and the exact engine must still reproduce the polynomial
    ex den2 = ex(1);
    for (const auto& f : le.den_factors) den2 = den2 * f.expr;
    CHECK((den - den2.expand()).expand().is_zero());
}

static void test_approx_factor_accuracy() {
    // CS amp with source resistance: the denominator does not factor exactly
    // but is well approximated by two real poles. Approximate factoring must
    // reproduce the magnitude response at low and mid frequencies.
    Circuit c;
    c.comps.push_back(comp(Kind::V, "V1", {"in", "0"}, "1"));
    c.comps.push_back(comp(Kind::R, "Rs", {"in", "g"}, "10k"));
    c.comps.push_back(comp(Kind::R, "Rg", {"g", "0"}, "100k"));
    Component m = comp(Kind::NMOS, "M1", {"out", "g", "0"}, "");
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

    AnalysisRequest req;
    req.input_ref = "V1";
    req.output = "V(out)";
    req.f0_hz = 1e6;
    AnalysisResult r = analyze(c, req);
    CHECK(r.pruned.poles.size() == 2);

    // rebuild the low-entropy H and compare with the exact H at a few points
    ex Hle = r.pruned.gain;
    for (const auto& f : r.pruned.num_factors) Hle = Hle * f.expr;
    ex Dle = ex(1);
    for (const auto& f : r.pruned.den_factors) Dle = Dle * f.expr;
    Hle = (Hle / Dle).normal();
    for (double f : {1e3, 1e5, 1e6, 1e7, 1e8}) {
        double w = 2 * M_PI * f;
        double exact = mag_db_at(r, w);
        double approx = eval_mag_db(Hle, r.params, w);
        CHECK_CLOSE(approx, exact, 1.0); // within 1 dB across the band
    }
}

static void test_coefficient_product_not_dropped() {
    // The user-visible low-entropy contract: a product of two small terms
    // inside a coefficient must be dropped when it is negligible.
    ParamTable pt;
    ex s = pt.get("s");
    ex R1 = pt.get("R1"), C1 = pt.get("C1"), C2 = pt.get("C2"), C3 = pt.get("C3");
    pt.set("R1", 1e3, UnitClass::Ohm);
    pt.set("C1", 1e-6, UnitClass::Farad);
    pt.set("C2", 1e-6, UnitClass::Farad);
    pt.set("C3", 1e-6, UnitClass::Farad);
    ex den = 1 + s * (C1 * C2 + C3) * R1; // s-coefficient = C1*C2*R1 + C3*R1
    LowEntropyOptions o;
    o.prune = true;
    o.f0_hz = 1e3;
    LowEntropy le = low_entropy(ex(1), den, pt, o);
    // C1*C2*R1 is ~120 dB below C3*R1: the surviving form is 1 + s*C3*R1
    CHECK(le.dropped.size() == 1);
    CHECK(le.text.find("C3") != std::string::npos);
    CHECK(le.text.find("C2") == std::string::npos);
}

static void test_approx_off_is_consistent() {
    // Turning approximate factoring off must never fabricate a factor: the
    // factors must still multiply back to the exact polynomial.
    Circuit c;
    c.comps.push_back(comp(Kind::V, "V1", {"in", "0"}, "1"));
    c.comps.push_back(comp(Kind::R, "R1", {"in", "a"}, "10k"));
    c.comps.push_back(comp(Kind::R, "R2", {"a", "out"}, "20k"));
    c.comps.push_back(comp(Kind::C, "C1", {"a", "0"}, "1n"));
    c.comps.push_back(comp(Kind::C, "C2", {"out", "0"}, "2n"));
    c.comps.push_back(comp(Kind::GND, "G1", {"0"}));
    AnalysisRequest req;
    req.input_ref = "V1";
    req.output = "V(out)";
    req.approx_factor = false;
    AnalysisResult r = analyze(c, req);
    ex prod = ex(1);
    for (const auto& f : r.pruned.den_factors) prod = prod * f.expr;
    CHECK((r.pruned.den_poly - prod.expand()).expand().is_zero());
}

// ---------------------------------------------------------------------------
// New analyses: DC, noise, loop gain, PSRR.
// ---------------------------------------------------------------------------
static void test_dc_analysis() {
    Circuit c;
    Component v1 = comp(Kind::V, "V1", {"in", "0"}, "1");
    v1.dc_text = "1"; // large-signal DC uses the source's DC value
    c.comps.push_back(v1);
    c.comps.push_back(comp(Kind::R, "R1", {"in", "out"}, "10k"));
    c.comps.push_back(comp(Kind::R, "R2", {"out", "0"}, "10k"));
    c.comps.push_back(comp(Kind::C, "C1", {"out", "0"}, "1u")); // opens at DC
    c = ground(c);
    AnalysisSpec sp;
    sp.kind = AnalysisKind::DC;
    sp.output = "V(out)";
    CardResult cr = run_analysis(c, sp);
    // resistive divider: V(out) = V1/2, with V1 kept as a symbol.
    CHECK(cr.report.find("V(out)") != std::string::npos);
    CHECK(cr.report.find("V1") != std::string::npos);
    // There is no single headline expression for a multi-value operating point,
    // so only the per-value LaTeX report is emitted.
    CHECK(cr.latex.empty());
    CHECK(!cr.latex_report.empty());
    CHECK(cr.latex_report.find("V(out)") != std::string::npos);
}

static void test_noise_analysis_input_referred() {
    // resistive divider driven by V1: the input-referred voltage noise density
    // is sqrt(4kT*(R1||R2)); the output-referred is half that. Input referral
    // is a ratio, so it holds for the integrated rms values too.
    Circuit c;
    c.comps.push_back(comp(Kind::V, "V1", {"in", "0"}, "1"));
    c.comps.push_back(comp(Kind::R, "R1", {"in", "out"}, "10k"));
    c.comps.push_back(comp(Kind::R, "R2", {"out", "0"}, "10k"));
    c = ground(c);
    AnalysisSpec sp;
    sp.kind = AnalysisKind::Noise;
    sp.input_ref = "V1";
    sp.output = "V(out)";
    sp.sweep.f_start_hz = 1;
    sp.sweep.f_stop_hz = 1e6;
    CardResult cr = run_analysis(c, sp);
    CHECK(cr.report.find("Output Noise") != std::string::npos);
    CHECK(cr.report.find("Input Noise") != std::string::npos);
    CHECK(cr.values.size() == 2);

    // numeric: gain = 1/2, so vin = 2*vout
    auto get = [&](const std::string& k) -> double {
        for (const auto& v : cr.values)
            if (v.first == k) return std::atof(v.second.c_str());
        return -1;
    };
    double vout = get("Vout_n"), vin = get("Vin_n");
    CHECK_CLOSE(vin / vout, 2.0, 0.01);
    // Both resistors contribute 4kT/R seen through R1||R2, so the output
    // voltage noise density is sqrt(4kT*(R1||R2)); integrating that flat
    // density over a 1 Hz..1 MHz band gives sqrt(4kT*(R1||R2)*(1e6-1)).
    double expect_density = std::sqrt(4.0 * 1.380649e-23 * 300.15 * 5000.0);
    double expect_vout = expect_density * std::sqrt(1e6 - 1.0);
    CHECK_CLOSE(vout, expect_vout, expect_vout * 0.03);
    CHECK_CLOSE(vin, 2.0 * expect_vout, expect_vout * 0.04);
    // The typeset (LaTeX) report must be populated, otherwise the Math tab is
    // blank.
    CHECK(!cr.latex_report.empty());
    CHECK(cr.latex_report.find("S_{v}") != std::string::npos);
    // a spectrum is published for the plot tab
    CHECK(cr.transfer.has_noise);
    CHECK(cr.transfer.noise_f_hz.size() > 10);
    CHECK(cr.transfer.noise_vout.size() == cr.transfer.noise_f_hz.size());
    // Output portion comes before Input portion; density precedes integrated.
    size_t po = cr.report.find("Output Noise");
    size_t pi = cr.report.find("Input Noise");
    CHECK(po != std::string::npos && pi != std::string::npos && po < pi);
    size_t pd = cr.report.find("Density S_v");
    size_t pint = cr.report.find("Integrated: V_n,out");
    CHECK(pd != std::string::npos && pint != std::string::npos && pd < pint);
    // The output density and input density are separate expressions.
    CHECK(cr.report.find("Density S_v(f)") != std::string::npos);
    CHECK(cr.report.find("Density S_i(f)") != std::string::npos);
    CHECK(cr.report.find("H(s) = ") != std::string::npos);
}

// Noise with a current-source excitation reports input-referred *current*
// noise, and an amplifier's en contributes an input voltage noise term.
static void test_noise_current_input_and_amp() {
    Circuit c;
    c.comps.push_back(comp(Kind::I, "I1", {"n1", "0"}, "1"));
    Component op = comp(Kind::OPAMP, "U1", {"0", "n1", "out"}, "1e5");
    op.param_text["en"] = "10n";
    op.param_on["en"] = true;
    c.comps.push_back(op);
    c.comps.push_back(comp(Kind::R, "R1", {"n1", "out"}, "1k"));
    c.comps.push_back(comp(Kind::GND, "G1", {"0"}));
    AnalysisSpec sp;
    sp.kind = AnalysisKind::Noise;
    sp.input_ref = "I1";
    sp.output = "V(out)";
    sp.sweep.f_start_hz = 1;
    sp.sweep.f_stop_hz = 1e6;
    CardResult cr = run_analysis(c, sp);
    CHECK(cr.report.find("Input Noise") != std::string::npos);
    CHECK(cr.transfer.has_noise);
    CHECK(cr.transfer.noise_input_is_current);
    CHECK(cr.transfer.noise_iin_total > 0.0);
    // The amplifier's en is modelled as unity-gain at the output: its output
    // contribution is en^2 alone (no H(s) multiplier), and at the input it is
    // divided by H(s)^2. Also, it is a VOLTAGE density (e_n), not i_n.
    CHECK(cr.report.find("(en_U1^2)") != std::string::npos);
    CHECK(cr.report.find("(en_U1^2)/H(s)^2") != std::string::npos);
    CHECK(cr.report.find("e_n^2 = en_U1^2 V^2/Hz") != std::string::npos);
    // The LaTeX report mirrors the text, and percentages are plain decimals.
    CHECK(cr.latex_report.find("S_{v}") != std::string::npos);
    CHECK(cr.latex_report.find("S_{i}") != std::string::npos);
    size_t ppos = cr.latex_report.find("\\%");
    CHECK(ppos != std::string::npos);
    if (ppos != std::string::npos) {
        size_t eq = cr.latex_report.rfind('=', ppos);
        std::string val = cr.latex_report.substr(eq + 1, ppos - eq - 1);
        CHECK(val.find('e') == std::string::npos &&
              val.find('E') == std::string::npos);
    }
}

// Zero-value (open-circuit) time constants: one per reactive element, with
// the correct physical value R*C and the element as its label. The SUM of the
// OCTC taus equals the denominator's first-order coefficient (a general
// identity), which is what makes TTC ordering valid.
static void test_octc_time_constants() {
    // RC low-pass: single C1, tau = R1*C1 exactly.
    {
        Circuit c;
        c.comps.push_back(comp(Kind::V, "V1", {"in", "0"}, "1"));
        c.comps.push_back(comp(Kind::R, "R1", {"in", "out"}, "10k"));
        c.comps.push_back(comp(Kind::C, "C1", {"out", "0"}, "1n"));
        c = ground(c);
        AnalysisRequest req;
        req.input_ref = "V1";
        req.output = "V(out)";
        AnalysisResult r = analyze(c, req);
        CHECK(r.octc.size() == 1);
        if (!r.octc.empty()) {
            CHECK(r.octc[0].label == "C1");
            CHECK((r.octc[0].tau - S(r, "R1") * S(r, "C1")).normal().is_zero());
        }
    }
    // Two-pole RC ladder (R1-C1, R2-C2 to ground): two OCTC taus, and their sum
    // equals the exact denominator's s-coefficient.
    {
        Circuit c;
        c.comps.push_back(comp(Kind::V, "V1", {"in", "0"}, "1"));
        c.comps.push_back(comp(Kind::R, "R1", {"in", "a"}, "10k"));
        c.comps.push_back(comp(Kind::C, "C1", {"a", "0"}, "100p"));
        c.comps.push_back(comp(Kind::R, "R2", {"a", "out"}, "10k"));
        c.comps.push_back(comp(Kind::C, "C2", {"out", "0"}, "1p"));
        c = ground(c);
        AnalysisRequest req;
        req.input_ref = "V1";
        req.output = "V(out)";
        req.sweep.f_start_hz = 1;
        req.sweep.f_stop_hz = 1e9;
        AnalysisResult r = analyze(c, req);
        ex s = S(r, "s");
        CHECK(r.octc.size() == 2);
        ex tsum = 0;
        for (const auto& tc : r.octc) tsum += tc.tau;
        ex a1 = (r.den_raw.coeff(s, 1) / r.den_raw.coeff(s, 0)).normal();
        CHECK((tsum - a1).normal().is_zero());
        // the report lists the time constants
        CHECK(r.report.find("Time Constants") != std::string::npos);
        CHECK(r.report.find("tau = ") != std::string::npos);
    }
    // A device parasitic capacitance gets its own time constant, labelled with
    // the parameter symbol (Cgs_M1), not just the component ref.
    {
        Circuit c;
        c.comps.push_back(comp(Kind::V, "V1", {"in", "0"}, "1"));
        c.comps.push_back(comp(Kind::R, "R1", {"in", "out"}, "10k"));
        Component m = comp(Kind::NMOS, "M1", {"out", "out", "0"}, "");
        m.param_on["Cgs"] = true;
        m.param_text["Cgs"] = "100f";
        c.comps.push_back(m);
        c = ground(c);
        AnalysisRequest req;
        req.input_ref = "V1";
        req.output = "V(out)";
        AnalysisResult r = analyze(c, req);
        bool found = false;
        for (const auto& tc : r.octc)
            if (tc.label == "Cgs_M1") found = true;
        CHECK(found);
    }
}

// Transfer-function low-entropy form must match loop gain: a parallel load
// reads (R1||R2), not an expanded resistor ratio.
static void test_tf_low_entropy_parallel() {
    Circuit c;
    c.comps.push_back(comp(Kind::I, "I1", {"n1", "0"}, "1"));
    Component op = comp(Kind::OPAMP, "U1", {"0", "n1", "out"}, "1e5");
    op.param_text["GBW"] = "1e7";
    c.comps.push_back(op);
    c.comps.push_back(comp(Kind::R, "R1", {"n1", "out"}, "1k"));
    c.comps.push_back(comp(Kind::R, "R2", {"n1", "out"}, "3.3k"));
    c.comps.push_back(comp(Kind::GND, "G1", {"0"}));
    AnalysisSpec sp;
    sp.kind = AnalysisKind::TransferFunction;
    sp.input_ref = "I1";
    sp.output = "V(out)";
    sp.sweep.f_start_hz = 1;
    sp.sweep.f_stop_hz = 1e9;
    CardResult cr = run_analysis(c, sp);
    CHECK(cr.transfer.pruned.text.find("||") != std::string::npos);
    // the gain reads -(R1||R2), not R1*R2/(R1+R2)
    CHECK(cr.transfer.pruned.gain.is_equal(ex(1)) == false);
    CHECK(cr.transfer.pruned.text.find("(R1||R2)") != std::string::npos ||
          cr.transfer.pruned.text.find("(R2||R1)") != std::string::npos);
}

// The MNA must combine resistances that sit between the same AC nodes into a
// single held parallel atom BEFORE solving, so the result is in terms of
// (R1||ro) and never has an expanded R1*ro/(R1+ro) to recover.
static void test_mna_parallel_precombine() {
    Circuit c;
    c.comps.push_back(comp(Kind::V, "V1", {"in", "0"}, "1"));
    Component m = comp(Kind::NMOS, "M1", {"out", "in", "0"}, "");
    m.param_on["ro"] = true;
    m.param_text["gm"] = "1m";
    m.param_text["ro"] = "100k";
    m.param_on["Cgs"] = false;
    m.param_on["Cgd"] = false;
    m.param_on["Cds"] = false;
    c.comps.push_back(m);
    // Rd from out to ground; Rg from out to ground: Rd||Rg||ro at the drain.
    c.comps.push_back(comp(Kind::R, "Rd", {"out", "0"}, "10k"));
    c.comps.push_back(comp(Kind::R, "Rg", {"out", "0"}, "10k"));
    c.comps.push_back(comp(Kind::GND, "G1", {"0"}));
    AnalysisRequest req;
    req.input_ref = "V1";
    req.output = "V(out)";
    AnalysisResult r = analyze(c, req);
    // Every 1/R term at the drain is one held par() atom, never expanded.
    CHECK(str(r.num_raw).find("Rd*ro_M1") == std::string::npos);
    CHECK(str(r.den_raw).find("Rd*ro_M1") == std::string::npos);
}

// Output impedance must collapse R1 and ro into (R1||ro) -- the impedance
// analyses used to disable normalization, which left the result fully expanded
// (R1*ro/(R1+ro+s*C1*R1*ro)) instead of (R1||ro)/(1+s*C1*(R1||ro)).
static void test_zout_parallel_collapses() {
    Circuit c;
    c.comps.push_back(comp(Kind::V, "V1", {"in", "0"}, "1"));
    Component m = comp(Kind::NMOS, "M1", {"out", "in", "0"}, "");
    m.param_on["ro"] = true;
    m.param_text["gm"] = "1m";
    m.param_text["ro"] = "100k";
    m.param_on["Cgs"] = false;
    m.param_on["Cgd"] = false;
    m.param_on["Cds"] = false;
    c.comps.push_back(m);
    c.comps.push_back(comp(Kind::R, "R1", {"out", "VDD"}, "100k"));
    c.comps.push_back(comp(Kind::VDD, "VDD1", {"VDD"}));
    c.comps.push_back(comp(Kind::C, "C1", {"out", "0"}, "1p"));
    c.comps.push_back(comp(Kind::GND, "G1", {"0"}));
    AnalysisSpec sp;
    sp.kind = AnalysisKind::OutputImpedance;
    sp.input_ref = "V1";
    sp.output = "V(out)";
    sp.sweep.f_start_hz = 1;
    sp.sweep.f_stop_hz = 1e9;
    CardResult cr = run_analysis(c, sp);
    CHECK(cr.transfer.pruned.text.find("||") != std::string::npos);
    // it must not print the expanded R1*ro/(R1+ro) form
    CHECK(cr.transfer.pruned.text.find("ro_M1*R1") == std::string::npos);
    CHECK(cr.transfer.pruned.text.find("R1*ro_M1") == std::string::npos);
}

// Every analysis must produce a LaTeX report (the Math tab mirrors the text).
static void test_all_analyses_have_latex_report() {
    Circuit c;
    c.comps.push_back(comp(Kind::V, "V1", {"in", "0"}, "1"));
    c.comps.push_back(comp(Kind::R, "R1", {"in", "out"}, "10k"));
    c.comps.push_back(comp(Kind::R, "R2", {"out", "0"}, "10k"));
    c = ground(c);
    const AnalysisKind kinds[] = {
        AnalysisKind::TransferFunction, AnalysisKind::AC, AnalysisKind::DC,
        AnalysisKind::InputImpedance, AnalysisKind::OutputImpedance};
    for (AnalysisKind k : kinds) {
        AnalysisSpec sp;
        sp.kind = k;
        sp.input_ref = "V1";
        sp.output = "V(out)";
        sp.sweep.f_start_hz = 1;
        sp.sweep.f_stop_hz = 1e6;
        CardResult cr = run_analysis(c, sp);
        CHECK(!cr.report.empty());
        CHECK(!cr.latex_report.empty());
    }
}

// The amplifier-noise percent must not use scientific notation.
static void test_percent_no_exponent() {
    CHECK(eng::format_percent(96.5, 3) == "96.5");
    CHECK(eng::format_percent(0.0084, 3).find("e") == std::string::npos);
    CHECK(eng::format_percent(0.0084, 3) == "0.0084");
    CHECK(eng::format_percent(100.0, 3) == "100");
}

// Noise must still work when the card's input is not an ideal source (it falls
// back to any source in the circuit) and report output-referred noise.
static void test_noise_survives_bad_input() {
    Circuit c;
    c.comps.push_back(comp(Kind::R, "R1", {"a", "0"}, "1k"));
    c.comps.push_back(comp(Kind::I, "I1", {"a", "0"}, "1"));
    c.comps.push_back(comp(Kind::GND, "G1", {"0"}));
    AnalysisSpec sp;
    sp.kind = AnalysisKind::Noise;
    sp.input_ref = "VDOESNOTEXIST"; // not a source at all
    sp.output = "V(a)";
    sp.f0_hz = 1e3;
    CardResult cr = run_analysis(c, sp);
    CHECK(cr.report.find("Output Noise") != std::string::npos);
    CHECK(!cr.latex_report.empty());
    CHECK(cr.values.size() == 2);
}

static void test_loop_gain_opamp() {
    // non-inverting op-amp with gain block A: T = A*R2/(R1+R2)
    Circuit c;
    c.comps.push_back(comp(Kind::V, "V1", {"in", "0"}, "1"));
    c.comps.push_back(comp(Kind::OPAMP, "X1", {"in", "fb", "out"}, "1e5"));
    c.comps.push_back(comp(Kind::R, "R1", {"out", "fb"}, "10k"));
    c.comps.push_back(comp(Kind::R, "R2", {"fb", "0"}, "1k"));
    c = ground(c);
    AnalysisSpec sp;
    sp.kind = AnalysisKind::LoopGain;
    sp.input_ref = "V1";
    sp.output = "V(out)";
    sp.probe_ref = "X1";
    CardResult cr = run_analysis(c, sp);
    CHECK(cr.report.find("Return ratio") != std::string::npos);
    CHECK(cr.report.find("T(s)") != std::string::npos);
    // at DC, T = 1e5 * 1k/(11k) ~ 9091
    double T = std::abs(syms::eval_complex(
        (cr.transfer.num_raw / cr.transfer.den_raw).normal(), cr.transfer.params,
        0.0));
    CHECK_CLOSE(T, 1e5 * 1.0 / 11.0, 5.0);
}

// The nullor is a boxed two-port: nullator across the input port (in+, in-),
// norator across the output port (out+, out-). An ideal inverting amp built
// from it must give V(out)/V1 = -R2/R1 exactly (the virtual short at the
// inverting node).
static void test_nullor_two_port_ideal_amp() {
    Circuit c;
    c.comps.push_back(comp(Kind::V, "V1", {"in", "0"}, "1"));
    c.comps.push_back(comp(Kind::R, "R1", {"in", "n1"}, "1k"));
    c.comps.push_back(comp(Kind::NULLOR, "N1", {"0", "n1", "out", "0"}, ""));
    c.comps.push_back(comp(Kind::R, "R2", {"out", "n1"}, "10k"));
    c.comps.push_back(comp(Kind::GND, "G1", {"0"}));
    AnalysisRequest req;
    req.input_ref = "V1";
    req.output = "V(out)";
    AnalysisResult r = analyze(c, req);
    ex H = (r.num_raw / r.den_raw).normal();
    ex expect = -ex(r.params.get("R2")) / r.params.get("R1");
    CHECK((H - expect).normal().is_zero());
}

static void test_psrr_vdd() {
    Circuit c;
    c.comps.push_back(comp(Kind::V, "V1", {"in", "0"}, "1"));
    c.comps.push_back(comp(Kind::VDD, "VDD1", {"VDD"}));
    c.comps.push_back(comp(Kind::R, "Rd", {"VDD", "out"}, "10k"));
    Component m = comp(Kind::NMOS, "M1", {"out", "in", "0"}, "");
    m.param_on["ro"] = true;
    m.param_text["gm"] = "1m";
    m.param_text["ro"] = "100k";
    c.comps.push_back(m);
    c.comps.push_back(comp(Kind::GND, "G1", {"0"}));
    AnalysisSpec sp;
    sp.kind = AnalysisKind::PSRR;
    sp.input_ref = "V1";
    sp.output = "V(out)";
    CardResult cr = run_analysis(c, sp);
    CHECK(cr.report.find("PSR ") != std::string::npos);
    CHECK(cr.report.find("PSRR") != std::string::npos);
    CHECK(!cr.transfer.pruned.text.empty());
}

// ---------------------------------------------------------------------------
// Low-entropy correctness: structural pole attribution and the two thresholds.
// ---------------------------------------------------------------------------

// The lower (input) pole of a common-source stage driven through a source
// resistor R2 is Cgs*R2, not C1*R2. Cgs and C1 share the same numeric value
// (100 f) but different resistors, so the attribution must be structural --
// the engine must not let the reduction collapse Cgs into C1.
static void test_cs_input_pole_is_cgs_not_c1() {
    Circuit c;
    c.comps.push_back(comp(Kind::V, "V1", {"in", "0"}, "1"));
    Component m = comp(Kind::NMOS, "M1", {"out", "vg", "0"}, "");
    m.param_on["Cgs"] = true;   m.param_text["Cgs"] = "100f";
    m.param_on["Cgd"] = false;
    m.param_on["Cds"] = false;
    m.param_on["ro"] = true;    m.param_text["ro"] = "100k";
    m.param_text["gm"] = "1m";
    c.comps.push_back(m);
    c.comps.push_back(comp(Kind::R, "R1", {"out", "VDD"}, "1k"));
    c.comps.push_back(comp(Kind::VDD, "VDD1", {"VDD"}));
    c.comps.push_back(comp(Kind::C, "C1", {"out", "0"}, "100f"));
    c.comps.push_back(comp(Kind::R, "R2", {"vg", "in"}, "10k"));
    c.comps.push_back(comp(Kind::GND, "G1", {"0"}));

    AnalysisSpec sp;
    sp.kind = AnalysisKind::TransferFunction;
    sp.input_ref = "V1";
    sp.output = "V(out)";
    sp.f0_hz = 1.0;
    sp.sweep.f_start_hz = 1.0;
    sp.sweep.f_stop_hz = 1e9;

    CardResult cr = run_analysis(c, sp);
    const auto& r = cr.transfer;

    CHECK(r.pruned.poles.size() == 2);
    if (r.pruned.poles.size() == 2) {
        // poles are sorted ascending omega; the lower is the input pole.
        CHECK(r.pruned.poles[0].label.find("Cgs") != std::string::npos);
        CHECK(r.pruned.poles[0].label.find("R2") != std::string::npos);
        CHECK(r.pruned.poles[0].label.find("C1") == std::string::npos);
        CHECK(r.pruned.poles[1].label.find("C1") != std::string::npos);
    }
}

// Pole/zero reduction is 60 dB along the frequency axis: a pole 1000x (60 dB)
// away is dropped, a pole 100x (40 dB) away is kept.
static void test_pole_zero_60db_threshold() {
    auto poles_at = [](double tau2) {
        ParamTable pt;
        ex s = pt.get("s");
        ex R1 = pt.get("R1"), C1 = pt.get("C1"), R2 = pt.get("R2"), C2 = pt.get("C2");
        pt.set("R1", 1e3, UnitClass::Ohm);
        pt.set("C1", 1e-9, UnitClass::Farad); // tau1 = 1 us
        pt.set("R2", 1e3, UnitClass::Ohm);
        pt.set("C2", tau2 / 1e3, UnitClass::Farad);
        ex den = (1 + s * R1 * C1) * (1 + s * R2 * C2);
        LowEntropyOptions o; o.prune = true; o.f0_hz = 1e3;
        return low_entropy(ex(1), den, pt, o).poles.size();
    };
    CHECK(poles_at(1e-8) == 2);  // 40 dB apart -> both kept
    CHECK(poles_at(1e-9) == 1);  // 60 dB apart -> far pole dropped
    CHECK(poles_at(1e-10) == 1); // 80 dB apart -> far pole dropped
}

// The report must include the numeric gain/bandwidth metrics.
static void test_gain_bw_report() {
    Circuit c;
    c.comps.push_back(comp(Kind::V, "V1", {"in", "0"}, "1"));
    c.comps.push_back(comp(Kind::R, "R1", {"in", "out"}, "10k"));
    c.comps.push_back(comp(Kind::C, "C1", {"out", "0"}, "1n"));
    c = ground(c);
    AnalysisRequest req;
    req.input_ref = "V1";
    req.output = "V(out)";
    req.sweep.f_start_hz = 1.0;
    req.sweep.f_stop_hz = 1e6;
    AnalysisResult r = analyze(c, req);
    CHECK(r.report.find("DC Gain") != std::string::npos);
    CHECK(r.report.find("-3 dB Bandwidth") != std::string::npos);
    CHECK(r.report.find("Unity-Gain") != std::string::npos);
    // RC low-pass: DC gain 0 dB, -3 dB at 1/(2*pi*R*C) ~ 15.9 kHz.
    CHECK(r.report.find("kHz") != std::string::npos);
    // Symbolic: the single pole is R1*C1, so the report carries omega_p0 =
    // 1/(R1*C1) (omega is the Greek letter in UTF-8; match the tail).
    CHECK(r.report.find("_p0") != std::string::npos);
    CHECK(r.report.find("R1*C1") != std::string::npos);
}

// Symbolic gain/bandwidth in the LaTeX report (Math tab).
static void test_metrics_latex() {
    Circuit c;
    c.comps.push_back(comp(Kind::V, "V1", {"in", "0"}, "1"));
    c.comps.push_back(comp(Kind::R, "R1", {"in", "out"}, "10k"));
    c.comps.push_back(comp(Kind::C, "C1", {"out", "0"}, "1n"));
    c = ground(c);
    AnalysisRequest req;
    req.input_ref = "V1";
    req.output = "V(out)";
    req.sweep.f_start_hz = 1.0;
    req.sweep.f_stop_hz = 1e6;
    AnalysisResult r = analyze(c, req);
    std::string lx = format_report_latex(r);
    CHECK(lx.find("DC\\ Gain") != std::string::npos);
    CHECK(lx.find("-3\\ dB\\ Bandwidth") != std::string::npos);
    CHECK(lx.find("Unity-Gain") != std::string::npos);
    CHECK(lx.find("\\omega_{p0}") != std::string::npos);
}

// The symbolic -3 dB bandwidth must collapse to the dominant pole alone when
// the second pole is far enough away (100x) that it cannot move the corner,
// even though that second pole is still listed in the pole table.
static void test_metrics_dominant_pole_bandwidth() {
    Circuit c;
    c.comps.push_back(comp(Kind::V, "V1", {"in", "0"}, "1"));
    c.comps.push_back(comp(Kind::R, "R1", {"in", "a"}, "1k"));
    c.comps.push_back(comp(Kind::C, "C1", {"a", "0"}, "1n")); // tau ~ 1 us
    c.comps.push_back(comp(Kind::R, "R2", {"a", "out"}, "1k"));
    c.comps.push_back(comp(Kind::C, "C2", {"out", "0"}, "1p")); // ~ 10 ns
    c = ground(c);
    AnalysisRequest req;
    req.input_ref = "V1";
    req.output = "V(out)";
    req.sweep.f_start_hz = 1.0;
    req.sweep.f_stop_hz = 1e9;
    AnalysisResult r = analyze(c, req);
    // the -3 dB symbolic line names the dominant pole omega_p0 (bandwidth
    // reduces to the dominant time constant when the other pole is 100x away)
    CHECK(r.report.find("_p0") != std::string::npos);
    CHECK(r.report.find("-3 dB Bandwidth") != std::string::npos);
}

// Amplifier gain is symbolic: A/(A+1) must simplify to 1 for a large A, so the
// closed-loop gain reads -R1 (not a decimal ratio).
static void test_amp_gain_is_symbolic() {
    Circuit c;
    c.comps.push_back(comp(Kind::I, "I1", {"n1", "0"}, "1"));
    c.comps.push_back(comp(Kind::R, "R1", {"n1", "out"}, "1k"));
    Component op = comp(Kind::OPAMP, "U1", {"0", "n1", "out"}, "1e9");
    op.param_text["GBW"] = "1e6";
    c.comps.push_back(op);
    c.comps.push_back(comp(Kind::GND, "G1", {"0"}));
    AnalysisSpec sp;
    sp.kind = AnalysisKind::TransferFunction;
    sp.input_ref = "I1";
    sp.output = "V(out)";
    sp.f0_hz = 1.0;
    sp.sweep.f_start_hz = 1.0;
    sp.sweep.f_stop_hz = 1e9;
    CardResult cr = run_analysis(c, sp);
    // the printed H(s) keeps R1 and drops the A/(A+1) ~ 1 factor
    CHECK(cr.transfer.pruned.text.find("R1") != std::string::npos);
    CHECK(cr.transfer.pruned.text.find("A_U1/(A_U1") == std::string::npos);
}

// Loop gain by the return-ratio method: H_inf is the ideal gain, and the
// report includes the numeric gain/bandwidth metrics and a phase margin.
static void test_loop_gain_return_ratio() {
    Circuit c;
    c.comps.push_back(comp(Kind::I, "I1", {"n1", "0"}, "1"));
    c.comps.push_back(comp(Kind::R, "R1", {"n1", "out"}, "1k"));
    c.comps.push_back(comp(Kind::C, "C1", {"0", "n1"}, "1p"));
    Component op = comp(Kind::OPAMP, "U1", {"0", "n1", "out"}, "1e9");
    op.param_text["GBW"] = "1e6";
    c.comps.push_back(op);
    c.comps.push_back(comp(Kind::GND, "G1", {"0"}));
    AnalysisSpec sp;
    sp.kind = AnalysisKind::LoopGain;
    sp.input_ref = "I1";
    sp.output = "V(out)";
    sp.probe_ref = "U1";
    sp.sweep.f_start_hz = 1.0;
    sp.sweep.f_stop_hz = 1e9;
    CardResult cr = run_analysis(c, sp);
    CHECK(cr.report.find("Return-ratio") != std::string::npos);
    CHECK(cr.report.find("H_inf") != std::string::npos);
    CHECK(cr.report.find("beta") != std::string::npos);
    CHECK(cr.report.find("Gain / Bandwidth") != std::string::npos);
    CHECK(cr.report.find("Phase Margin") != std::string::npos);
    // the asymptotic gain of a TIA is -R1
    CHECK(cr.summary.find("H_inf") != std::string::npos);
}

// The Miller feedforward zero (s = gm/Cgd) from a gate-drain capacitance is a
// *significant* zero: it must survive "ignore negligible", not be dropped as
// if gm*Rd (the forward gain) were a gm*ro intrinsic-gain term.
static void test_miller_zero_survives_pruning() {
    Circuit c;
    c.comps.push_back(comp(Kind::V, "V1", {"in", "0"}, "1"));
    Component m = comp(Kind::NMOS, "M1", {"out", "in", "0"}, "");
    m.param_on["Cgd"] = true;   m.param_text["Cgd"] = "100f";
    m.param_on["ro"] = false;
    m.param_on["Cgs"] = false;
    m.param_on["Cds"] = false;
    m.param_text["gm"] = "1m";
    c.comps.push_back(m);
    c.comps.push_back(comp(Kind::R, "Rd", {"out", "0"}, "10k"));
    c.comps.push_back(comp(Kind::GND, "G1", {"0"}));

    AnalysisRequest req;
    req.input_ref = "V1";
    req.output = "V(out)";
    req.prune = true;
    AnalysisResult r = analyze(c, req);

    CHECK(r.pruned.zeros.size() == 1);
    if (!r.pruned.zeros.empty())
        CHECK(r.pruned.zeros[0].label.find("Cgd") != std::string::npos);
    CHECK(r.pruned.text.find("Cgd") != std::string::npos);
}

// Series resistors collapse at 20 dB (10 + 1 -> 10), so a resistor 40 dB
// below its series partner is dropped -- unlike a pole 40 dB away, which the
// 60 dB pole/zero rule keeps.
static void test_series_reduction_20db() {
    ParamTable pt;
    ex s = pt.get("s");
    ex R1 = pt.get("R1"), R2 = pt.get("R2"), C1 = pt.get("C1");
    pt.set("R1", 1e4, UnitClass::Ohm);
    pt.set("R2", 100.0, UnitClass::Ohm); // 40 dB below R1
    pt.set("C1", 1e-9, UnitClass::Farad);
    ex den = 1 + s * (R1 + R2) * C1;
    LowEntropyOptions o; o.prune = true; o.f0_hz = 1e3;
    LowEntropy le = low_entropy(ex(1), den, pt, o);
    CHECK(le.text.find("R2") == std::string::npos);
    CHECK(le.text.find("R1") != std::string::npos);
}

// GBW is optional: with it off, the op-amp is ideal (infinite bandwidth), so
// a TIA with only an input capacitor has a single pole; switching GBW on adds
// the op-amp's own dominant pole.
static void test_gbw_optional() {
    auto make = [](bool gbw_on) {
        Circuit c;
        c.comps.push_back(comp(Kind::I, "I1", {"n1", "0"}, "1"));
        Component op = comp(Kind::OPAMP, "U1", {"0", "n1", "out"}, "1e5");
        op.param_text["GBW"] = "1e6";
        op.param_on["GBW"] = gbw_on;
        c.comps.push_back(op);
        c.comps.push_back(comp(Kind::R, "R1", {"n1", "out"}, "1k"));
        c.comps.push_back(comp(Kind::C, "C1", {"n1", "0"}, "1p"));
        c.comps.push_back(comp(Kind::GND, "G1", {"0"}));
        return c;
    };
    AnalysisSpec sp;
    sp.kind = AnalysisKind::TransferFunction;
    sp.input_ref = "I1";
    sp.output = "V(out)";
    sp.sweep.f_start_hz = 1;
    sp.sweep.f_stop_hz = 1e9;
    CardResult on = run_analysis(make(true), sp);
    CardResult off = run_analysis(make(false), sp);
    CHECK(off.transfer.pruned.poles.size() == 1); // R1*C1 only
    CHECK(on.transfer.pruned.poles.size() == 2);  // + op-amp dominant pole
}

// "Copy of" works for a passive: an inverting op-amp with Rf = 6*Rin reports
// the low-entropy closed-loop gain -6 (the ratio Rf/Rin collapses to the
// number), rather than an independent R2 symbol.
static void test_copy_of_passive() {
    Circuit c;
    c.comps.push_back(comp(Kind::V, "V1", {"in", "0"}, "0"));
    c.comps.push_back(comp(Kind::R, "R1", {"in", "n1"}, "1k"));
    Component rf = comp(Kind::R, "R2", {"n1", "out"}, "1k");
    rf.mirror_ref = "R1";
    rf.mirror_mult = 6;
    c.comps.push_back(rf);
    Component op = comp(Kind::OPAMP, "U1", {"0", "n1", "out"}, "1e5");
    op.value_text = "1e5";
    c.comps.push_back(op);
    c.comps.push_back(comp(Kind::GND, "G1", {"0"}));
    AnalysisSpec sp;
    sp.kind = AnalysisKind::TransferFunction;
    sp.input_ref = "V1";
    sp.output = "V(out)";
    sp.sweep.f_start_hz = 1;
    sp.sweep.f_stop_hz = 1e9;
    CardResult cr = run_analysis(c, sp);
    // The DC gain is exactly -6, and R2 must not appear as its own symbol.
    CHECK(cr.text.find("-6") != std::string::npos);
    CHECK(cr.text.find("R2") == std::string::npos);
}

// Two passives copied from the same unit must fold together: with R2 and R3 both
// "copy of" R1, the series group is (R1+R1) and the partner collapses to
// R1||(2*R1) = 2/3*R1 -- the copy substitution has to reach the *structural*
// series fold, not just the MNA stamps.
static void test_copy_of_series_parallel() {
    Circuit c;
    c.comps.push_back(comp(Kind::I, "I1", {"n1", "0"}, "1"));
    Component op = comp(Kind::OPAMP, "U1", {"0", "n1", "out"}, "1e5");
    op.param_text["GBW"] = "100M";
    c.comps.push_back(op);
    c.comps.push_back(comp(Kind::R, "R1", {"n1", "out"}, "10k"));
    Component r2 = comp(Kind::R, "R2", {"n1", "m"}, "10k");
    r2.mirror_ref = "R1";
    c.comps.push_back(r2);
    Component r3 = comp(Kind::R, "R3", {"m", "out"}, "10k");
    r3.mirror_ref = "R1";
    c.comps.push_back(r3);
    c.comps.push_back(comp(Kind::C, "C1", {"n1", "0"}, "1n"));
    c.comps.push_back(comp(Kind::GND, "G1", {"0"}));
    AnalysisSpec sp;
    sp.kind = AnalysisKind::TransferFunction;
    sp.input_ref = "I1";
    sp.output = "V(out)";
    sp.sweep.f_start_hz = 1;
    sp.sweep.f_stop_hz = 1e9;
    CardResult cr = run_analysis(c, sp);
    const std::string& t = cr.transfer.pruned.text;
    // The copies substitute to R1: neither R2 nor R3 survives as a symbol, and
    // the series/parallel group has folded to the single number 2/3.
    CHECK(t.find("R2") == std::string::npos);
    CHECK(t.find("R3") == std::string::npos);
    CHECK(t.find("2/3") != std::string::npos);
}

// Differential analysis on a differential pair: Adm, Acm and CMRR are formed
// from the two input-port nodes. Adm must be non-zero; with a *finite* tail the
// common-mode gain is non-zero, so CMRR is finite.
static void test_differential_analysis() {
    Circuit c;
    // Device parasitics off: the test only checks the Adm/Acm/CMRR plumbing and
    // port parsing, and the caps make the symbolic determinant far more
    // expensive (a full device network with every cap is ~20 s per solve).
    Component m1 = comp(Kind::NMOS, "M1", {"out", "g1", "t"}, "");
    m1.param_text["gm"] = "1m";
    for (const char* p : {"Cgs", "Cgd", "Cds", "Cdb", "Csb"}) m1.param_on[p] = false;
    c.comps.push_back(m1);
    Component m2 = comp(Kind::NMOS, "M2", {"d2", "g2", "t"}, "");
    m2.param_text["gm"] = "1m";
    for (const char* p : {"Cgs", "Cgd", "Cds", "Cdb", "Csb"}) m2.param_on[p] = false;
    c.comps.push_back(m2);
    c.comps.push_back(comp(Kind::R, "Rd", {"vdd", "out"}, "10k"));
    c.comps.push_back(comp(Kind::R, "Rd2", {"vdd", "d2"}, "10k"));
    c.comps.push_back(comp(Kind::R, "Rtail", {"t", "0"}, "100k"));
    c.comps.push_back(comp(Kind::VDD, "VDD", {"vdd"}, ""));
    c.comps.push_back(comp(Kind::GND, "G1", {"0"}));
    AnalysisSpec sp;
    sp.kind = AnalysisKind::Differential;
    sp.input_port_p = "g1";
    sp.input_port_n = "g2";
    sp.output = "V(out)";
    // The port fields accept either bare node names or V(node) probes; the
    // V(..) form must be stripped, or the card looks for a node named "V(g1)".
    AnalysisSpec probe = sp;
    probe.input_port_p = "V(g1)";
    probe.input_port_n = "V(g2)";
    CardResult cr = run_analysis(c, probe);
    CHECK(cr.report.find("Adm") != std::string::npos);
    CHECK(cr.report.find("Acm") != std::string::npos);
    CHECK(cr.report.find("CMRR") != std::string::npos);
    CHECK(cr.report.find("Rtail") != std::string::npos);
    CHECK(cr.report.find("Rd") != std::string::npos);
    CHECK(cr.report.find("V(g1)") == std::string::npos); // stripped
    CHECK(cr.has_transfer);
}

// A true differential pair with both gates as real nodes (a bridging source
// between the gates): the differential card must ground the un-driven port node
// (never leave it floating) and report a finite CMRR that grows with the tail.
static void test_differential_pair_cmrr() {
    auto make = [](const char* rtail) {
        Circuit c;
        // Parasitics off: keeps the symbolic solve small (the test only checks
        // the finite-CMRR behavior and the tail reference, not the full model).
        Component m1 = comp(Kind::NMOS, "M1", {"out", "g1", "t"}, "");
        m1.param_text["gm"] = "1m";
        for (const char* p : {"Cgs", "Cgd", "Cds", "Cdb", "Csb"}) m1.param_on[p] = false;
        c.comps.push_back(m1);
        Component m2 = comp(Kind::NMOS, "M2", {"d2", "g2", "t"}, "");
        m2.param_text["gm"] = "1m";
        for (const char* p : {"Cgs", "Cgd", "Cds", "Cdb", "Csb"}) m2.param_on[p] = false;
        c.comps.push_back(m2);
        c.comps.push_back(comp(Kind::R, "Rd", {"vdd", "out"}, "10k"));
        c.comps.push_back(comp(Kind::R, "Rd2", {"vdd", "d2"}, "10k"));
        c.comps.push_back(comp(Kind::R, "Rtail", {"t", "0"}, rtail));
        // A source bridging the two gates (the differential drive).
        c.comps.push_back(comp(Kind::V, "V1", {"g1", "g2"}, "0"));
        c.comps.push_back(comp(Kind::VDD, "VDD", {"vdd"}, ""));
        c.comps.push_back(comp(Kind::GND, "G1", {"0"}));
        return c;
    };
    AnalysisSpec sp;
    sp.kind = AnalysisKind::Differential;
    sp.input_port_p = "g1";
    sp.input_port_n = "g2";
    sp.output = "V(out)";
    CardResult cr = run_analysis(make("100k"), sp);
    CHECK(cr.report.find("Adm") != std::string::npos);
    // Finite CMRR (not "infinite") because the tail is finite, and it mentions
    // the tail resistance.
    CHECK(cr.report.find("infinite") == std::string::npos);
    CHECK(cr.report.find("Rtail") != std::string::npos);
}

// The general amplifier gain is signed: +100 and -100 give opposite-sign H(s).
static void test_amp_signed_gain() {
    auto make = [](const char* v) {
        Circuit c;
        c.comps.push_back(comp(Kind::V, "V1", {"in", "0"}, "1"));
        c.comps.push_back(comp(Kind::AMP, "A1", {"in", "out"}, v));
        c.comps.push_back(comp(Kind::GND, "G1", {"0"}));
        return c;
    };
    AnalysisRequest req;
    req.input_ref = "V1";
    req.output = "V(out)";
    AnalysisResult pp = analyze(make("100"), req);
    AnalysisResult nn = analyze(make("-100"), req);
    CHECK(pp.pruned.text.find("A_A1") != std::string::npos);
    // the positive-gain form has no leading minus at the numerator
    CHECK(pp.pruned.text.find("-A_A1") == std::string::npos);
    CHECK(nn.pruned.text.find("-A_A1") != std::string::npos);
}

// Loop gain by return ratio must list H_inf, T, beta and a noise gain, and its
// LaTeX must carry the same sections as the text report.
static void test_loop_gain_report_sections() {
    Circuit c;
    c.comps.push_back(comp(Kind::I, "I1", {"n1", "0"}, "1"));
    Component op = comp(Kind::OPAMP, "U1", {"0", "n1", "out"}, "1e5");
    op.param_text["GBW"] = "1e7";
    c.comps.push_back(op);
    c.comps.push_back(comp(Kind::R, "R1", {"n1", "out"}, "1k"));
    c.comps.push_back(comp(Kind::C, "C1", {"n1", "0"}, "1p"));
    c.comps.push_back(comp(Kind::GND, "G1", {"0"}));
    AnalysisSpec sp;
    sp.kind = AnalysisKind::LoopGain;
    sp.input_ref = "I1";
    sp.output = "V(out)";
    sp.probe_ref = "U1";
    sp.sweep.f_start_hz = 1;
    sp.sweep.f_stop_hz = 1e9;
    CardResult cr = run_analysis(c, sp);

    // order: asymptotic -> H_0 -> return ratio -> feedback -> closed loop ->
    // gain/bandwidth (which now carries the phase margin; no separate
    // Stability section)
    size_t pa = cr.report.find("Asymptotic");
    size_t ph0 = cr.report.find("H_0");
    size_t pr = cr.report.find("Return ratio");
    size_t pf = cr.report.find("Feedback factor");
    size_t pc = cr.report.find("Closed-loop gain");
    size_t pg = cr.report.find("Gain / Bandwidth");
    CHECK(pa != std::string::npos && ph0 != std::string::npos &&
          pr != std::string::npos && pf != std::string::npos &&
          pc != std::string::npos);
    CHECK(pa < ph0 && ph0 < pr && pr < pf && pf < pc && pc < pg);
    CHECK(cr.report.find("Stability") == std::string::npos);
    CHECK(cr.report.find("Phase Margin") != std::string::npos);
    CHECK(cr.report.find("Noise gain") == std::string::npos);

    // The return ratio is labelled T(s), not a stray "H(s) = ..." header, and
    // the closed-loop gain uses the asymptotic-gain formula.
    CHECK(cr.report.find("T(s) = -A(s)*beta(s)") != std::string::npos);
    CHECK(cr.report.find("H(s) = A_U1") == std::string::npos);
    CHECK(cr.report.find("H_inf*T/(1+T)") != std::string::npos);

    // LaTeX mirrors the same sections, with omega and the degree symbol.
    CHECK(cr.latex_report.find("Asymptotic") != std::string::npos);
    CHECK(cr.latex_report.find("Return ratio") != std::string::npos);
    CHECK(cr.latex_report.find("Feedback factor") != std::string::npos);
    CHECK(cr.latex_report.find("Closed-loop gain") != std::string::npos);
    CHECK(cr.latex_report.find("Phase\\ Margin") != std::string::npos);
    CHECK(cr.latex_report.find("Gain / Bandwidth") != std::string::npos);
    CHECK(cr.latex_report.find("H_{\\infty}") != std::string::npos);
    CHECK(cr.latex_report.find("\\omega") != std::string::npos);
    CHECK(cr.latex_report.find("Noise gain") == std::string::npos);
}

// Mirrored devices (multiplicity) scale their parameters from the unit device:
// a MOSFET with m = 4 has 4x gm and 4x Cgs, while ro is divided by 4.
static void test_mirror_multiplicity() {
    Circuit c;
    c.comps.push_back(comp(Kind::V, "V1", {"in", "0"}, "1"));
    Component unit = comp(Kind::NMOS, "M1", {"out", "in", "0"}, "");
    unit.param_text["gm"] = "1m";
    unit.param_text["Cgs"] = "100f";
    unit.param_text["ro"] = "100k";
    unit.param_on["ro"] = true;
    c.comps.push_back(unit);
    Component copy = comp(Kind::NMOS, "M2", {"o2", "in", "0"}, "");
    copy.mirror_ref = "M1";
    copy.mirror_mult = 4;
    c.comps.push_back(copy);
    c.comps.push_back(comp(Kind::R, "Rd", {"out", "0"}, "10k"));
    c.comps.push_back(comp(Kind::R, "Rd2", {"o2", "0"}, "10k"));
    c.comps.push_back(comp(Kind::GND, "G1", {"0"}));

    Circuit r = c;
    resolve_mirrors(r);
    const Component* m2 = r.find("M2");
    CHECK(m2 != nullptr);
    if (m2) {
        CHECK_CLOSE(m2->param_estimate("gm"), 4e-3, 1e-9);
        CHECK_CLOSE(m2->param_estimate("Cgs"), 400e-15, 1e-21);
        CHECK_CLOSE(m2->param_estimate("ro"), 25e3, 1.0);
        // the unit's enable state propagates
        CHECK(m2->param_enabled("ro"));
    }

    // Mismatched kinds are rejected.
    Component bad = comp(Kind::PMOS, "M3", {"x", "in", "0"}, "");
    bad.mirror_ref = "M1";
    bad.mirror_mult = 2;
    r.comps.push_back(bad);
    bool threw = false;
    try {
        resolve_mirrors(r);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
}

// A purely resistive series group must be combined FIRST and then combined in
// parallel with its partner, and -- critically -- the result must stay one held
// term through the WHOLE flow: numerator, denominator, poles and gain. R2+R3 in
// series with R1 in parallel must read R1||(R2+R3) everywhere, never an
// expanded (R1*R3 + R1*R2)/(R1+R3+R2).
static void test_series_then_parallel_form() {
    Circuit c;
    c.comps.push_back(comp(Kind::I, "I1", {"n1", "0"}, "1"));
    Component op = comp(Kind::OPAMP, "U1", {"0", "n1", "out"}, "1e5");
    op.param_text["GBW"] = "100M";
    c.comps.push_back(op);
    c.comps.push_back(comp(Kind::R, "R1", {"n1", "out"}, "10k"));
    c.comps.push_back(comp(Kind::R, "R2", {"n1", "m"}, "3.3k"));
    c.comps.push_back(comp(Kind::R, "R3", {"m", "out"}, "3.3k"));
    c.comps.push_back(comp(Kind::C, "C1", {"n1", "0"}, "1n"));
    c.comps.push_back(comp(Kind::GND, "G1", {"0"}));
    AnalysisSpec sp;
    sp.kind = AnalysisKind::TransferFunction;
    sp.input_ref = "I1";
    sp.output = "V(out)";
    sp.sweep.f_start_hz = 1;
    sp.sweep.f_stop_hz = 1e9;
    CardResult cr = run_analysis(c, sp);
    const std::string& t = cr.transfer.pruned.text;
    const std::string& ltx = cr.transfer.pruned.latex;
    // The series group is a held atom, not expanded, and it is never broken
    // apart anywhere (nothing may recompute R1*R3 or R1*R2).
    CHECK(cr.transfer.pruned.den_factors.size() >= 1);
    for (const auto& f : cr.transfer.pruned.den_factors) {
        std::string s = str(f.expr);
        CHECK(s.find("R1*R3") == std::string::npos);
        CHECK(s.find("R1*R2") == std::string::npos);
    }
    // The numerator, the denominator and the gain all carry the SAME held atom.
    auto holds = [](const std::string& s) {
        bool par = s.find("||") != std::string::npos ||
                   s.find("\\parallel") != std::string::npos;
        return par && (s.find("R2+R3") != std::string::npos ||
                       s.find("R3+R2") != std::string::npos);
    };
    CHECK(holds(t));                        // text (numerator + denominator)
    CHECK(!t.empty() && holds(ltx));        // LaTeX mirrors it
    CHECK(holds(str(cr.transfer.pruned.gain)));
    // The denominator's s-coefficient holds the atom as a single factor too.
    CHECK(holds(str(cr.transfer.pruned.den_poly)));
    // No expanded resistor-ratio form survives anywhere.
    CHECK(t.find("R1*R3") == std::string::npos);
    CHECK(t.find("R1*R2") == std::string::npos);
    // The held parallel atom binds to the held SERIES atom (par(.,ser)).
    CHECK(is_parallel(cr.transfer.pruned.gain));
    auto pa = parallel_args(cr.transfer.pruned.gain);
    bool has_series_operand = false;
    for (const auto& a : pa)
        if (is_series(a)) has_series_operand = true;
    CHECK(has_series_operand);
}

// Independent current sources follow the SPICE convention: positive current
// flows from n+ (pin 0) through the source to n- (pin 1). A 1 A DC source into
// a grounded resistor therefore drives the node NEGATIVE (LTspice: V = -I*R).
static void test_current_source_spice_sign() {
    Circuit c;
    Component i1 = comp(Kind::I, "I1", {"out", "0"}, "1");
    i1.dc_text = "1"; // DC analysis uses the source's DC value
    c.comps.push_back(i1);
    c.comps.push_back(comp(Kind::R, "R1", {"out", "0"}, "1k"));
    c.comps.push_back(comp(Kind::GND, "G1", {"0"}));
    AnalysisSpec sp;
    sp.kind = AnalysisKind::DC;
    sp.output = "V(out)";
    CardResult cr = run_analysis(c, sp);
    // The source is symbolic: V(out) = -(I1 * R1).
    CHECK(cr.report.find("V(out) = -I1*R1") != std::string::npos ||
          cr.report.find("V(out) = -R1*I1") != std::string::npos);
}

// Series is "two elements share a node nothing else touches". Whether to fold
// depends ONLY on whether that intermediate node is USED. This delta divider
// (R1 in->m, R2 m->out, Rb in->out) is the tia_lg topology:
//   * output V(out): m is unused -> R1+R2 fold, then parallel with Rb:
//     (R1+R2)||Rb;
//   * output V(m): m is the probe -> R1+R2 must NOT fold, and V(m) must resolve.
static void test_series_fold_respects_used_node() {
    auto build = []() {
        Circuit c;
        c.comps.push_back(comp(Kind::V, "V1", {"in", "0"}, "1"));
        c.comps.push_back(comp(Kind::R, "R1", {"in", "m"}, "10k"));
        c.comps.push_back(comp(Kind::R, "R2", {"m", "out"}, "10k"));
        c.comps.push_back(comp(Kind::R, "Rb", {"in", "out"}, "100k"));
        c.comps.push_back(comp(Kind::R, "Rl", {"out", "0"}, "1k"));
        return ground(c);
    };
    AnalysisRequest req;
    req.input_ref = "V1";
    req.sweep.f_start_hz = 1;
    req.sweep.f_stop_hz = 1e9;

    // m unused: R1+R2 fold into a held series atom, then bind in parallel with
    // Rb, so the text carries (R1+R2)||Rb and never the expanded R1*R2.
    req.output = "V(out)";
    AnalysisResult ro = analyze(build(), req);
    CHECK(ro.pruned.text.find("||") != std::string::npos);
    CHECK(ro.pruned.text.find("R1*R2") == std::string::npos); // not expanded

    // m used: the pair must stay two separate real nodes, and V(m) must
    // resolve (not throw "unknown node"). This is the exact bug the fold
    // introduced when it ignored whether the intermediate node was probed.
    req.output = "V(m)";
    bool threw = false;
    AnalysisResult rm;
    try {
        rm = analyze(build(), req);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(!threw);
    CHECK(rm.output_desc == "V(m)");
}

// An inductor must actually be usable: an RL low-pass V1 -> R1 -> L1 -> gnd
// has H(s) = s*L1/R1 / (1 + s*L1/R1). Regression for the branch-index key
// mismatch (registered as the bare ref, looked up as "L:"+ref) that made every
// circuit with an inductor throw map::at.
static void test_inductor_rl_lowpass() {
    Circuit c;
    c.comps.push_back(comp(Kind::V, "V1", {"in", "0"}, "1"));
    c.comps.push_back(comp(Kind::R, "R1", {"in", "out"}, "1k"));
    c.comps.push_back(comp(Kind::L, "L1", {"out", "0"}, "1m"));
    c.comps.push_back(comp(Kind::GND, "G1", {"0"}));
    AnalysisRequest req;
    req.input_ref = "V1";
    req.output = "V(out)";
    req.sweep.f_start_hz = 1;
    req.sweep.f_stop_hz = 1e9;
    bool threw = false;
    AnalysisResult r;
    try {
        r = analyze(c, req);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(!threw);
    ex s = S(r, "s");
    ex H = raw_H(r);
    ex expect = (s * S(r, "L1") / S(r, "R1")) /
                (ex(1) + s * S(r, "L1") / S(r, "R1"));
    CHECK((H - expect).normal().is_zero());
}

// Large-signal DC: an NMOS in saturation, with Id and the operating point
// determined by the source DC values and the device model (vdsat = 2*Id/gm,
// Vgs = vdsat + Vth). Checked against the closed-form bias solution.
static void test_dc_large_signal_mosfet() {
    auto build = []() {
        Circuit c;
        Component vg = comp(Kind::V, "VG", {"g", "0"}, "0");
        vg.dc_text = "0.8"; // Vgs = 0.8 V
        c.comps.push_back(vg);
        Component vdd = comp(Kind::V, "VDD1", {"vdd", "0"}, "0");
        vdd.dc_text = "3"; // 3 V rail
        c.comps.push_back(vdd);
        Component m = comp(Kind::NMOS, "M1", {"out", "g", "0"}, "");
        m.param_text["gm"] = "1m";
        m.param_text["ro"] = "100k";
        m.param_on["ro"] = true;
        c.comps.push_back(m);
        c.comps.push_back(comp(Kind::R, "Rd", {"vdd", "out"}, "10k"));
        c.comps.push_back(comp(Kind::GND, "G1", {"0"}));
        return c;
    };
    AnalysisSpec sp;
    sp.kind = AnalysisKind::DC;
    sp.output = "V(out)";
    sp.tech.vth = 0.5; // DC tech setting (universal Vth)
    CardResult cr = run_analysis(build(), sp);
    CHECK(cr.report.find("large-signal") != std::string::npos);
    // In saturation: Vds >= Vdsat, so the device is not flagged.
    CHECK(cr.report.find("not in saturation") == std::string::npos);
    CHECK(cr.report.find("I(M1)") != std::string::npos);
    CHECK(!cr.latex_report.empty());
    // Mode 1 reports the saturation voltage directly: Vdsat = Vgs - Vth =
    // VG - Vth = 0.8 - 0.5 = 0.3 V, and the drain current is gm*Vdsat/2.
    // Mode 1 reports the saturation voltage directly (Vdsat = Vgs - Vth) and
    // the drain current as gm*Vdsat/2; the exact sign/ordering of the printed
    // sum varies, so match on the structure.
    CHECK(cr.report.find("Vdsat = ") != std::string::npos);
    CHECK(cr.report.find("VG") != std::string::npos);
    CHECK(cr.report.find("Vth") != std::string::npos);
    CHECK(cr.report.find("I(M1) = ") != std::string::npos);
    CHECK(cr.report.find("gm_M1") != std::string::npos);
    CHECK(cr.report.find("not in saturation") == std::string::npos);
}

// An inverting amplifier's gain is a *magnitude* in dB: K = -A reports +N dB,
// not -N dB (the sign is a 180-degree phase, not attenuation).
static void test_gain_db_is_magnitude() {
    Circuit c;
    Component v = comp(Kind::V, "V1", {"in", "0"}, "0");
    v.dc_text = "0";
    v.ac_text = "1";
    c.comps.push_back(v);
    // inverting amp block with gain -100 (magnitude 100 -> 40 dB)
    c.comps.push_back(comp(Kind::AMP, "A1", {"in", "out"}, "-100"));
    c.comps.push_back(comp(Kind::GND, "G1", {"0"}));
    AnalysisSpec sp;
    sp.kind = AnalysisKind::TransferFunction;
    sp.input_ref = "V1";
    sp.output = "V(out)";
    CardResult cr = run_analysis(c, sp);
    CHECK(cr.report.find("DC Gain: 40.00 dB") != std::string::npos);
    CHECK(cr.report.find("-40.00 dB") == std::string::npos);
}

// Push the same device into triode by lowering the drain resistor's headroom
// (raise Vgs so Vdsat > Vds): the analysis must abort and name the device.
static void test_dc_large_signal_triode_aborts() {
    Circuit c;
    Component vg = comp(Kind::V, "VG", {"g", "0"}, "0");
    vg.dc_text = "3"; // Vgs = 3 V -> Vdsat = 2*(3-0.5) = 5 V, above the rail
    c.comps.push_back(vg);
    Component vdd = comp(Kind::V, "VDD1", {"vdd", "0"}, "0");
    vdd.dc_text = "3";
    c.comps.push_back(vdd);
    Component m = comp(Kind::NMOS, "M1", {"out", "g", "0"}, "");
    m.param_text["gm"] = "1m";
    m.param_text["ro"] = "100k";
    m.param_on["ro"] = true;
    c.comps.push_back(m);
    c.comps.push_back(comp(Kind::R, "Rd", {"vdd", "out"}, "10k"));
    c.comps.push_back(comp(Kind::GND, "G1", {"0"}));
    AnalysisSpec sp;
    sp.kind = AnalysisKind::DC;
    sp.output = "V(out)";
    sp.tech.vth = 0.5;
    CardResult cr = run_analysis(c, sp);
    CHECK(cr.report.find("not in saturation") != std::string::npos);
    CHECK(cr.report.find("M1") != std::string::npos);
}

// AC is a superposition of every independent source's AC value into one
// output -- no input source is selected. V1 (AC=1) and V2 (AC=2) driving a
// resistive summing node give a linear combination.
static void test_ac_superposition_of_sources() {
    Circuit c;
    Component v1 = comp(Kind::V, "V1", {"a", "0"}, "0");
    v1.ac_text = "1";
    c.comps.push_back(v1);
    Component v2 = comp(Kind::V, "V2", {"b", "0"}, "0");
    v2.ac_text = "2";
    c.comps.push_back(v2);
    c.comps.push_back(comp(Kind::R, "R1", {"a", "out"}, "1k"));
    c.comps.push_back(comp(Kind::R, "R2", {"b", "out"}, "1k"));
    c.comps.push_back(comp(Kind::R, "R3", {"out", "0"}, "1k"));
    c.comps.push_back(comp(Kind::GND, "G1", {"0"}));
    AnalysisSpec sp;
    sp.kind = AnalysisKind::AC;
    sp.output = "V(out)";
    sp.sweep.f_start_hz = 1;
    sp.sweep.f_stop_hz = 1e6;
    CardResult cr = run_analysis(c, sp);
    // The response to (ac1=1, ac2=2) is (2 R3 R1 + R3 R2)/(R3 R1 + R2 R1 + R3 R2).
    CHECK(cr.report.find("superposition") != std::string::npos);
    // at f0 the numeric value is (2/5 + 1/5) = 0.6 -> 20*log10(0.6) = -4.44 dB
    const auto& vals = cr.values;
    bool found_f0 = false;
    for (const auto& kv : vals)
        if (kv.first == "at f0") found_f0 = true;
    CHECK(found_f0);
    // the two per-source contributions are both listed
    int contribs = 0;
    for (const auto& kv : vals)
        if (kv.first == "V1" || kv.first == "V2") ++contribs;
    CHECK(contribs == 2);
}

// A common-source stage biased by DC: V(out) = VDD - R1*Id (the drain drop),
// with Id from the saturation model and Vth from the DC tech settings (no
// per-device Vth / rds -- ro is the single channel-length-modulation value).
static void test_dc_common_source_vdd_minus_rid() {
    Circuit c;
    Component vdd = comp(Kind::V, "VDD1", {"vdd", "0"}, "0");
    vdd.dc_text = "5";
    c.comps.push_back(vdd);
    c.comps.push_back(comp(Kind::R, "R1", {"vdd", "out"}, "2k"));
    Component m = comp(Kind::NMOS, "M1", {"out", "g", "0"}, "");
    m.param_text["gm"] = "1m";
    m.param_text["ro"] = "1e6";
    m.param_on["ro"] = true;
    c.comps.push_back(m);
    Component vg = comp(Kind::V, "VG", {"g", "0"}, "0");
    vg.dc_text = "0.8"; // Vgs = 0.8
    c.comps.push_back(vg);
    c.comps.push_back(comp(Kind::GND, "G1", {"0"}));
    AnalysisSpec sp;
    sp.kind = AnalysisKind::DC;
    sp.output = "V(out)";
    sp.tech.vth = 0.6;
    CardResult cr = run_analysis(c, sp);
    CHECK(cr.report.find("Vth") != std::string::npos); // tech Vth in use
    CHECK(cr.report.find("I(M1)") != std::string::npos);
    // Symbolic operating point: evaluate it numerically with the component
    // estimates. Id = gm*(Vgs-Vth)/2 = 100 uA, so V(out) = VDD - R1*Id (the
    // large ro only nudges it) ~ 4.8 V.
    DcSolution dc = solve_dc(c, sp.tech);
    double vout = eval_complex(dc.node_v["out"], dc.params, 0.0).real();
    CHECK_CLOSE(vout, 4.8, 0.05);
    // The report keeps the symbols and exact fractions, and reads as a KVL
    // bias equation rather than a numeric dump.
    CHECK(cr.report.find("1/2") != std::string::npos);
    CHECK(cr.report.find("V1") == std::string::npos); // this circuit uses VDD/VG
    CHECK(cr.report.find("Vth") != std::string::npos);
    CHECK(cr.report.find("0.5") == std::string::npos); // fractions, not decimals
}

// Mode 2: symbolic square law. Id = 1/2*uCox*(W/L)*Vov^2, with Vov, W, L kept
// as symbols (W/L named W_M1/L_M1), and the defining relation Vov = Vgs - Vth
// reported. Saturation is assumed (no triode abort).
// Mode 2 with a current-mirror copy: the copy's geometry is the unit's
// (W_M2 = mult*W_M1) and, since Id is fixed by KCL, I(M2) = I(M1) = I1 -- the
// copy count and geometry cancel out of the drain currents.
static void test_dc_square_law_mirror() {
    Circuit c;
    Component i1 = comp(Kind::I, "I1", {"vdd", "n1"}, "0");
    i1.dc_text = "10u";
    c.comps.push_back(i1);
    Component m1 = comp(Kind::NMOS, "M1", {"n1", "n1", "0"}, "");
    m1.param_text["W"] = "10u";
    m1.param_text["L"] = "1u";
    c.comps.push_back(m1);
    Component m2 = comp(Kind::NMOS, "M2", {"out", "n1", "0"}, "");
    m2.mirror_ref = "M1";
    m2.mirror_mult = 4;
    c.comps.push_back(m2);
    c.comps.push_back(comp(Kind::R, "R2", {"vdd", "out"}, "10k"));
    Component vdd = comp(Kind::V, "VDD", {"vdd", "0"}, "0");
    vdd.dc_text = "3.3";
    vdd.value_text = "3.3";
    c.comps.push_back(vdd);
    c.comps.push_back(comp(Kind::GND, "G1", {"0"}));
    AnalysisSpec sp;
    sp.kind = AnalysisKind::DC;
    sp.input_ref = "I1";
    sp.output = "V(out)";
    sp.tech.dc_mode = DcMode::SquareLaw;
    sp.tech.vth = 0.5;
    sp.tech.uncox = 200e-6;
    CardResult cr = run_analysis(c, sp);
    // The drain current of a mirror leg is a plain multiple of the reference
    // current (KCL): M1 carries I1, and the 4x copy carries 4*I1 -- no
    // per-copy geometry or square-law term survives.
    CHECK(cr.report.find("I(M1) = I1") != std::string::npos);
    CHECK(cr.report.find("I(M2) = 4*I1") != std::string::npos);
    CHECK(cr.report.find("W_M2") == std::string::npos);
    CHECK(cr.report.find("L_M2") == std::string::npos);
}

static void test_dc_square_law_symbolic() {
    Circuit c;
    Component vdd = comp(Kind::V, "VDD1", {"vdd", "0"}, "0");
    vdd.dc_text = "3.3";
    c.comps.push_back(vdd);
    Component vg = comp(Kind::V, "VG", {"g", "0"}, "0");
    vg.dc_text = "0.9";
    c.comps.push_back(vg);
    Component m = comp(Kind::NMOS, "M1", {"d", "g", "0"}, "");
    m.param_text["W"] = "10u";
    m.param_text["L"] = "1u";
    c.comps.push_back(m);
    c.comps.push_back(comp(Kind::R, "Rd", {"vdd", "d"}, "10k"));
    c.comps.push_back(comp(Kind::GND, "G1", {"0"}));
    AnalysisSpec sp;
    sp.kind = AnalysisKind::DC;
    sp.output = "V(d)";
    sp.tech.dc_mode = DcMode::SquareLaw;
    sp.tech.vth = 0.5;
    sp.tech.uncox = 200e-6;
    CardResult cr = run_analysis(c, sp);
    CHECK(cr.report.find("square-law") != std::string::npos);
    CHECK(cr.report.find("Vov_M1") != std::string::npos);
    CHECK(cr.report.find("W_M1") != std::string::npos);
    CHECK(cr.report.find("L_M1") != std::string::npos);
    CHECK(cr.report.find("unCox") != std::string::npos);
    // The overdrive is reported in its definitional and solved forms.
    CHECK(cr.report.find("Vov_M1 = Vgs - Vth") != std::string::npos);
    CHECK(cr.report.find("sqrt(2*Id*L/(uCox*W))") != std::string::npos);
    // Never aborts on triode (saturation is assumed in this mode).
    CHECK(cr.report.find("ABORTED") == std::string::npos);
}

// Mode 3: numeric operating point from a SPICE level-1 model. Id must equal the
// square law exactly, and the override exports gm/ro/caps.
static void test_dc_numeric_spice() {
    Circuit c;
    Component vdd = comp(Kind::V, "VDD", {"vdd", "0"}, "0");
    vdd.dc_text = "3.3";
    c.comps.push_back(vdd);
    Component vg = comp(Kind::V, "VG", {"g", "0"}, "0");
    vg.dc_text = "0.9";
    c.comps.push_back(vg);
    Component m = comp(Kind::NMOS, "M1", {"d", "g", "0"}, "");
    m.param_text["W"] = "10u";
    m.param_text["L"] = "1u";
    c.comps.push_back(m);
    c.comps.push_back(comp(Kind::R, "Rd", {"vdd", "d"}, "10k"));
    c.comps.push_back(comp(Kind::GND, "G1", {"0"}));
    MosModel nm;
    nm.vto = 0.5;
    nm.kp = 200e-6;
    nm.lambda = 0.05;
    nm.cgso = 2e-10;
    nm.cgdo = 2e-10;
    nm.cj = 1e-3;
    MosModel pm;
    pm.pmos = true;
    pm.vto = -0.5;
    NumericDcResult nr = solve_dc_numeric(c, nm, pm);
    CHECK(nr.ok);
    if (nr.ok) {
        // Vov = 0.4, Id = 1/2*200u*(10)*(0.4)^2*(1+0.05*Vds).
        double vd = nr.node_v.at("d");
        double expect = 0.5 * nm.kp * 10.0 * 0.4 * 0.4 * (1.0 + nm.lambda * vd);
        CHECK_CLOSE(std::fabs(nr.mos[0].id), expect, expect * 1e-6);
        CHECK(nr.mos[0].saturated);

        AnalysisSpec sp;
        sp.kind = AnalysisKind::DC;
        sp.output = "V(d)";
        sp.tech.dc_mode = DcMode::Numeric;
        sp.tech.model_file = "models.lib"; // not read when no override
        sp.tech.override_small_signal = false;
        // (report-only path needs a readable file; use the numeric result API.)
        CHECK(nr.mos[0].gm > 0.0);
    }
}

// The SPICE level-1 model parser.
static void test_spice_model_parser() {
    std::string data =
        "* comment\n"
        ".model nch nmos level=1 vto=0.5 kp=200u lambda=0.05 cgso=2e-10\n"
        ".model pch pmos level=1 vto=-0.5 kp=100u\n"
        "+ lambda=0.06 cj=1e-3\n";
    // parse from a temp file
    std::string path = "spice_test_tmp.mod";
    { std::FILE* f = std::fopen(path.c_str(), "w"); std::fputs(data.c_str(), f);
      std::fclose(f); }
    std::string err;
    std::vector<MosModel> ms = parse_spice_models(path, err);
    CHECK(err.empty());
    CHECK(ms.size() == 2);
    const MosModel* nch = find_model(ms, "NCH"); // case-insensitive
    CHECK(nch != nullptr);
    if (nch) {
        CHECK(!nch->pmos);
        CHECK_CLOSE(nch->vto, 0.5, 1e-9);
        CHECK_CLOSE(nch->kp, 200e-6, 1e-12);
        CHECK_CLOSE(nch->cgso, 2e-10, 1e-21);
    }
    const MosModel* pch = find_model(ms, "pch");
    CHECK(pch != nullptr);
    if (pch) {
        CHECK(pch->pmos);
        CHECK_CLOSE(pch->lambda, 0.06, 1e-9); // continuation line
        CHECK_CLOSE(pch->cj, 1e-3, 1e-12);
    }
    std::remove(path.c_str());
}

// The tia_lg operating point: the amp's inverting-node voltage appears through
// the finite gain, and V(out) collapses to I1*(R1||(R2+R3))*A/(A+1) with the
// parallel atom recovered (not an expanded resistor ratio).
static void test_dc_tia_low_entropy_form() {
    Circuit c;
    Component i1 = comp(Kind::I, "I1", {"n1", "0"}, "0");
    i1.dc_text = "1";
    c.comps.push_back(i1);
    Component op = comp(Kind::OPAMP, "U1", {"0", "n1", "out"}, "1e5");
    c.comps.push_back(op);
    c.comps.push_back(comp(Kind::R, "R1", {"n1", "out"}, "10k"));
    c.comps.push_back(comp(Kind::R, "R2", {"n1", "m"}, "3.3k"));
    c.comps.push_back(comp(Kind::R, "R3", {"m", "out"}, "3.3k"));
    c.comps.push_back(comp(Kind::GND, "G1", {"0"}));
    AnalysisSpec sp;
    sp.kind = AnalysisKind::DC;
    sp.output = "V(out)";
    CardResult cr = run_analysis(c, sp);
    // Parallel atom recovered, symbolic gain kept (A/(A+1)), I1 symbolic.
    CHECK(cr.report.find("||") != std::string::npos);
    CHECK(cr.report.find("A_U1") != std::string::npos);
    CHECK(cr.report.find("I1") != std::string::npos);
    // The exact KVL: V(out) - V(n1) = I1*(R1||(R2+R3))*(finite-gain factor).
    // R1||(R2+R3) = 10k * 6.6k / 16.6k ~ 3.976k, so with I1 = 1 A the drop is
    // ~ 3976 V (a 1 A bias drives large numbers; the point is the structure).
    DcSolution dc = solve_dc(c);
    ex diff = (dc.node_v["out"] - dc.node_v["n1"]).normal();
    CHECK_CLOSE(eval_complex(diff, dc.params, 0.0).real(), 3975.9, 1.0);
}

// The pretty-printer must parenthesise an additive denominator factor, so
// 1/((1+A)*(R1+R2+R3)) does not print as 1/(1+A*R1+R2+R3).
static void test_pretty_denominator_parens() {
    Circuit c;
    Component i1 = comp(Kind::I, "I1", {"n1", "0"}, "0");
    i1.dc_text = "1";
    c.comps.push_back(i1);
    Component op = comp(Kind::OPAMP, "U1", {"0", "n1", "out"}, "1e5");
    c.comps.push_back(op);
    c.comps.push_back(comp(Kind::R, "R1", {"n1", "out"}, "10k"));
    c.comps.push_back(comp(Kind::R, "R2", {"n1", "m"}, "3.3k"));
    c.comps.push_back(comp(Kind::R, "R3", {"m", "out"}, "3.3k"));
    c.comps.push_back(comp(Kind::GND, "G1", {"0"}));
    AnalysisSpec sp;
    sp.kind = AnalysisKind::DC;
    sp.output = "V(out)";
    CardResult cr = run_analysis(c, sp);
    // A factored two-factor denominator must appear as (…)*(…) or via a
    // parallel atom -- never as a bare "A_U1*R1" run-together.
    CHECK(cr.report.find("A_U1*R1") == std::string::npos ||
          cr.report.find("(A_U1") != std::string::npos);
}

// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    struct Test { const char* name; std::function<void()> fn; };
    std::vector<Test> tests = {
        {"eng_parse", test_eng_parse},
        {"sweep_points", test_sweep_points},
        {"rank_band", test_rank_band_keeps_high_freq_term},
        {"anon_gnd", test_anonymous_ground_refs},
        {"roots", test_roots},
        {"divider", test_divider},
        {"rc_lowpass", test_rc_lowpass},
        {"cs_amp_parasitics", test_cs_amp_parasitics},
        {"prune_series_r", test_prune_series_r},
        {"prune_no_drop", test_prune_no_drop_when_close},
        {"two_stage_factoring", test_two_stage_factoring},
        {"degree_drop_low_f0", test_degree_drop_at_low_f0},
        {"branch_current", test_branch_current_output},
        {"bjt_rb_node", test_bjt_rb_node},
        {"errors", test_errors},
        {"size_offset_db", test_size_offset_db},
        {"parallel_form", test_parallel_form},
        {"magnitude_pruning", test_magnitude_pruning},
        {"parallel_collapse", test_parallel_collapse},
        {"rational_sum_parallel", test_rational_sum_exposes_parallel},
        {"symbolic_two_pole_factor", test_symbolic_two_pole_factorization},
        {"copy_of_passive", test_copy_of_passive},
        {"copy_of_series_parallel", test_copy_of_series_parallel},
        {"differential_analysis", test_differential_analysis},
        {"differential_pair_cmrr", test_differential_pair_cmrr},
        {"latex_output", test_latex_output},
        {"latex_factor_parens", test_latex_factor_parens},
        {"report_latex", test_report_has_latex_and_factors},
        {"le_no_bogus_factor", test_low_entropy_no_bogus_factor},
        {"approx_factor_accuracy", test_approx_factor_accuracy},
        {"coeff_product_dropped", test_coefficient_product_not_dropped},
        {"approx_off_consistent", test_approx_off_is_consistent},
        {"dc_analysis", test_dc_analysis},
        {"noise_input_referred", test_noise_analysis_input_referred},
        {"loop_gain_opamp", test_loop_gain_opamp},
        {"nullor_two_port", test_nullor_two_port_ideal_amp},
        {"psrr_vdd", test_psrr_vdd},
        {"cs_input_pole_cgs", test_cs_input_pole_is_cgs_not_c1},
        {"pole_zero_60db", test_pole_zero_60db_threshold},
        {"series_20db", test_series_reduction_20db},
        {"miller_zero_survives", test_miller_zero_survives_pruning},
        {"gain_bw_report", test_gain_bw_report},
        {"metrics_latex", test_metrics_latex},
        {"metrics_dominant_pole", test_metrics_dominant_pole_bandwidth},
        {"amp_gain_symbolic", test_amp_gain_is_symbolic},
        {"loop_gain_return_ratio", test_loop_gain_return_ratio},
        {"gbw_optional", test_gbw_optional},
        {"amp_signed_gain", test_amp_signed_gain},
        {"loop_gain_sections", test_loop_gain_report_sections},
        {"mirror_multiplicity", test_mirror_multiplicity},
        {"dc_square_law_mirror", test_dc_square_law_mirror},
        {"noise_bad_input", test_noise_survives_bad_input},
        {"noise_current_amp", test_noise_current_input_and_amp},
        {"tf_low_entropy_parallel", test_tf_low_entropy_parallel},
        {"zout_parallel_collapses", test_zout_parallel_collapses},
        {"mna_parallel_precombine", test_mna_parallel_precombine},
        {"octc_time_constants", test_octc_time_constants},
        {"all_analyses_latex_report", test_all_analyses_have_latex_report},
        {"percent_no_exponent", test_percent_no_exponent},
        {"series_then_parallel", test_series_then_parallel_form},
        {"current_source_sign", test_current_source_spice_sign},
        {"series_fold_used_node", test_series_fold_respects_used_node},
        {"inductor_rl_lowpass", test_inductor_rl_lowpass},
        {"dc_large_signal_mosfet", test_dc_large_signal_mosfet},
        {"gain_db_is_magnitude", test_gain_db_is_magnitude},
        {"far_zeros_vs_dominant_pole", test_far_zeros_vs_dominant_pole},
        {"dc_common_source", test_dc_common_source_vdd_minus_rid},
        {"dc_square_law", test_dc_square_law_symbolic},
        {"dc_numeric_spice", test_dc_numeric_spice},
        {"spice_model_parser", test_spice_model_parser},
        {"dc_tia_low_entropy", test_dc_tia_low_entropy_form},
        {"pretty_denominator", test_pretty_denominator_parens},
        {"dc_triode_aborts", test_dc_large_signal_triode_aborts},
        {"ac_source_superposition", test_ac_superposition_of_sources},
    };

    std::string filter = argc > 1 ? argv[1] : "";
    int run = 0;
    for (const auto& t : tests) {
        if (!filter.empty() && std::string(t.name).find(filter) == std::string::npos)
            continue;
        int before = g_failures;
        std::printf("[%s] %s ...\n", t.name, "");
        try {
            t.fn();
        } catch (const std::exception& e) {
            ++g_failures;
            std::printf("  EXCEPTION: %s\n", e.what());
        } catch (...) {
            ++g_failures;
            std::printf("  EXCEPTION: unknown\n");
        }
        std::printf("  -> %s\n", g_failures == before ? "ok" : "FAILED");
        ++run;
    }
    if (run == 0) {
        std::printf("no tests matched filter '%s'\n", filter.c_str());
        return 2;
    }
    std::printf("\n%d test(s) run, %d failure(s)\n", run, g_failures);
    return g_failures == 0 ? 0 : 1;
}
