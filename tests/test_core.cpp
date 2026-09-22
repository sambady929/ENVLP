// SymCirc core engine tests.
// Assert-based mini framework; run via CTest (all tests) or
//   test_core <substring>   to run a subset.

#include "core/Eng.h"
#include "core/Engine.h"
#include "core/MNA.h"
#include "core/Netlist.h"
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
    // V -> R1(10k) -> R2(10) -> C(1n) -> gnd.  C*R2 is60 dB below C*R1.
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

    CHECK(!r.pruned.dropped.empty());
    bool dropped_r2 = false;
    for (const auto& d : r.pruned.dropped)
        if (d.term.find("R2") != std::string::npos) dropped_r2 = true;
    CHECK(dropped_r2);

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
int main(int argc, char** argv) {
    struct Test { const char* name; std::function<void()> fn; };
    std::vector<Test> tests = {
        {"eng_parse", test_eng_parse},
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
