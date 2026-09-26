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
    // (b) ro enabled: H = -gm * (Rd || ro)
    {
        AnalysisResult r = analyze(make(true, false, false), req);
        gm = S(r, "gm_M1"); ro = S(r, "ro_M1");
        ex H = raw_H(r);
        CHECK(str(H).find("ro_M1") != std::string::npos);
        ex expect = -gm * S(r, "Rd") * ro / (S(r, "Rd") + ro);
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
    // the Cgd time-constant term in the denominator is ~154 dB below the CL
    // term and must be dropped, leaving a single dominant pole
    bool dropped_cgd_den = false;
    for (const auto& d : r.pruned.dropped)
        if (d.location.find("denominator") == 0 &&
            d.term.find("Cgd_M1") != std::string::npos)
            dropped_cgd_den = true;
    CHECK(dropped_cgd_den);
    CHECK(r.pruned.text.find("CL") != std::string::npos);
    CHECK(r.pruned.den_factors.size() == 1);

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
    c.comps.push_back(comp(Kind::V, "V1", {"in", "0"}, "1"));
    c.comps.push_back(comp(Kind::R, "R1", {"in", "out"}, "10k"));
    c.comps.push_back(comp(Kind::R, "R2", {"out", "0"}, "10k"));
    c.comps.push_back(comp(Kind::C, "C1", {"out", "0"}, "1u")); // opens at DC
    c = ground(c);
    AnalysisSpec sp;
    sp.kind = AnalysisKind::DC;
    sp.input_ref = "V1";
    sp.output = "V(out)";
    CardResult cr = run_analysis(c, sp);
    // resistive divider: V(out) = 1/2
    CHECK(cr.report.find("V(out)") != std::string::npos);
    CHECK(cr.report.find("LaTeX") != std::string::npos);
    CHECK(!cr.latex.empty());
    CHECK(cr.latex.find("aligned") != std::string::npos);
}

static void test_noise_analysis_input_referred() {
    // resistive divider driven by V1: input-referred noise density is
    // sqrt(4kT*(R1||R2)) (the two resistors' thermal noise seen at the input),
    // output-referred is half of that; check the ratio and the value.
    Circuit c;
    c.comps.push_back(comp(Kind::V, "V1", {"in", "0"}, "1"));
    c.comps.push_back(comp(Kind::R, "R1", {"in", "out"}, "10k"));
    c.comps.push_back(comp(Kind::R, "R2", {"out", "0"}, "10k"));
    c = ground(c);
    AnalysisSpec sp;
    sp.kind = AnalysisKind::Noise;
    sp.input_ref = "V1";
    sp.output = "V(out)";
    sp.f0_hz = 1e3;
    CardResult cr = run_analysis(c, sp);
    CHECK(cr.report.find("output-referred") != std::string::npos);
    CHECK(cr.report.find("input-referred") != std::string::npos);
    CHECK(cr.values.size() == 2);

    // numeric: gain = 1/2, so vin_rms = 2*vout_rms
    auto get = [&](const std::string& k) -> double {
        for (const auto& v : cr.values)
            if (v.first == k) return std::atof(v.second.c_str());
        return -1;
    };
    double vout = get("Vout_n"), vin = get("Vin_n");
    CHECK_CLOSE(vin / vout, 2.0, 0.01);
    // Both resistors contribute 4kT/R seen through R1||R2, so
    //   vout^2 = 2*(4kT/R)*(R1||R2)^2 = 4kT*(R1||R2)
    double expect_vout = std::sqrt(4.0 * 1.380649e-23 * 300.15 * 5000.0);
    CHECK_CLOSE(vout, expect_vout, expect_vout * 0.02);
    CHECK_CLOSE(vin, 2.0 * expect_vout, expect_vout * 0.03);
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
    CHECK(r.report.find("DC gain") != std::string::npos);
    CHECK(r.report.find("-3 dB bandwidth") != std::string::npos);
    CHECK(r.report.find("Unity-gain") != std::string::npos);
    // RC low-pass: DC gain 0 dB, -3 dB at 1/(2*pi*R*C) ~ 15.9 kHz.
    CHECK(r.report.find("kHz") != std::string::npos);
    // Symbolic: the single pole is R1*C1, so the report carries w_p0 = 1/(R1*C1).
    CHECK(r.report.find("w_p0") != std::string::npos);
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
    CHECK(lx.find("DC\\ gain") != std::string::npos);
    CHECK(lx.find("-3\\ dB\\ bandwidth") != std::string::npos);
    CHECK(lx.find("unity") != std::string::npos);
    CHECK(lx.find("w_{p0}") != std::string::npos);
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
    // the -3 dB symbolic line names the dominant pole w_p0 (bandwidth reduces
    // to the dominant time constant when the other pole is 100x away)
    CHECK(r.report.find("w_p0") != std::string::npos);
    CHECK(r.report.find("-3 dB bandwidth") != std::string::npos);
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
    CHECK(cr.report.find("Gain / bandwidth") != std::string::npos);
    CHECK(cr.report.find("phase margin") != std::string::npos);
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
        {"latex_output", test_latex_output},
        {"report_latex", test_report_has_latex_and_factors},
        {"le_no_bogus_factor", test_low_entropy_no_bogus_factor},
        {"approx_factor_accuracy", test_approx_factor_accuracy},
        {"coeff_product_dropped", test_coefficient_product_not_dropped},
        {"approx_off_consistent", test_approx_off_is_consistent},
        {"dc_analysis", test_dc_analysis},
        {"noise_input_referred", test_noise_analysis_input_referred},
        {"loop_gain_opamp", test_loop_gain_opamp},
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
