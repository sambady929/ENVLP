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
    c.comps.push_back(mkv(Kind::V, "V1", {"in", "0"}, "1"));
    Component m = mkv(Kind::NMOS, "M1", {"VDD", "in", "out"});
    m.param_text["gm"] = "1m";
    m.param_text["ro"] = "100k";
    m.param_on["ro"] = true;
    m.param_on["Cgs"] = false;
    m.param_on["Cgd"] = false;
    c.comps.push_back(m);
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
    syms::CardResult cr = syms::run_analysis(cd_test(), s);
    std::printf("TF (no prune):\n  %s\n", cr.text.c_str());

    s.prune = true;
    cr = syms::run_analysis(cd_test(), s);
    std::printf("\nTF (prune):\n  %s\n", cr.text.c_str());
    std::printf("(expected: gm*ro/(1+gm*ro) ~ 1 for gm*ro = 100)\n");

    // A circuit where gm*ro is small (gm = 1u, ro = 100): the "+1" is no
    // longer negligible, so the exact form gm*ro/(1+gm*ro) must survive.
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
    cr = syms::run_analysis(small, s);
    std::printf("\nsmall gm*ro (prune):\n  %s\n", cr.text.c_str());
    std::printf("(expected: gm*ro/(1+gm*ro) -- the +1 must survive)\n");
    return 0;
}
