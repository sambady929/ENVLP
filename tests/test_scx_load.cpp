// Debug harness: load the example .scx through Document, resolve its nets,
// print every pin's assigned node name, then run a transfer-function analysis
// exactly as the GUI does (so regressions like "unknown node" surface here).
#include "app/Document.h"
#include "app/LatexRender.h"

#include "core/Analysis.h"

#include <cstdio>

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1]
                                : "D:/Projects/Programming/SymCirc/examples/"
                                  "cs_test.scx";
    symcirc::Document d;
    std::string err;
    if (!d.load(path, err)) {
        std::printf("load failed: %s\n", err.c_str());
        return 1;
    }
    std::printf("comps=%zu wires=%zu labels=%zu\n", d.circuit.comps.size(),
                d.wires.size(), d.labels.size());

    syms::Circuit c = d.resolved(err);
    if (!err.empty()) {
        std::printf("resolve error: %s\n", err.c_str());
    }
    for (const auto& comp : c.comps) {
        std::printf("%s:", comp.ref.c_str());
        for (const auto& n : comp.nodes) std::printf(" %s", n.c_str());
        std::printf("\n");
    }

    // Run EVERY analysis kind the example ships with, exactly as the GUI's
    // run_card(-1) does. Any card that throws is fatal for the test; the GUI
    // would show a MessageBox and leave the results window empty.
    struct K { syms::AnalysisKind kind; const char* name; };
    const K kinds[] = {
        {syms::AnalysisKind::TransferFunction, "TF"},
        {syms::AnalysisKind::AC, "AC"},
        {syms::AnalysisKind::DC, "DC"},
        {syms::AnalysisKind::InputImpedance, "Zin"},
        {syms::AnalysisKind::OutputImpedance, "Zout"},
        {syms::AnalysisKind::ShortCircuitCurrent, "Isc"},
        // Loop gain / PSRR need extra probe setup not present in this example,
        {syms::AnalysisKind::Noise, "Noise"},
    };
    int fails = 0;
    for (const auto& k : kinds) {
        syms::AnalysisSpec sp;
        sp.kind = k.kind;
        sp.input_ref = d.req.input_ref;
        sp.output = d.req.output;
        sp.sweep = d.req.sweep;
        sp.f0_hz = d.req.sweep.f_start_hz;
        sp.prune = d.req.prune;
        sp.use_parallel = d.req.use_parallel;
        try {
            syms::CardResult cr = syms::run_analysis(c, sp);
            std::printf("  %-6s OK  %s\n", k.name, cr.summary.c_str());
        } catch (const std::exception& ex) {
            std::printf("  %-6s FAIL: %s\n", k.name, ex.what());
            ++fails;
        }
    }
    if (fails) {
        std::printf("\n%d card(s) failed\n", fails);
        return 1;
    }
    std::printf("\nall cards OK\n");

    // Item 6 sanity check: the engine's LaTeX output should be a non-empty
    // string that round-trips through the offline HTML renderer (the Math tab
    // in the GUI). If it ever stops being valid LaTeX, the renderer would
    // emit empty HTML.
    {
        syms::AnalysisSpec sp;
        sp.kind = syms::AnalysisKind::TransferFunction;
        sp.input_ref = d.req.input_ref;
        sp.output = d.req.output;
        sp.sweep = d.req.sweep;
        sp.f0_hz = d.req.sweep.f_start_hz;
        sp.prune = d.req.prune;
        sp.use_parallel = d.req.use_parallel;
        syms::CardResult cr = syms::run_analysis(c, sp);
        if (cr.latex.empty()) {
            std::printf("LaTeX: empty!\n");
            return 1;
        }
        std::printf("LaTeX length=%zu head='%.80s...'\n", cr.latex.size(),
                    cr.latex.c_str());
        // Round-trip through the offline renderer used by the Math tab.
        std::string html = symcirc::latex_to_html(cr.latex);
        if (html.find("class=\"frac\"") == std::string::npos &&
            cr.latex.find("\\frac") != std::string::npos) {
            std::printf("renderer dropped \\frac!\n");
            return 1;
        }
        if (html.find("&#8741;") == std::string::npos &&
            cr.latex.find("\\parallel") != std::string::npos) {
            std::printf("renderer dropped \\parallel!\n");
            return 1;
        }
        std::printf("LaTeX -> HTML round-trip OK (size %zu)\n", html.size());
    }
    return 0;
}
