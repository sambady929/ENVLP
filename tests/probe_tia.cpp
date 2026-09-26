#include "app/Document.h"
#include "core/Analysis.h"
#include "core/Engine.h"
#include <cstdio>
#include <sstream>
#include <string>

static std::string str(const GiNaC::ex& e) { std::ostringstream os; os << e; return os.str(); }

int main() {
    symcirc::Document d;
    std::string err;
    if (!d.load("D:/Projects/Programming/SymCirc/examples/tia_lg.scx", err)) {
        std::printf("load failed: %s\n", err.c_str());
        return 1;
    }
    syms::Circuit c = d.resolved(err);

    for (const auto& comp : c.comps) {
        std::printf("%s(%d) value='%s' nodes:", comp.ref.c_str(), (int)comp.kind,
                    comp.value_text.c_str());
        for (auto& n : comp.nodes) std::printf(" %s", n.c_str());
        std::printf("\n");
    }

    for (int kind : {(int)syms::AnalysisKind::TransferFunction,
                     (int)syms::AnalysisKind::LoopGain}) {
        syms::AnalysisSpec sp;
        sp.kind = (syms::AnalysisKind)kind;
        sp.input_ref = "I1";
        sp.output = d.req.output;
        sp.probe_ref = "U1";
        sp.sweep = d.req.sweep;
        sp.f0_hz = d.req.sweep.f_start_hz;
        sp.prune = true;
        sp.use_parallel = true;
        std::printf("\n===== kind=%d =====\n", kind);
        try {
            syms::CardResult cr = syms::run_analysis(c, sp);
            std::printf("raw num = %s\n", str(cr.transfer.num_raw.expand()).c_str());
            std::printf("raw den = %s\n", str(cr.transfer.den_raw.expand()).c_str());
            std::printf("gain    = %s\n", str(cr.transfer.pruned.gain).c_str());
            std::printf("--- estimates ---\n");
            for (const auto& kv : cr.transfer.params.est)
                std::printf("  %s = %g\n", kv.first.c_str(), kv.second);
            std::printf("--- poles ---\n");
            for (const auto& p : cr.transfer.pruned.poles)
                std::printf("  %s\n", p.factor_text.c_str());
            std::printf("summary: %s\n", cr.summary.c_str());
        } catch (const std::exception& e) {
            std::printf("EXCEPTION: %s\n", e.what());
        }
    }
    return 0;
}
