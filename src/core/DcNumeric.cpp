#include "core/DcNumeric.h"
#include "core/Eng.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>

namespace syms {

namespace {

// --- small dense linear solve (Gaussian elimination, partial pivoting) ------
bool solve_linear(std::vector<std::vector<double>>& A, std::vector<double>& b,
                  std::vector<double>& x) {
    int n = int(b.size());
    for (int col = 0; col < n; ++col) {
        int piv = col;
        double best = std::fabs(A[col][col]);
        for (int r = col + 1; r < n; ++r)
            if (std::fabs(A[r][col]) > best) { best = std::fabs(A[r][col]); piv = r; }
        if (best < 1e-18) return false;
        if (piv != col) {
            std::swap(A[piv], A[col]);
            std::swap(b[piv], b[col]);
        }
        double d = A[col][col];
        for (int r = col + 1; r < n; ++r) {
            double f = A[r][col] / d;
            if (f == 0.0) continue;
            for (int c = col; c < n; ++c) A[r][c] -= f * A[col][c];
            b[r] -= f * b[col];
        }
    }
    x.assign(n, 0.0);
    for (int r = n - 1; r >= 0; --r) {
        double s = b[r];
        for (int c = r + 1; c < n; ++c) s -= A[r][c] * x[c];
        x[r] = s / A[r][r];
    }
    return true;
}

// One MOSFET's drain current and its derivatives at a bias. Signed by
// polarity: for a PMOS everything is evaluated in magnitudes and the returned
// current is negated (drain current flows out of the source).
struct MosEval {
    double id = 0.0;   // signed D->S current
    double gm = 0.0;   // dId/dVgs (signed)
    double gds = 0.0;  // dId/dVds (signed)
    bool saturated = true;
    double vov = 0.0;  // signed overdrive
};

MosEval eval_mos(const MosModel& m, double w, double l, double vgs, double vds) {
    MosEval e;
    const double sgn = m.pmos ? -1.0 : 1.0;
    // Work in magnitudes for a PMOS.
    double Vgs = sgn * vgs;      // >= Vth to conduct
    double Vds = sgn * vds;
    double W = std::max(w, 1e-12);
    double L = std::max(l, 1e-12);
    double Vth = m.vto + (m.pmos ? 0.0 : 0.0);
    // (No body terminal: the body is tied to the source, so Vbs = 0 and the
    // body-effect term drops out.)
    double Vov = Vgs - Vth;
    e.vov = sgn * Vov;

    if (Vov <= 0.0) {
        e.id = e.gm = e.gds = 0.0;
        e.saturated = false;
        return e;
    }
    const double k = m.kp * W / L;
    // Output-conductance factor. lambda describes channel-length modulation in
    // saturation (Vds > 0); for Vds < 0 it must NOT apply or (1+lambda*Vds)
    // turns negative and flips the triode current, manufacturing spurious roots.
    const double lam = 1.0 + m.lambda * std::max(Vds, 0.0);
    if (m.level >= 3) {
        // Simplified level 3: mobility degradation + DIBL + lambda.
        double Vth_eff = Vth - m.eta * Vds;         // DIBL pulls Vth down
        double Vov3 = Vgs - Vth_eff;
        if (Vov3 <= 0.0) { e.saturated = false; return e; }
        double denom = 1.0 + m.theta * Vov3;
        double vdsat = Vov3 / denom;
        if (Vds >= vdsat) {
            e.saturated = true;
            double id0 = 0.5 * k * Vov3 * Vov3 / denom;
            e.id = id0 * lam;
            e.gm = k * Vov3 / (denom * denom) * lam;
            e.gds = id0 * m.lambda;
        } else {
            e.saturated = false;
            // Gradual channel: a smooth quadratic between 0 and vdsat.
            double f = Vds / vdsat;
            e.id = 0.5 * k * Vov3 * Vov3 / denom * (2.0 * f - f * f) * lam;
            e.gm = 0.9 * e.id / std::max(Vov3, 1e-6);
            e.gds = e.id / std::max(Vds, 1e-9);
        }
    } else {
        // Level 1 square law.
        double vdsat = Vov;
        if (Vds >= vdsat) {
            e.saturated = true;
            e.id = 0.5 * k * Vov * Vov * lam;
            e.gm = k * Vov * lam;
            e.gds = 0.5 * k * Vov * Vov * m.lambda;
        } else {
            e.saturated = false;
            e.id = k * (Vov - 0.5 * Vds) * Vds * lam;
            e.gm = k * Vds * lam;
            e.gds = k * (Vov - Vds) * lam +
                    k * (Vov - 0.5 * Vds) * Vds * m.lambda;
        }
    }
    e.id *= sgn;
    e.gm *= sgn;
    e.gds *= sgn;
    return e;
}

} // namespace

NumericDcResult solve_dc_numeric(const Circuit& c, const MosModel& nmos_model,
                                 const MosModel& pmos_model) {
    NumericDcResult out;

    // --- node index (ground excluded) ---
    std::vector<std::string> nodes;
    std::set<std::string> seen;
    auto add_node = [&](const std::string& raw) {
        std::string nd = (raw == "GND") ? "0" : raw;
        if (nd == "0") return;
        if (seen.insert(nd).second) nodes.push_back(nd);
    };
    for (const auto& cc : c.comps)
        for (const auto& n : cc.nodes) add_node(n);
    std::map<std::string, int> nidx;
    for (size_t i = 0; i < nodes.size(); ++i) nidx[nodes[i]] = int(i);
    auto N = [&](const std::string& raw) -> int {
        std::string nd = (raw == "GND") ? "0" : raw;
        if (nd == "0") return -1;
        auto it = nidx.find(nd);
        return it == nidx.end() ? -1 : it->second;
    };

    // --- voltage-source / inductor branches (extra current unknowns) ---
    struct Branch { std::string ref; int a, b; double value; };
    std::vector<Branch> branches;
    for (const auto& cc : c.comps) {
        if (cc.kind == Kind::V) {
            double v = 0.0;
            eng::parse_value(cc.dc_text, v);
            branches.push_back({cc.ref, N(cc.nodes[0]), N(cc.nodes[1]), v});
        } else if (cc.kind == Kind::VDD) {
            double v = 0.0;
            eng::parse_value(cc.value_text, v);
            branches.push_back({cc.ref, N(cc.nodes[0]), -1, v});
        } else if (cc.kind == Kind::L) {
            branches.push_back({cc.ref, N(cc.nodes[0]), N(cc.nodes[1]), 0.0});
        }
    }
    int nb = int(branches.size());
    int nvars = int(nodes.size()) + nb;

    // --- current sources ---
    struct Isrc { int a, b; double value; };
    std::vector<Isrc> isrcs;
    for (const auto& cc : c.comps) {
        if (cc.kind == Kind::I) {
            double v = 0.0;
            eng::parse_value(cc.dc_text, v);
            isrcs.push_back({N(cc.nodes[0]), N(cc.nodes[1]), v});
        }
    }

    std::vector<std::pair<double, double>> mos_wl; // per MOSFET: W, L
    std::vector<const MosModel*> mos_model;
    for (const auto& cc : c.comps) {
        if (cc.kind != Kind::NMOS && cc.kind != Kind::PMOS) continue;
        mos_wl.push_back({cc.param_estimate("W"), cc.param_estimate("L")});
        mos_model.push_back(cc.kind == Kind::NMOS ? &nmos_model : &pmos_model);
    }

    // Initial guess: all node voltages 0 (the standard SPICE start). A device
    // then starts off, but the gmin stepping below conditions every node, and
    // the Newton follows the physical branch up rather than a spurious one.
    std::vector<double> x(nvars, 0.0);
    const int kMaxIter = 200;
    const double kTol = 1e-12;

    // Newton for one gmin. Operates on a copy so a failed step cannot corrupt
    // the last good solution; commits through `xout` on success.
    auto newton = [&](double gg_min, std::vector<double> xx,
                      std::vector<double>& xout) -> bool {
        for (int iter = 0; iter < kMaxIter; ++iter) {
            // Convention: F[node] = sum of currents LEAVING the node (KCL), and
            // the Jacobian J = dF/dx. Newton then solves J*dx = -F, x += dx.
            std::vector<std::vector<double>> J(nvars,
                                               std::vector<double>(nvars, 0.0));
            std::vector<double> F(nvars, 0.0);

            auto stamp_conductance = [&](int a, int b, double gg) {
                if (a >= 0) J[a][a] += gg;
                if (b >= 0) J[b][b] += gg;
                if (a >= 0 && b >= 0) { J[a][b] -= gg; J[b][a] -= gg; }
            };

            // gmin: a conductance from every node to ground. It keeps a node
            // reached only through the drain of an off device well posed, and
            // (stepped from coarse to fine below) removes the spurious
            // all-devices-off root that a plain Newton can fall into.
            for (size_t i = 0; i < nodes.size(); ++i) {
                J[i][i] += gg_min;
                F[i] += xx[i] * gg_min;
            }

            // Resistors: current leaving a = (v_a - v_b)/R.
            for (const auto& cc : c.comps) {
                if (cc.kind != Kind::R) continue;
                double r = cc.estimate();
                if (r <= 0.0) continue;
                int a = N(cc.nodes[0]), b = N(cc.nodes[1]);
                double gg = 1.0 / r;
                stamp_conductance(a, b, gg);
                if (a >= 0) F[a] += (xx[a] - (b >= 0 ? xx[b] : 0.0)) * gg;
                if (b >= 0) F[b] += (xx[b] - (a >= 0 ? xx[a] : 0.0)) * gg;
            }
            // Independent current sources: value flows n+ -> n- through the
            // source, i.e. current LEAVES n+ and ENTERS n-.
            for (const auto& sq : isrcs) {
                if (sq.a >= 0) F[sq.a] += sq.value;
                if (sq.b >= 0) F[sq.b] -= sq.value;
            }
            // Voltage-source / inductor branches (current unknown at row).
            for (int k = 0; k < nb; ++k) {
                const Branch& br = branches[k];
                int row = int(nodes.size()) + k;
                if (br.a >= 0) { J[br.a][row] += 1; J[row][br.a] += 1; }
                if (br.b >= 0) { J[br.b][row] -= 1; J[row][br.b] -= 1; }
                if (br.a >= 0) F[br.a] += xx[row];
                if (br.b >= 0) F[br.b] -= xx[row];
                double vl = (br.a >= 0 ? xx[br.a] : 0.0) -
                            (br.b >= 0 ? xx[br.b] : 0.0);
                F[row] += vl - br.value;
            }
            // MOSFETs: current leaves D by Ids, enters S.
            int mi = 0;
            for (const auto& cc : c.comps) {
                if (cc.kind != Kind::NMOS && cc.kind != Kind::PMOS) continue;
                int D = N(cc.nodes[0]), G = N(cc.nodes[1]), S = N(cc.nodes[2]);
                double vgs = (G >= 0 ? xx[G] : 0.0) - (S >= 0 ? xx[S] : 0.0);
                double vds = (D >= 0 ? xx[D] : 0.0) - (S >= 0 ? xx[S] : 0.0);
                MosEval e = eval_mos(*mos_model[mi], mos_wl[mi].first,
                                     mos_wl[mi].second, vgs, vds);
                ++mi;
                if (D >= 0) F[D] += e.id;
                if (S >= 0) F[S] -= e.id;
                auto stamp_pair = [&](int p, int q, double gg) {
                    if (p >= 0) J[p][p] += gg;
                    if (q >= 0) J[q][q] += gg;
                    if (p >= 0 && q >= 0) { J[p][q] -= gg; J[q][p] -= gg; }
                };
                stamp_pair(G, S, e.gm);
                stamp_pair(D, S, e.gds);
            }

            std::vector<double> rhs(nvars, 0.0);
            for (int i = 0; i < nvars; ++i) rhs[i] = -F[i];
            std::vector<double> dx;
            if (nvars == 0) { xout = xx; return true; }
            if (!solve_linear(J, rhs, dx)) return false;
            // Voltage limiting: clamp the largest node-voltage step. A full
            // Newton step can overshoot past a device threshold and oscillate
            // between the off and saturated regions; limiting the step makes it
            // creep up to the physical bias instead. Near convergence the step
            // is tiny, so limiting does not slow the final iterations.
            double scale = 1.0;
            const double kStepLim = 0.5;
            for (int i = 0; i < nvars; ++i)
                if (std::fabs(dx[i]) > kStepLim)
                    scale = std::min(scale, kStepLim / std::fabs(dx[i]));
            double maxd = 0.0;
            for (int i = 0; i < nvars; ++i) {
                xx[i] += scale * dx[i];
                maxd = std::max(maxd, std::fabs(scale * dx[i]));
            }
            if (maxd < kTol) { xout = xx; return true; }
        }
        return false;
    };

    // gmin stepping: solve with an artificially large node-to-ground leak, then
    // shrink it by decades, each time resuming from the previous solution. The
    // leak keeps every node conditioned while the bias establishes, so Newton
    // cannot land on the degenerate all-off root.
    bool converged = false;
    std::vector<double> best = x;
    for (double gg = 1e-3; gg >= 1e-12; gg *= 0.1) {
        std::vector<double> xo;
        if (newton(gg, x, xo)) {
            x = xo;
            best = xo;
            converged = true;
        } else {
            break; // keep the last converged (coarser) solution
        }
    }
    if (!converged) {
        out.error = "DC (numeric): Newton iteration did not converge -- check "
                    "for floating nodes or a device with no DC path";
        return out;
    }
    x = best;

    for (size_t i = 0; i < nodes.size(); ++i) out.node_v[nodes[i]] = x[i];
    // Operating points.
    int mi = 0;
    for (const auto& cc : c.comps) {
        if (cc.kind != Kind::NMOS && cc.kind != Kind::PMOS) continue;
        int D = N(cc.nodes[0]), G = N(cc.nodes[1]), S = N(cc.nodes[2]);
        double vgs = (G >= 0 ? x[G] : 0.0) - (S >= 0 ? x[S] : 0.0);
        double vds = (D >= 0 ? x[D] : 0.0) - (S >= 0 ? x[S] : 0.0);
        MosEval e = eval_mos(*mos_model[mi], mos_wl[mi].first,
                             mos_wl[mi].second, vgs, vds);
        MosOpPoint op;
        op.ref = cc.ref;
        op.vgs = vgs;
        op.vds = vds;
        op.vov = e.vov;
        op.id = e.id;
        op.gm = std::fabs(e.gm);
        op.gds = std::fabs(e.gds);
        op.saturated = e.saturated;
        op.in_triode = !e.saturated && std::fabs(e.id) > 0.0;
        out.mos.push_back(op);
        ++mi;
    }
    out.ok = true;
    return out;
}

} // namespace syms
