// Reproduce the source-follower circuit the user described: gate = Vin, source
// = Vout (the output node), drain = VDD. The only path from Vout to ground is
// through ro itself (the M1 drain-source path), so the small-signal gain at
// s=0 is gm*ro / (1 + gm*ro). Verify the engine produces that.

#include "core/Analysis.h"
#include "core/Netlist.h"

#include <cstdio>
#include <string>

using namespace syms;

static Component mkv(Kind k, const std::string& ref,
                       std::initializer_list<std::string> nodes,
                       const std::string& v = "") {
    Component c;
    c.kind = k;
    c.ref = ref;
    c.nodes.assign(nodes);
    c.value_text = v;
    return c;
}

static Circuit cd_test() {
    Circuit c;
    // input source
    c.comps.push_back(mkv(Kind::V, "V1", {"in", "0"}, "1"));

    // MOSFET: drain=VDD, gate=in, source=out (the output node)
    Component m = mkv(Kind::NMOS, "M1", {"VDD", "in", "out"});
    m.param_text["gm"] = "1m";
    m.param_text["ro"] = "100k";
    m.param_on["ro"] = true;
    m.param_on["Cgs"] = false;
    m.param_on["Cgd"] = false;
    c.comps.push_back(m);

    // DC source for the drain
    c.comps.push_back(mkv(Kind::V, "VDD", {"VDD", "0"}, "5"));

    return c;
}

int main() {
    syms::AnalysisSpec s;
    s.kind = AnalysisKind::TransferFunction;
    s.input_ref = "V1";
    s.output = "V(out)";
    s.sweep = syms::SweepSpec{};
    s.prune = false;
    s.gm_ro_assume = false;

    syms::CardResult cr = syms::run_analysis(cd_test(), s);
    std::printf("TF (no prune, gm_ro off):\n  %s\n", cr.text.c_str());

    s.gm_ro_assume = true;
    cr = syms::run_analysis(cd_test(), s);
    std::printf("\nTF (no prune, gm_ro ON):\n  %s\n", cr.text.c_str());

    s.prune = true;
    s.gm_ro_assume = false;
    cr = syms::run_analysis(cd_test(), s);
    std::printf("\nTF (prune, gm_ro off):\n  %s\n", cr.text.c_str());

    s.gm_ro_assume = true;
    cr = syms::run_analysis(cd_test(), s);
    std::printf("\nTF (prune, gm_ro ON):\n  %s\n", cr.text.c_str());
    std::printf("(expected: '1' when gm*ro dominates)\n");

    // A circuit where gm*ro is small (gm = 1u, ro = 100) -- the idealization
    // must NOT fire (gm*ro = 1e-4, no longer >> 1). The exact form is
    // gm*ro / (1 + gm*ro) and the printed form should keep that.
    Circuit small;
    small.comps.push_back(mkv(Kind::V, "V1", {"in", "0"}, "1"));
    Component m = mkv(Kind::NMOS, "M1", {"VDD", "in", "out"});
    m.param_text["gm"] = "1u";
    m.param_text["ro"] = "100";
    m.param_on["ro"] = true;
    m.param_on["Cgs"] = false;
    m.param_on["Cgd"] = false;
    m.param_on["Cds"] = false;
    small.comps.push_back(m);
    small.comps.push_back(mkv(Kind::V, "VDD", {"VDD", "0"}, "5"));
    s.prune = true;
    s.gm_ro_assume = true;
    cr = syms::run_analysis(small, s);
    std::printf("\nsmall gm*ro (prune, gm_ro ON):\n  %s\n", cr.text.c_str());
    std::printf("(expected: gm*ro/(1+gm*ro) -- idealization must NOT fire)\n");
    return 0;
}
