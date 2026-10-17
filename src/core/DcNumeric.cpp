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
    double Vth = std::fabs(m.vto); // magnitude (a PMOS card stores a negative vto)
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
    // gm and gds are DERIVATIVES of the (signed) drain current: d(id)/d(vgs) and
    // d(id)/d(vds). For a PMOS both come out positive (id falls as vgs rises,
    // and vgs = -|vgs|), so they are left as the magnitudes computed above; only
    // the current itself is negated.
    return e;
}

} // namespace

NumericDcResult solve_dc_numeric(const Circuit& c, const MosModel& nmos_model,
                                 const MosModel& pmos_model, double vdd) {
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
    bool seen_vdd = false;
    for (const auto& cc : c.comps) {
        if (cc.kind == Kind::V) {
            double v = 0.0;
            eng::parse_value(cc.dc_text, v);
            branches.push_back({cc.ref, N(cc.nodes[0]), N(cc.nodes[1]), v});
        } else if (cc.kind == Kind::VDD) {
            // The rail voltage is a DC-analysis setting (tech.vdd), not a
            // per-symbol value. VDD is a single anonymous supply net, so
            // several VDD symbols are electrically one source on the same
            // rail: stamping a branch per symbol adds identical rows and makes
            // the Jacobian singular. (build_mna collapses them for exactly this
            // reason; the numeric solver has to as well.)
            if (seen_vdd) continue;
            seen_vdd = true;
            branches.push_back({cc.ref, N(cc.nodes[0]), -1, vdd});
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
    const int kMaxIter = 300;
    // Node-voltage convergence. The absolute term is deliberately larger than
    // SPICE's VNTOL: a device biased right at the triode/saturation boundary
    // (Vds ~ 0, e.g. a mirror with equal input/output currents) leaves a node
    // there, and the device-voltage limiting then jitters by a few microvolts.
    // This solve only picks each device's region, so microvolts do not matter.
    const double kVTol = 1e-5;
    const double kResTol = 1e-9; // KCL/KVL residual convergence
    // Relative tolerances, SPICE style: an absolute 1e-9 on both volts and
    // amps is unreachable for a mA-scale bias (the device-voltage limiting
    // never settles the last few ppm), so a perfectly good operating point
    // would be reported as "did not converge". A term is converged when it is
    // within `kRelTol` of its own magnitude. (SPICE's RELTOL default is 1e-3;
    // 1e-4 is tighter and still far more than the region check needs.)
    const double kRelTol = 1e-4;
    double f_scale = 0.0; // first-iteration KCL residual: the current scale

    // --- SPICE device-voltage limiting (fetlim) ----------------------------
    // Limits how far vgs/vds may move between Newton iterations. This is what
    // makes a multi-device bias converge instead of oscillating across the
    // threshold; it is the core of a SPICE DC solve.
    auto fetlim = [](double vnew, double vold, double vto) {
        const double vtsthi = std::fabs(2.0 * (vold - vto)) + 2.0;
        const double vtstlo = vtsthi / 2.0 + 2.0;
        const double vtox = vto + 3.5;
        const double delv = vnew - vold;
        if (vold >= vto) {
            if (vold >= vtox) {
                if (delv <= 0.0) {
                    if (vnew >= vtox) {
                        if (-delv > vtstlo) vnew = vold - vtstlo;
                    } else {
                        vnew = std::max(vnew, vto + 2.0);
                    }
                } else if (delv >= vtsthi) {
                    vnew = vold + vtsthi;
                }
            } else {
                if (delv <= 0.0) vnew = std::max(vnew, vto - 0.5);
                else vnew = std::min(vnew, vto + 4.0);
            }
        } else {
            if (delv <= 0.0) {
                if (-delv > vtsthi) vnew = vold - vtsthi;
            } else {
                const double vtemp = vto + 0.5;
                if (vnew <= vtemp) {
                    if (delv > vtstlo) vnew = vold + vtstlo;
                } else {
                    vnew = vtemp;
                }
            }
        }
        return vnew;
    };
    auto fetlim_vds = [](double vnew, double vold) {
        const double vtsthi = std::fabs(2.0 * vold) + 2.0;
        const double vtstlo = vtsthi / 2.0 + 2.0;
        const double delv = vnew - vold;
        if (vold >= 0.0) {
            if (delv <= 0.0) {
                if (vold >= 3.5) {
                    if (-delv > vtstlo) vnew = vold - vtstlo;
                } else {
                    vnew = std::max(vnew, -0.5);
                }
            } else if (delv >= vtsthi) {
                vnew = vold + vtsthi;
            }
        } else {
            if (delv <= 0.0) {
                if (-delv > vtsthi) vnew = vold - vtsthi;
            } else if (vnew <= -0.5) {
                if (delv > vtstlo) vnew = vold + vtstlo;
            } else {
                vnew = -0.5;
            }
        }
        return vnew;
    };
    const int n_mos = int(mos_model.size());
    std::vector<double> lim_vgs(n_mos, 0.0), lim_vds(n_mos, 0.0);

    // `src_scale` scales every independent source for source stepping.
    double src_scale = 1.0;

    // Newton for one gmin. The device voltages are limited each iteration
    // (fetlim) so the iteration creeps up to the bias instead of jumping across
    // the threshold and oscillating.
    auto newton = [&](double gg_min, std::vector<double> xx,
                      std::vector<double>& xout) -> bool {
        for (int iter = 0; iter < kMaxIter; ++iter) {
            std::vector<std::vector<double>> J(nvars,
                                               std::vector<double>(nvars, 0.0));
            std::vector<double> F(nvars, 0.0);
            auto stamp_conductance = [&](int a, int b, double gg) {
                if (a >= 0) J[a][a] += gg;
                if (b >= 0) J[b][b] += gg;
                if (a >= 0 && b >= 0) { J[a][b] -= gg; J[b][a] -= gg; }
            };
            // gmin: a conductance from every node to ground, keeping a node
            // reached only through the drain of an off device well posed.
            for (size_t i = 0; i < nodes.size(); ++i) {
                J[i][i] += gg_min;
                F[i] += xx[i] * gg_min;
            }
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
            for (const auto& sq : isrcs) {
                double v = sq.value * src_scale;
                if (sq.a >= 0) F[sq.a] += v;
                if (sq.b >= 0) F[sq.b] -= v;
            }
            for (int k = 0; k < nb; ++k) {
                const Branch& br = branches[k];
                int row = int(nodes.size()) + k;
                if (br.a >= 0) { J[br.a][row] += 1; J[row][br.a] += 1; }
                if (br.b >= 0) { J[br.b][row] -= 1; J[row][br.b] -= 1; }
                if (br.a >= 0) F[br.a] += xx[row];
                if (br.b >= 0) F[br.b] -= xx[row];
                double vl = (br.a >= 0 ? xx[br.a] : 0.0) -
                            (br.b >= 0 ? xx[br.b] : 0.0);
                F[row] += vl - br.value * src_scale;
            }
            int mi = 0;
            for (const auto& cc : c.comps) {
                if (cc.kind != Kind::NMOS && cc.kind != Kind::PMOS) continue;
                int D = N(cc.nodes[0]), G = N(cc.nodes[1]), S = N(cc.nodes[2]);
                const MosModel& mm = *mos_model[mi];
                const double sgn = mm.pmos ? -1.0 : 1.0;
                double vgs = (G >= 0 ? xx[G] : 0.0) - (S >= 0 ? xx[S] : 0.0);
                double vds = (D >= 0 ? xx[D] : 0.0) - (S >= 0 ? xx[S] : 0.0);
                // Limit in magnitude (a PMOS is evaluated as |vgs|, |vds|).
                double lvgs_m =
                    fetlim(sgn * vgs, lim_vgs[mi], std::fabs(mm.vto));
                double lvds_m = fetlim_vds(sgn * vds, lim_vds[mi]);
                lim_vgs[mi] = lvgs_m;
                lim_vds[mi] = lvds_m;
                MosEval e = eval_mos(mm, mos_wl[mi].first, mos_wl[mi].second,
                                     sgn * lvgs_m, sgn * lvds_m);
                ++mi;
                // Id leaves D and enters S: Id = gm*(vG-vS) + gds*(vD-vS).
                // The transconductance is a VCCS (D<-G), NOT a conductance
                // between G and S (the gate draws no current).
                auto stamp_vccs = [&](int op, int on, int cp, int cn, double gg) {
                    if (op >= 0) {
                        if (cp >= 0) J[op][cp] += gg;
                        if (cn >= 0) J[op][cn] -= gg;
                    }
                    if (on >= 0) {
                        if (cp >= 0) J[on][cp] -= gg;
                        if (cn >= 0) J[on][cn] += gg;
                    }
                };
                auto stamp_pair = [&](int p, int q, double gg) {
                    if (p >= 0) J[p][p] += gg;
                    if (q >= 0) J[q][q] += gg;
                    if (p >= 0 && q >= 0) { J[p][q] -= gg; J[q][p] -= gg; }
                };
                stamp_vccs(D, S, G, S, e.gm);
                stamp_pair(D, S, e.gds);
                if (D >= 0) F[D] += e.id;
                if (S >= 0) F[S] -= e.id;
            }

            double fmax = 0.0;
            for (double f : F) fmax = std::max(fmax, std::fabs(f));
            if (iter == 0) f_scale = fmax;
            std::vector<double> rhs(nvars, 0.0);
            for (int i = 0; i < nvars; ++i) rhs[i] = -F[i];
            std::vector<double> dx;
            if (nvars == 0) { xout = xx; return true; }
            if (!solve_linear(J, rhs, dx)) return false;
            // Cap the node-voltage step so a near-singular iterate cannot throw
            // a node 1e14 volts away; fetlim keeps the device model sane.
            double vstep = 0.0;
            for (size_t i = 0; i < nodes.size(); ++i)
                vstep = std::max(vstep, std::fabs(dx[i]));
            const double kStepLim = 1.0;
            if (vstep > kStepLim) {
                double sc = kStepLim / vstep;
                for (int i = 0; i < nvars; ++i) dx[i] *= sc;
            }
            double vmax = 0.0;
            for (int i = 0; i < nvars; ++i) xx[i] += dx[i];
            for (size_t i = 0; i < nodes.size(); ++i)
                vmax = std::max(vmax, std::fabs(dx[i]));
            // A node is settled when its step is small in absolute terms or
            // small relative to the voltage already on it.
            bool vok = true;
            for (size_t i = 0; i < nodes.size(); ++i)
                if (std::fabs(dx[i]) > kVTol + kRelTol * std::fabs(xx[i]))
                    vok = false;
            if (vok && fmax < kResTol + kRelTol * f_scale) {
                xout = xx;
                return true;
            }
        }
        return false;
    };

    // gmin stepping: solve with an artificial node-to-ground leak, then shrink
    // it by decades, each time resuming from the previous solution. If that
    // fails, fall back to source stepping (ramp every source from 0).
    std::vector<double> best = x;
    bool converged = false;
    std::fill(lim_vgs.begin(), lim_vgs.end(), 0.0);
    std::fill(lim_vds.begin(), lim_vds.end(), 0.0);
    for (int dec = 3; dec <= 12; ++dec) {
        // Exact decades (0.1 multiplication drifts, which made a "is the last
        // gmin small enough" test fail on a rounding error).
        const double gg = std::pow(10.0, -dec);
        std::vector<double> xo;
        if (newton(gg, x, xo)) {
            x = xo;
            best = xo;
            converged = true;
        } else {
            converged = false;
            break;
        }
    }
    if (!converged) {
        std::vector<double> xs(nvars, 0.0);
        bool any = false;
        for (int step = 0; step <= 10; ++step) {
            src_scale = step / 10.0;
            std::vector<double> cur = xs;
            bool step_ok = false;
            for (int dec = 3; dec <= 12; ++dec) {
                const double gg = std::pow(10.0, -dec);
                std::vector<double> xo;
                if (newton(gg, cur, xo)) {
                    cur = xo;
                    step_ok = true;
                } else {
                    step_ok = false;
                    break;
                }
            }
            if (!step_ok) { any = false; break; }
            xs = cur;
            any = true;
        }
        if (any) {
            x = xs;
            best = xs;
            converged = true;
        }
    }
    if (!converged) {
        out.error = "DC (numeric): Newton iteration did not converge -- a "
                    "device biased right at the triode/saturation boundary "
                    "leaves a node that the iteration cannot settle";
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


