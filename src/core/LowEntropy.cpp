#include "core/LowEntropy.h"
#include "core/Eng.h"
#include "core/Par.h"
#include "core/Print.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <sstream>

namespace syms {

using GiNaC::ex;
using GiNaC::exmap;
using GiNaC::is_a;
using GiNaC::numeric;

namespace {
// LaTeX printer with explicit \cdot between multiplied factors (defined
// below; forward-declared so the root-table builder can use it).
std::string to_latex_cdot(const ex& e);
}

// ---------------------------------------------------------------------------
// numeric evaluation
// ---------------------------------------------------------------------------
std::complex<double> eval_complex(const ex& e, const ParamTable& params,
                                  double omega) {
    exmap m;
    for (const auto& kv : params.est) {
        auto it = params.syms.find(kv.first);
        if (it != params.syms.end()) m[it->second] = ex(kv.second);
    }
    auto sit = params.syms.find("s");
    if (sit != params.syms.end())
        m[sit->second] = ex(GiNaC::I) * omega;

    ex v;
    try {
        v = e.subs(m).evalf();
    } catch (...) {
        // e.g. a pole at DC (1/s) evaluating at s=0 -> division by zero.
        return {std::numeric_limits<double>::quiet_NaN(),
                std::numeric_limits<double>::quiet_NaN()};
    }
    if (!is_a<numeric>(v))
        return {std::numeric_limits<double>::quiet_NaN(),
                std::numeric_limits<double>::quiet_NaN()};
    numeric n = GiNaC::ex_to<numeric>(v);
    return {n.real().to_double(), n.imag().to_double()};
}

double eval_mag_db(const ex& e, const ParamTable& params, double omega) {
    std::complex<double> z = eval_complex(e, params, omega);
    double mag = std::hypot(z.real(), z.imag());
    if (!(mag > 0.0)) return -std::numeric_limits<double>::infinity();
    return 20.0 * std::log10(mag);
}

double eval_phase_deg(const ex& e, const ParamTable& params, double omega) {
    std::complex<double> z = eval_complex(e, params, omega);
    return std::atan2(z.imag(), z.real()) * 180.0 / M_PI;
}

namespace {
ex factor_common_impl(const ex& e);

// Exact polynomial divisibility of `num` (a polynomial in `s`) by `fac`.
// GiNaC's rem() fails when a coefficient is a held function (par(R1,ro)), so
// instead reduce the ratio with normal() and ask whether any power of `s`
// remains in the denominator. normal() cancels the gcd, so fac divides num
// exactly when the reduced denominator is free of `s`. (The candidates are
// monic 1 + tau*s, so a constant denominator cannot hide a non-divisor.)
bool poly_remainder_is_zero(const ex& num, const ex& fac, const ex& s) {
    if (fac.is_zero()) return false;
    ex q = (num / fac).normal();
    return !q.denom().has(s);
}
} // namespace

// ---------------------------------------------------------------------------
// polynomial roots (Durand-Kerner)
// ---------------------------------------------------------------------------
std::vector<std::complex<double>> poly_roots(std::vector<double> c) {
    while (!c.empty() && c.back() == 0.0) c.pop_back();
    std::vector<std::complex<double>> roots;
    int n = int(c.size()) - 1;
    if (n <= 0) return roots;

    // Quadratic: closed form (by far the most common case here).
    if (n == 2) {
        double a = c[2], b = c[1], cc = c[0];
        double disc = b * b - 4.0 * a * cc;
        if (disc >= 0.0) {
            double r = std::sqrt(disc);
            // numerically stable roots
            double q = -0.5 * (b + (b >= 0 ? r : -r));
            roots.push_back(std::complex<double>(q / a, 0.0));
            if (q != 0.0) roots.push_back(std::complex<double>(cc / q, 0.0));
            else roots.push_back(std::complex<double>(-b / a - q / a, 0.0));
        } else {
            std::complex<double> sq(0.0, std::sqrt(-disc));
            roots.push_back((-b + sq) / (2.0 * a));
            roots.push_back((-b - sq) / (2.0 * a));
        }
        std::sort(roots.begin(), roots.end(),
                  [](const std::complex<double>& x, const std::complex<double>& y) {
                      return x.real() < y.real();
                  });
        return roots;
    }

    // Higher orders: scale s so the roots are O(1), then Durand-Kerner.
    double scale = 0.0;
    for (int k = 0; k < n; ++k) {
        double r = std::fabs(c[k] / c[n]);
        if (r > 0.0) scale = std::max(scale, std::pow(r, 1.0 / (n - k)));
    }
    if (!(scale > 0.0) || !std::isfinite(scale)) scale = 1.0;
    std::vector<double> cs(n + 1);
    for (int k = 0; k <= n; ++k) cs[k] = c[k] * std::pow(scale, k);

    for (int i = 0; i < n; ++i) {
        double a2 = 2.0 * M_PI * (i + 0.5) / n;
        roots.push_back(std::complex<double>(0.4 * std::cos(a2),
                                             0.9 * std::sin(a2)));
    }
    std::complex<double> lead(cs[n], 0.0);
    for (int it = 0; it < 500; ++it) {
        double maxd = 0.0;
        for (int i = 0; i < n; ++i) {
            std::complex<double> z = roots[i];
            std::complex<double> p = lead;
            for (int k = n - 1; k >= 0; --k)
                p = p * z + std::complex<double>(cs[k], 0.0);
            std::complex<double> d(1.0, 0.0);
            for (int j = 0; j < n; ++j)
                if (j != i) d *= (z - roots[j]);
            if (std::abs(d) < 1e-300) d = std::complex<double>(1e-300, 0.0);
            std::complex<double> dz = p / d;
            roots[i] = z - dz;
            maxd = std::max(maxd, std::abs(dz));
        }
        if (maxd < 1e-13) break;
    }
    // undo the scaling: original roots are scaled roots / scale
    for (auto& r : roots) r /= scale;
    std::sort(roots.begin(), roots.end(),
              [](const std::complex<double>& a, const std::complex<double>& b) {
                  if (std::abs(a.real() - b.real()) > 1e-9)
                      return a.real() < b.real();
                  return a.imag() < b.imag();
              });
    return roots;
}

// ---------------------------------------------------------------------------
// term pruning
// ---------------------------------------------------------------------------
namespace {

struct Entry {
    int k;
    ex term;
    double db;
};

// Rank and drop negligible terms of `poly` (a polynomial in s). Mutates poly.
// Terms are ranked by their peak magnitude over the sweep band when a sweep is
// configured, otherwise at the single frequency f0. The ranking reference can
// be global (whole polynomial) or per s-coefficient.
void prune_poly(ex& poly, const ex& s, const ParamTable& pt,
                const LowEntropyOptions& o, const std::string& what,
                std::vector<Dropped>& dropped) {
    ex pe = poly.expand();
    int deg = 0;
    if (pe.has(s)) {
        try {
            deg = pe.degree(s);
        } catch (...) {
            return;
        }
        if (deg < 0 || deg > 64) return;
    }

    // Frequency band over which each term's contribution is taken to be its
    // worst case. For a term t*s^k the magnitude at w is |t|*w^k, which is
    // monotone in w for k >= 0, so the peak is at one end of the band.
    double w_lo, w_hi;
    if (o.band_hi_hz > 0 && o.band_hi_hz >= o.band_lo_hz) {
        w_lo = 2.0 * M_PI * std::max(o.band_lo_hz, 1e-9);
        w_hi = 2.0 * M_PI * std::max(o.band_hi_hz, 1e-9);
    } else {
        double f0 = o.f0_hz > 1e-12 ? o.f0_hz : 1e-12;
        w_lo = w_hi = 2.0 * M_PI * f0;
    }
    auto term_db = [&](const ex& t, int k) {
        std::complex<double> z = eval_complex(t, pt, 0.0);
        double mag = std::hypot(z.real(), z.imag());
        if (!(mag > 0.0) || !std::isfinite(mag))
            return std::numeric_limits<double>::quiet_NaN();
        double w = k == 0 ? w_lo : std::max(w_lo, w_hi);
        return 20.0 * std::log10(mag) + k * 20.0 * std::log10(w);
    };

    std::vector<Entry> entries;
    for (int k = 0; k <= deg; ++k) {
        ex c = pe.coeff(s, k);
        if (c.is_zero()) continue;
        std::vector<ex> terms;
        if (is_a<GiNaC::add>(c))
            for (size_t i = 0; i < c.nops(); ++i) terms.push_back(c.op(i));
        else
            terms.push_back(c);
        for (const ex& t : terms)
            entries.push_back({k, t, term_db(t, k)});
    }
    if (entries.empty()) return;

    auto finite_max = [&](const std::vector<Entry>& es) {
        double m = -std::numeric_limits<double>::infinity();
        for (const auto& e : es)
            if (!std::isnan(e.db)) m = std::max(m, e.db);
        return m;
    };

    std::vector<char> keep(entries.size(), 1);
    std::vector<double> refs(entries.size(), 0.0);
    // "At or beyond the threshold" (a 1000x frequency ratio == 60 dB) is
    // dropped, so a pole at 100 MHz is ignored against a 100 kHz pole while a
    // 10 MHz pole (100x, 40 dB) is kept. The +1e-9 tolerates the floating-point
    // error at an exact power-of-ten boundary.
    const double eps = 1e-9;
    if (o.global_ref) {
        double ref = finite_max(entries);
        for (size_t i = 0; i < entries.size(); ++i) {
            refs[i] = ref;
            if (std::isnan(entries[i].db)) continue;
            if (entries[i].db <= ref - o.pole_zero_threshold_db + eps) keep[i] = 0;
        }
    } else {
        for (size_t i = 0; i < entries.size(); ++i) {
            std::vector<Entry> group;
            for (const auto& e : entries)
                if (e.k == entries[i].k) group.push_back(e);
            double ref = finite_max(group);
            refs[i] = ref;
            if (std::isnan(entries[i].db)) continue;
            if (entries[i].db <= ref - o.pole_zero_threshold_db + eps) keep[i] = 0;
        }
    }
    bool any = false;
    for (char c : keep) any = any || (c != 0);
    if (!any) for (auto& c : keep) c = 1;

    ex acc = 0;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (!keep[i]) {
            std::string loc = what + ", ";
            loc += entries[i].k == 0 ? "constant"
                   : entries[i].k == 1 ? "s"
                                       : "s^" + std::to_string(entries[i].k);
            double rel = entries[i].db - refs[i];
            dropped.push_back({loc, pretty(entries[i].term),
                               std::isnan(rel) ? 0.0 : rel});
            continue;
        }
        acc += entries[i].term * GiNaC::pow(s, entries[i].k);
    }
    poly = acc;
}

// ---------------------------------------------------------------------------
// time-constant candidates for factor matching (TTC style)
// ---------------------------------------------------------------------------
struct Candidate {
    double value;
    ex expr;
};

std::vector<Candidate> build_tau_candidates(const ParamTable& pt) {
    struct Sym { double v; ex e; };
    std::vector<Sym> rs, cs, ls, gs;
    // Amplifier poles: an op-amp's dominant pole sits at w0 = 2*pi*GBW/A, so
    // its time constant is A/(2*pi*GBW). Collected by symbol-name prefix (the
    // A_* / GBW_* variables MNA registers for the amplifier-like blocks).
    std::vector<Sym> amps, gbws;
    for (const auto& kv : pt.est) {
        auto sit = pt.syms.find(kv.first);
        if (sit == pt.syms.end()) continue;
        if (kv.second <= 0.0 || !std::isfinite(kv.second)) continue;
        const std::string& name = kv.first;
        if (name.rfind("A_", 0) == 0) amps.push_back({kv.second, sit->second});
        else if (name.rfind("GBW_", 0) == 0)
            gbws.push_back({kv.second, sit->second});
        auto cit = pt.cls.find(kv.first);
        if (cit == pt.cls.end()) continue;
        ex e = sit->second;
        switch (cit->second) {
            case UnitClass::Ohm: rs.push_back({kv.second, e}); break;
            case UnitClass::Siemens: gs.push_back({kv.second, e}); break;
            case UnitClass::Farad: cs.push_back({kv.second, e}); break;
            case UnitClass::Henry: ls.push_back({kv.second, e}); break;
            default: break;
        }
    }
    std::vector<Candidate> out;
    auto add = [&](double v, const ex& e) {
        if (out.size() > 8192) return;
        if (v > 0.0 && std::isfinite(v)) out.push_back({v, e});
    };
    for (const auto& r : rs)
        for (const auto& c : cs) add(r.v * c.v, r.e * c.e);
    for (const auto& l : ls)
        for (const auto& r : rs) add(l.v / r.v, l.e / r.e);
    for (const auto& c : cs)
        for (const auto& g : gs) add(c.v / g.v, c.e / g.e);
    // op-amp dominant pole: GBW_<ref> is registered in rad/s (the estimate is
    // the user's Hz value times 2*pi), so tau = A/GBW. The closed-loop
    // counterpart tau = 1/GBW applies when the loop crosses over at GBW.
    for (const auto& a : amps)
        for (const auto& g : gbws) {
            add(a.v / g.v, a.e / g.e);
            add(1.0 / g.v, 1 / g.e);
        }
    // parallel resistor pairs: (R1||R2)*C  -- the signature of a pole
    for (size_t i = 0; i < rs.size(); ++i)
        for (size_t j = i + 1; j < rs.size(); ++j) {
            double rp = rs[i].v * rs[j].v / (rs[i].v + rs[j].v);
            ex rex = par_ex(rs[i].e, rs[j].e);
            for (const auto& c : cs) add(rp * c.v, rex * c.e);
        }
    // sums of two capacitors sharing one effective resistance -- the signature
    // of a Miller / output pole: R*(Cgd + CL), (Rd||ro)*(Cgd + Cgs), ...
    for (size_t i = 0; i < rs.size(); ++i) {
        for (size_t a = 0; a < cs.size(); ++a)
            for (size_t b = a + 1; b < cs.size(); ++b)
                add(rs[i].v * (cs[a].v + cs[b].v),
                    rs[i].e * (cs[a].e + cs[b].e));
    }
    for (size_t i = 0; i < rs.size(); ++i)
        for (size_t j = i + 1; j < rs.size(); ++j) {
            double rp = rs[i].v * rs[j].v / (rs[i].v + rs[j].v);
            ex rex = par_ex(rs[i].e, rs[j].e);
            for (size_t a = 0; a < cs.size(); ++a)
                for (size_t b = a + 1; b < cs.size(); ++b)
                    add(rp * (cs[a].v + cs[b].v),
                        rex * (cs[a].e + cs[b].e));
        }
    return out;
}

// ---------------------------------------------------------------------------
// approximate (numeric) factoring
// ---------------------------------------------------------------------------
// Exact symbolic factoring of a real MNA denominator is frequently impossible
// (the poles are not products of a single R and a single C). This helper
// estimates every coefficient numerically, roots the resulting real polynomial
// with Durand-Kerner, then rebuilds real first-order factors -- matching each
// root to a physically meaningful time constant when one is numerically close,
// and falling back to the numeric 1/|w| otherwise. Complex-conjugate pairs
// become second-order factors (1 + s/(Q*w) + s^2/w^2). The reconstruction is
// verified numerically before the factors are accepted, so a failure leaves
// the exact polynomial untouched.
bool approx_factor(ex& poly, ParamTable& pt, const ex& s,
                   const std::vector<Candidate>& cands,
                   std::vector<Factor>& factors, bool& any_numeric) {
    int deg = 0;
    try {
        if (!poly.is_polynomial(s)) return false;
        deg = poly.has(s) ? poly.degree(s) : 0;
    } catch (...) {
        return false;
    }
    if (deg < 2 || deg > 16) return false;

    std::vector<double> coeffs;
    for (int k = 0; k <= deg; ++k) {
        ex v = pt.eval_real(poly.coeff(s, k)).evalf();
        if (!is_a<numeric>(v)) return false;
        double d = GiNaC::ex_to<numeric>(v).to_double();
        if (!std::isfinite(d)) return false;
        coeffs.push_back(d);
    }
    if (coeffs[deg] == 0.0 || coeffs[0] == 0.0) return false;

    std::vector<std::complex<double>> roots = poly_roots(coeffs);
    if (int(roots.size()) != deg) return false;

    // 1. numeric factors (used for the verification)
    struct NumFac { bool complex_pair; double a, b; };
    std::vector<NumFac> nf;
    std::vector<char> used(roots.size(), 0);
    // The factors are normalized to a constant term of 1; the polynomial's own
    // constant term (which may not be 1 when it could not be normalized) is
    // carried out as a leading numeric constant.
    double lead_const = coeffs[0];
    if (!(lead_const > 0.0) || !std::isfinite(lead_const)) return false;
    for (size_t i = 0; i < roots.size(); ++i) {
        if (used[i]) continue;
        double re = roots[i].real(), im = roots[i].imag();
        if (std::fabs(im) > 1e-6 * (1.0 + std::fabs(re))) continue;
        used[i] = 1;
        double tau = -1.0 / re;
        if (!(tau > 0.0) || !std::isfinite(tau)) return false;
        nf.push_back({false, tau, 0.0});
    }
    for (size_t i = 0; i < roots.size(); ++i) {
        if (used[i]) continue;
        if (roots[i].imag() < 0.0) continue;
        used[i] = 1;
        double re = roots[i].real(), im = roots[i].imag();
        double wn = std::hypot(re, im);
        double q = (im > 0.0) ? wn / (2.0 * im) : 1e30;
        if (!(wn > 0.0) || !std::isfinite(q)) return false;
        nf.push_back({true, 1.0 / (q * wn), 1.0 / (wn * wn)});
    }

    // 2. verify the numeric reconstruction against the (numeric) polynomial
    {
        std::vector<double> recon(coeffs.size(), 0.0);
        recon[0] = lead_const;
        int rdeg = 0;
        for (const auto& f : nf) {
            std::vector<double> nxt(coeffs.size(), 0.0);
            int nd = rdeg;
            for (int k = 0; k <= rdeg; ++k) {
                nxt[k] += recon[k];                    // * 1
                nxt[k + 1] += recon[k] * f.a;          // * a*s
                if (f.complex_pair) nxt[k + 2] += recon[k] * f.b; // * b*s^2
            }
            nd += f.complex_pair ? 2 : 1;
            recon = nxt;
            rdeg = nd;
        }
        for (int k = 0; k <= deg; ++k) {
            double tol = 1e-6 * std::fabs(coeffs[k]) + 1e-300;
            if (std::fabs(recon[k] - coeffs[k]) > tol) return false;
        }
    }

    // 3. display factors: swap a numeric tau for a matched time constant
    auto close_candidate = [&](double tau) -> const Candidate* {
        if (!(tau > 0.0) || !std::isfinite(tau)) return nullptr;
        const Candidate* best = nullptr;
        double bestd = std::log(1.02); // accept a ~2% numeric match
        for (const auto& c : cands) {
            if (!(c.value > 0.0)) continue;
            double d = std::fabs(std::log(c.value / tau));
            if (d < bestd) { bestd = d; best = &c; }
        }
        return best;
    };
    // the residual constant term (1 when the polynomial was normalized)
    if (std::fabs(lead_const - 1.0) > 1e-12) {
        ex cf = GiNaC::numeric(lead_const);
        factors.push_back({cf, pretty(cf), false});
    }
    for (const auto& f : nf) {
        ex fe;
        if (!f.complex_pair) {
            double tau = f.a;
            const Candidate* c = close_candidate(tau);
            if (c) {
                fe = ex(1) + s * c->expr;
            } else {
                fe = ex(1) + s * GiNaC::numeric(tau);
            }
        } else {
            fe = ex(1) + s * GiNaC::numeric(f.a) + GiNaC::pow(s, 2) *
                 GiNaC::numeric(f.b);
        }
        factors.push_back({fe, pretty_in_s(fe, s), false});
    }
    poly = ex(1);
    any_numeric = true; // approximate by construction, even when the time
                        // constant matched a named candidate
    return true;
}

// ---------------------------------------------------------------------------
// symbolic factor peeling
// ---------------------------------------------------------------------------
void peel_factors(ex& poly, const std::vector<Candidate>& cands,
                  ParamTable& pt, const ex& s, std::vector<Factor>& factors,
                  bool approx_ok, bool& any_numeric) {
    while (true) {
        int deg;
        try {
            deg = poly.degree(s);
        } catch (...) {
            return;
        }
        if (deg <= 0) return;
        if (deg == 1) {
            factors.push_back({poly, pretty_in_s(poly, s), false});
            poly = ex(1);
            return;
        }
        // numeric roots for candidate ordering
        std::vector<double> coeffs;
        bool all_real = true;
        for (int k = 0; k <= deg; ++k) {
            ex v = pt.eval_real(poly.coeff(s, k));
            if (is_a<numeric>(v))
                coeffs.push_back(GiNaC::ex_to<numeric>(v).to_double());
            else
                all_real = false;
        }
        std::vector<std::complex<double>> roots =
            all_real ? poly_roots(coeffs) : std::vector<std::complex<double>>{};

        struct Ranked { double score; const Candidate* c; };
        std::vector<Ranked> ranked;
        for (const auto& cand : cands) {
            double best = std::numeric_limits<double>::infinity();
            for (const auto& r : roots) {
                if (std::abs(r.imag()) > 1e-6 * (1.0 + std::abs(r.real()))) continue;
                double target = -1.0 / r.real();
                if (target <= 0.0 || cand.value <= 0.0) continue;
                best = std::min(best, std::fabs(std::log(cand.value / target)));
            }
            if (best <= std::log(1.15)) ranked.push_back({best, &cand});
        }
        std::sort(ranked.begin(), ranked.end(),
                  [](const Ranked& a, const Ranked& b) { return a.score < b.score; });

        bool peeled = false;
        size_t tried = 0;
        for (const Ranked& rk : ranked) {
            if (++tried > 8 && rk.score > std::log(1.02)) break;
            ex f = ex(1) + rk.c->expr * s;
            // Exact symbolic division only: the factor must actually divide.
            if (!poly_remainder_is_zero(poly, f, s)) continue;
            ex q = (poly / f).normal();
            if (q.denom().has(s)) continue; // not a polynomial quotient
            factors.push_back({f, pretty_in_s(f, s), false});
            poly = q.expand();
            peeled = true;
            break;
        }
        if (peeled) continue;
        // No exact factor: attempt approximate (numeric) factoring so a
        // multi-pole denominator still comes out as (1+s*tau1)(1+s*tau2).
        if (approx_ok) {
            int before = int(factors.size());
            if (approx_factor(poly, pt, s, cands, factors, any_numeric))
                continue;
            factors.resize(before);
        }
        // Not factorable: keep the whole polynomial as one factor so the
        // printed denominator is never a fabricated product.
        factors.push_back({poly, pretty_in_s(poly, s), false});
        poly = ex(1);
        return;
    }
}

void roots_from_factors(const std::vector<Factor>& factors, ParamTable& pt,
                        const ex& s, std::vector<Root>& out) {
    for (const auto& f : factors) {
        if (f.origin) {
            Root r;
            r.label = "origin";
            r.factor = f.text;
            r.latex_factor = to_latex_cdot(f.expr);
            r.latex_label = "s";
            r.factor_expr = f.expr;
            r.omega_expr = 0;
            out.push_back(r);
            continue;
        }
        int deg = 0;
        try { deg = f.expr.degree(s); } catch (...) { continue; }
        if (deg == 1) {
            ex c1 = f.expr.coeff(s, 1);
            ex c0 = f.expr.coeff(s, 0);
            ex tau_ex = (c1 / c0).normal();
            ex tau_n = pt.eval_real(tau_ex);
            Root r;
            r.factor = f.text;
            r.latex_factor = to_latex_cdot(f.expr);
            r.factor_expr = f.expr;
            if (is_a<numeric>(tau_n)) r.tau = GiNaC::ex_to<numeric>(tau_n).to_double();
            if (r.tau != 0.0) {
                r.omega = 1.0 / r.tau;
                r.f_hz = r.omega / (2.0 * M_PI);
            }
            r.omega_expr = (c0 / c1).normal();
            if (is_a<GiNaC::mul>(tau_ex) || is_a<GiNaC::add>(tau_ex) ||
                is_parallel(tau_ex)) {
                // factor common terms so a pole reads as "(Rd||ro)*(Cgd+CL)"
                ex lab = factor_common_impl(tau_ex);
                r.label = pretty(lab);
                r.latex_label = to_latex_cdot(lab);
            }
            out.push_back(r);
        } else if (deg >= 2) {
            std::vector<double> coeffs;
            bool ok = true;
            for (int k = 0; k <= deg; ++k) {
                ex v = pt.eval_real(f.expr.coeff(s, k));
                if (!is_a<numeric>(v)) { ok = false; break; }
                coeffs.push_back(GiNaC::ex_to<numeric>(v).to_double());
            }
            if (!ok) continue;
            for (const auto& z : poly_roots(coeffs)) {
                Root r;
                r.factor = f.text;
                r.latex_factor = to_latex_cdot(f.expr);
                r.factor_expr = f.expr;
                if (std::abs(z.imag()) < 1e-6 * (1.0 + std::abs(z.real()))) {
                    r.real = true;
                    r.tau = -1.0 / z.real();
                    r.omega = r.tau != 0.0 ? 1.0 / r.tau : 0.0;
                    r.f_hz = r.omega / (2.0 * M_PI);
                } else {
                    r.real = false;
                    r.omega = std::abs(z);
                    r.f_hz = r.omega / (2.0 * M_PI);
                    r.q = std::abs(z) / (2.0 * std::abs(z.imag()));
                }
                out.push_back(r);
            }
        }
    }
}

// Drop poles/zeros that lie more than threshold_db (60 dB) along the frequency
// axis from the dominant (lowest-frequency) one. A 100 kHz pole dominates a
// 100 MHz pole (1000x, 60 dB) so the latter is dropped; a 10 MHz pole (100x,
// 40 dB) is kept. Operates on the already-factored (1 + s*tau) factors, so the
// reduction is done per time constant -- never by chopping terms out of the
// expanded polynomial (which would leave an inconsistent, mis-factored form).
void drop_far_factors(std::vector<Factor>& factors, ParamTable& pt,
                      const ex& s, double threshold_db, const std::string& where,
                      std::vector<Dropped>& dropped) {
    if (factors.size() <= 1) return;
    std::vector<double> tau(factors.size(), 0.0);
    double max_tau = 0.0;
    for (size_t i = 0; i < factors.size(); ++i) {
        if (factors[i].origin) continue;
        int deg = 0;
        try { deg = factors[i].expr.degree(s); } catch (...) { continue; }
        if (deg != 1) continue;
        ex c1 = factors[i].expr.coeff(s, 1);
        ex c0 = factors[i].expr.coeff(s, 0);
        if (c0.is_zero()) continue;
        ex te = (c1 / c0).normal();
        ex tv = pt.eval_real(te);
        if (is_a<numeric>(tv)) {
            double v = GiNaC::ex_to<numeric>(tv).to_double();
            if (v > 0.0 && std::isfinite(v)) {
                tau[i] = v;
                max_tau = std::max(max_tau, v);
            }
        }
    }
    if (max_tau <= 0.0) return;
    double lim = std::pow(10.0, threshold_db / 20.0);
    std::vector<Factor> kept;
    for (size_t i = 0; i < factors.size(); ++i) {
        // "At or beyond" the threshold is dropped (60 dB == 1000x), so a
        // 100 MHz pole is ignored against a 100 kHz pole, while a 10 MHz pole
        // (100x, 40 dB) is kept. The *1.000001 tolerates floating-point error
        // at an exact power-of-ten ratio.
        bool far = tau[i] > 0.0 && tau[i] <= max_tau / lim * 1.000001;
        if (far) {
            dropped.push_back({where + ", factor", factors[i].text,
                               20.0 * std::log10(tau[i] / max_tau)});
        } else {
            kept.push_back(factors[i]);
        }
    }
    factors = kept;
}

// Magnitude helper: |e| at s = 0 with the user's parameter estimates, with a
// floor to keep log10 sane when a summand has zero numeric value (a bare
// parameter or a term that simplifies to 0).
static double num_mag(const ex& e, const ParamTable& pt) {
    std::complex<double> z = eval_complex(e, pt, 0.0);
    double mag = std::hypot(z.real(), z.imag());
    return mag > 0.0 ? mag : 1e-30;
}

// Pull a common factor out of a sum of products: a*x + a*y -> a*(x+y).
// Used to make pole labels readable ("(Rd||ro)*(Cgd+CL)").
// Only a *polynomial* common factor is pulled out: pulling out a factor that
// itself sits in a denominator would turn the sum into (a/x + b) inside a
// product, which is not a valid factor and prints as garbage.
ex factor_common_impl(const ex& e) {
    if (!is_a<GiNaC::add>(e)) return e;
    // collect the multiplicative factor lists of each term
    std::vector<std::vector<ex>> terms;
    for (size_t i = 0; i < e.nops(); ++i) {
        std::vector<ex> fs;
        const ex& t = e.op(i);
        if (is_a<GiNaC::mul>(t)) {
            for (size_t j = 0; j < t.nops(); ++j) fs.push_back(t.op(j));
        } else {
            fs.push_back(t);
        }
        terms.push_back(fs);
    }
    if (terms.empty()) return e;
    // common factors: present in every term (compare structurally). Only
    // polynomial factors (non-negative powers) are extracted -- pulling out a
    // factor that lives in a denominator would turn the sum into a nested
    // fraction such as (a/x + b)*x, which is not low-entropy and reads badly.
    auto is_polynomial_factor = [](const ex& f) {
        if (is_a<GiNaC::power>(f)) {
            const ex& xp = f.op(1);
            if (is_a<numeric>(xp) && GiNaC::ex_to<numeric>(xp).is_negative())
                return false;
        }
        return true;
    };
    std::vector<ex> common;
    for (const ex& f : terms[0]) {
        if (!is_polynomial_factor(f)) continue;
        bool inall = true;
        for (size_t i = 1; i < terms.size() && inall; ++i) {
            bool found = false;
            for (const ex& g : terms[i])
                if (g.is_equal(f)) { found = true; break; }
            if (!found) inall = false;
        }
        if (inall) common.push_back(f);
    }
    if (common.empty()) return e;
    // divide each term by the common part
    ex cpart = ex(1);
    for (const ex& f : common) cpart = cpart * f;
    ex rest = (e / cpart).normal();
    if (!is_a<GiNaC::add>(rest)) return e;
    return cpart * rest;
}

// Factor each s-coefficient of a polynomial separately. factor_common_impl
// only pulls a factor common to *every* term of the whole polynomial, so for
// an impedance whose constant term is a sum (e.g. R1 + ro + s*R1*ro*Cds +
// s*C1*R1*ro) it leaves the s^1 coefficient un-factored. Factoring per power
// of s turns that into R1 + ro + s*R1*ro*(Cds + C1), which is the compact
// form the pole/time-constant labels also use.
ex factor_coeffs(const ex& poly, const ex& s) {
    int deg = 0;
    try { deg = poly.has(s) ? poly.degree(s) : 0; } catch (...) { return poly; }
    if (deg < 0 || deg > 64) return poly;
    ex acc = 0;
    for (int k = 0; k <= deg; ++k) {
        ex c = poly.coeff(s, k);
        if (c.is_zero()) continue;
        ex fc = factor_common_impl(c);
        acc += (fc.is_zero() ? c : fc) * GiNaC::pow(s, k);
    }
    return acc;
}

// Collapse a parallel combination to its dominant argument when the other is
// negligible. par(a,b) = a*b/(a+b); if b >> a then par -> a (the smaller
// resistance wins in parallel -- the big resistor is negligible), and if
// a >> b then par -> b. This is the "ignore negligible for parallel terms"
// rule the user asked for, and the capacitor dual falls out naturally: a
// parallel combination of *capacitances* never appears as par() (they sum),
// while a series combination of resistors never appears as par() either, so
// this only ever fires on genuine parallel resistor pairs.
ex prune_parallel(const ex& e, const ParamTable& pt, double threshold_db) {
    if (is_parallel(e)) {
        std::vector<ex> args = parallel_args(e);
        if (args.size() == 2) {
            double ma = num_mag(args[0], pt);
            double mb = num_mag(args[1], pt);
            double lim = std::pow(10.0, threshold_db / 20.0);
            if (ma > 0.0 && mb > 0.0) {
                // par(a,b) = a*b/(a+b): the SMALLER resistance carries the
                // current, so the bigger one is negligible. When b >> a the
                // combination tends to a; when a >> b it tends to b.
                if (mb > ma * lim)
                    return prune_parallel(args[0], pt, threshold_db);
                if (ma > mb * lim)
                    return prune_parallel(args[1], pt, threshold_db);
            }
        }
        GiNaC::exvector a2;
        for (const auto& a : args)
            a2.push_back(prune_parallel(a, pt, threshold_db));
        return make_parallel(a2);
    }
    if (is_a<GiNaC::add>(e) || is_a<GiNaC::mul>(e)) {
        GiNaC::exvector ops;
        for (size_t i = 0; i < e.nops(); ++i)
            ops.push_back(prune_parallel(e.op(i), pt, threshold_db));
        if (is_a<GiNaC::add>(e)) return ex(GiNaC::add(ops));
        return ex(GiNaC::mul(ops));
    }
    if (is_a<GiNaC::power>(e)) {
        return GiNaC::pow(prune_parallel(e.op(0), pt, threshold_db), e.op(1));
    }
    return e;
}

// Does an expression contain a negative (inverse) power, e.g. R1^(-1) or
// (R1+ro)^(-1)? An inverse of an Ohm expression is a *conductance*, not a
// resistance, so it must never be mistaken for a series resistor.
static bool has_negative_power(const ex& e) {
    if (is_a<GiNaC::power>(e)) {
        if (is_a<numeric>(e.op(1)) && GiNaC::ex_to<numeric>(e.op(1)).is_negative())
            return true;
        return has_negative_power(e.op(0));
    }
    if (is_a<GiNaC::add>(e) || is_a<GiNaC::mul>(e)) {
        for (size_t i = 0; i < e.nops(); ++i)
            if (has_negative_power(e.op(i))) return true;
    }
    return false;
}

// Classify a sum whose every term is a single passive element of the SAME
// physical class: R1 + R2 (all Ohm), C1 + C2 (all Farad), L1 + L2 (all Henry).
// Such a sum is either a series combination (R, L) or a parallel combination
// (C), always with the "keep the largest term" semantics. A mixed sum, or one
// containing a conductance 1/R, is not uniform. Returns false when not uniform.
static bool uniform_passive_sum(const ex& e, const ParamTable& pt,
                                UnitClass& cls) {
    if (!is_a<GiNaC::add>(e)) return false;
    cls = UnitClass::Plain;
    for (size_t i = 0; i < e.nops(); ++i) {
        const ex& t = e.op(i);
        if (has_negative_power(t)) return false;
        // the term's class: which unit-class symbol does it contain?
        UnitClass tc = UnitClass::Plain;
        int seen = 0;
        for (const auto& kv : pt.cls) {
            auto sit = pt.syms.find(kv.first);
            if (sit == pt.syms.end()) continue;
            if (!t.has(sit->second)) continue;
            if (kv.second != UnitClass::Ohm && kv.second != UnitClass::Farad &&
                kv.second != UnitClass::Henry)
                return false; // a gm / dimensionless symbol: not a passive sum
            tc = kv.second;
            ++seen;
        }
        // Exactly one passive symbol: a bare element (R1, C3, L2). A product
        // like C1*C2 has units F^2 and is NOT a capacitor, so it must not be
        // treated as one here (that magnitude pruning handles it, and records
        // it in `dropped`).
        if (seen != 1) return false;
        if (i == 0) cls = tc;
        else if (tc != cls) return false; // mixed classes
    }
    return cls != UnitClass::Plain;
}

// The series/parallel "keep the largest" rule, for a uniform passive sum.
// R1 + R2 -> R1 (series R), C1 + C2 -> C1 (parallel C), L1 + L2 -> L1 (series
// L): in all three the larger term dominates and a term more than threshold_db
// below it falls away.
static ex drop_small_terms(const ex& sum, const ParamTable& pt,
                           double threshold_db) {
    double lim = std::pow(10.0, threshold_db / 20.0);
    double best = -1e300;
    for (size_t i = 0; i < sum.nops(); ++i)
        best = std::max(best, num_mag(sum.op(i), pt));
    ex acc = 0;
    for (size_t i = 0; i < sum.nops(); ++i)
        if (num_mag(sum.op(i), pt) >= best / lim) acc += sum.op(i);
    return acc.is_zero() ? sum : acc;
}

// Reduce a *held* series atom ser(a,b) with the 20 dB "keep the largest" rule:
// ser(R1,R2) -> R1 when R2 is more than threshold_db below R1. Returns true and
// sets `out` when the atom changed. A held series group is the structural form
// the MNA fold produces, so this is where the "ignore negligible" series rule
// is applied to it (the expanded-sum path below handles the non-held case).
static bool reduce_held_series(const ex& e, const ParamTable& pt,
                              double threshold_db, ex& out) {
    if (!is_series(e)) return false;
    std::vector<ex> args = series_args(e);
    if (args.size() < 2) return false;
    double lim = std::pow(10.0, threshold_db / 20.0);
    double best = -1e300;
    for (const ex& a : args) best = std::max(best, num_mag(a, pt));
    std::vector<ex> keep;
    for (const ex& a : args)
        if (num_mag(a, pt) >= best / lim) keep.push_back(a);
    if (keep.size() == args.size()) return false;   // nothing dropped
    out = keep.empty() ? e : make_series(keep);
    return true;
}

// Collapse a series combination of resistors to the dominant one (20 dB):
// R1 + R2 -> R1 when R2 << R1, and R1*C + R2*C -> R1*C. This is the series
// dual of prune_parallel, and it runs at the same (20 dB) structural threshold
// -- distinct from the 60 dB pole/zero reduction applied after factoring.
ex prune_series(const ex& e, const ParamTable& pt, double threshold_db) {
    // A held series atom (from the structural MNA fold): reduce its operands
    // first, then apply the "keep the largest" rule to the held sum itself.
    if (is_series(e)) {
        std::vector<ex> args = series_args(e);
        std::vector<ex> rargs;
        for (const ex& a : args) rargs.push_back(prune_series(a, pt, threshold_db));
        ex built = make_series(rargs);
        ex out;
        if (reduce_held_series(built, pt, threshold_db, out)) return out;
        return built;
    }
    // Recurse into a held parallel atom's operands so a ser() nested inside it
    // is reduced too, then rebuild the atom.
    if (is_parallel(e)) {
        std::vector<ex> args = parallel_args(e);
        if (args.size() != 2) return e;
        ex a = prune_series(args[0], pt, threshold_db);
        ex b = prune_series(args[1], pt, threshold_db);
        return make_parallel({a, b});
    }
    if (is_a<GiNaC::add>(e)) {
        GiNaC::exvector ops;
        for (size_t i = 0; i < e.nops(); ++i)
            ops.push_back(prune_series(e.op(i), pt, threshold_db));
        ex sum = GiNaC::add(ops);
        if (!is_a<GiNaC::add>(sum)) return sum;
        UnitClass cls;
        if (uniform_passive_sum(sum, pt, cls))
            return drop_small_terms(sum, pt, threshold_db);
        // R1*C + R2*C factors to C*(R1 + R2) (or (R1+R2)*C -- GiNaC's operand
        // order depends on symbol serial numbers, so scan for the residual add
        // rather than assuming it is the last factor). Reduce the residual sum.
        ex fc = factor_common_impl(sum);
        if (is_a<GiNaC::mul>(fc) && fc.nops() >= 2) {
            ex inner = 0, common = 1;
            for (size_t i = 0; i < fc.nops(); ++i) {
                if (is_a<GiNaC::add>(fc.op(i))) inner = fc.op(i);
                else common = common * fc.op(i);
            }
            UnitClass icls;
            if (!inner.is_zero() && uniform_passive_sum(inner, pt, icls)) {
                ex reduced = drop_small_terms(inner, pt, threshold_db);
                if (!reduced.is_equal(inner))
                    return common * reduced;
            }
        }
        return sum;
    }
    if (is_a<GiNaC::mul>(e)) {
        GiNaC::exvector ops;
        for (size_t i = 0; i < e.nops(); ++i)
            ops.push_back(prune_series(e.op(i), pt, threshold_db));
        return ex(GiNaC::mul(ops));
    }
    // NOTE: do NOT recurse into power(). A negative power like (R1+ro)^(-1) is
    // the denominator of a *parallel* combination R1||ro = R1*ro/(R1+ro); the
    // sum R1+ro there must stay intact (collapsing it to ro would corrupt the
    // parallel resistance). Only genuine series sums -- R1 + R2 at the top of a
    // coefficient, or R1*C + R2*C -- are series-reduced.
    return e;
}

// Reduce a gain expression by dropping negligible terms inside its sums, using
// the same 20 dB structural rule as the series/parallel pass. A/(A+1) with
// A = 1e9 collapses to 1 (the "+1" is 180 dB down); with A = 1 it stays.
ex simplify_gain(const ex& e, const ParamTable& pt, double threshold_db) {
    if (is_a<GiNaC::add>(e)) {
        GiNaC::exvector ops;
        for (size_t i = 0; i < e.nops(); ++i)
            ops.push_back(simplify_gain(e.op(i), pt, threshold_db));
        ex sum = GiNaC::add(ops);
        if (!is_a<GiNaC::add>(sum)) return sum;
        double best = -1e300;
        for (size_t i = 0; i < sum.nops(); ++i)
            best = std::max(best, num_mag(sum.op(i), pt));
        double lim = std::pow(10.0, threshold_db / 20.0);
        ex acc = 0;
        for (size_t i = 0; i < sum.nops(); ++i)
            if (num_mag(sum.op(i), pt) >= best / lim) acc += sum.op(i);
        return acc.is_zero() ? sum : acc;
    }
    if (is_a<GiNaC::mul>(e)) {
        GiNaC::exvector ops;
        for (size_t i = 0; i < e.nops(); ++i)
            ops.push_back(simplify_gain(e.op(i), pt, threshold_db));
        return ex(GiNaC::mul(ops));
    }
    if (is_a<GiNaC::power>(e)) {
        // A negative power is a denominator: simplify the base, then try the
        // ratio rule against a numerator that may sit outside this node.
        ex base = simplify_gain(e.op(0), pt, threshold_db);
        return GiNaC::pow(base, e.op(1));
    }
    return e;
}

// Reduce a coefficient all the way: recurse the structural rules, then let
// GiNaC cancel the result. This is what turns A/(GBW*(1+A)) into 1/GBW when A
// is large: the (1+A) inside the denominator reduces to A, so the A cancels.
ex simplify_coeff(const ex& e, const ParamTable& pt, double threshold_db) {
    return simplify_gain(e, pt, threshold_db).normal();
}

// Wrap a factor's text in parentheses when it is a sum or a ratio, so a
// product of factors prints unambiguously: "(a+b)" not "a+b". The text path
// uses an ASCII `*` (see core/Print.cpp); `pruned.latex` is built separately
// with real LaTeX.
std::string paren_factor(const std::string& t) {
    if (t == "1") return t;
    bool sum = t.find('+') != std::string::npos ||
               t.find(" - ") != std::string::npos;
    bool ratio = t.find('/') != std::string::npos;
    bool product = t.find('*') != std::string::npos;
    if (sum || ratio || product) return "(" + t + ")";
    return t;
}

// Already enclosed by one matching outer pair of parentheses?
bool already_wrapped(const std::string& t) {
    if (t.size() < 2 || t.front() != '(' || t.back() != ')') return false;
    int depth = 0;
    for (size_t i = 0; i < t.size(); ++i) {
        if (t[i] == '(') ++depth;
        else if (t[i] == ')') {
            --depth;
            if (depth == 0) return i == t.size() - 1;
        }
    }
    return false;
}

std::string join_factors_pretty(const std::vector<Factor>& fs) {
    std::string out;
    for (const auto& f : fs) {
        if (f.text == "1" && !out.empty()) continue;
        std::string t = f.text;
        // the origin factor s^k needs no parentheses
        if (!f.origin && !already_wrapped(t)) t = paren_factor(t);
        if (out.empty())
            out = t;
        else
            out += "*" + t;
    }
    return out.empty() ? "1" : out;
}

std::string wrap_compound(const std::string& t) {
    if (t == "1") return t;
    if (already_wrapped(t)) return t;
    bool compound = t.find('*') != std::string::npos ||
                    t.find('+') != std::string::npos ||
                    t.find('-') != std::string::npos ||
                    t.find('/') != std::string::npos ||
                    t.find('|') != std::string::npos;
    return compound ? "(" + t + ")" : t;
}

} // namespace

// ---------------------------------------------------------------------------
// LaTeX
// ---------------------------------------------------------------------------
std::string to_latex(const ex& e) {
    std::ostringstream os;
    e.print(GiNaC::print_latex(os));
    return os.str();
}

namespace {
// LaTeX printer that inserts an explicit `\cdot` between every multiplied
// factor, so `Cds_M1 ro_M1` (juxtaposition) becomes `Cds_M1\cdot ro_M1` and
// never reads as a single term. Fractions and everything GiNaC already
// handles well (subscripts, \left..\right, \frac, \parallel) are delegated to
// GiNaC's own print_latex; we only special-case the plain product / sum
// nodes. A `mul` that contains a negative-power factor (a ratio) is left to
// print_latex so it renders as \frac{..}{..} rather than a\cdot b^{-1}.
std::string to_latex_cdot(const ex& e) {
    if (is_a<GiNaC::mul>(e)) {
        bool has_denominator = false;
        for (size_t i = 0; i < e.nops(); ++i) {
            const ex& op = e.op(i);
            if (is_a<GiNaC::power>(op)) {
                const ex& xp = op.op(1);
                if (is_a<numeric>(xp) &&
                    GiNaC::ex_to<numeric>(xp).is_negative())
                    has_denominator = true;
            }
        }
        if (has_denominator) {
            std::ostringstream os;
            e.print(GiNaC::print_latex(os));
            return os.str();
        }
        // Separate the numeric coefficient from the symbolic factors.
        ex coeff = 1;
        std::vector<ex> sym;
        for (size_t i = 0; i < e.nops(); ++i) {
            const ex& op = e.op(i);
            if (is_a<numeric>(op)) coeff = coeff * op;
            else sym.push_back(op);
        }
        bool neg = false;
        if (is_a<numeric>(coeff)) {
            numeric nc = GiNaC::ex_to<numeric>(coeff);
            if (nc.is_negative()) { neg = true; coeff = -coeff; }
        }
        std::string body;
        if (!coeff.is_equal(ex(1))) body += to_latex_cdot(coeff);
        for (size_t i = 0; i < sym.size(); ++i) {
            if (i > 0 || !body.empty()) body += "\\cdot ";
            body += to_latex_cdot(sym[i]);
        }
        return (neg ? "-" : "") + body;
    }
    if (is_a<GiNaC::add>(e)) {
        std::string out;
        for (size_t i = 0; i < e.nops(); ++i) {
            std::string t = to_latex_cdot(e.op(i));
            bool tneg = !t.empty() && t[0] == '-';
            if (tneg) t = t.substr(1);
            if (i == 0) out += (tneg ? "-" : "") + t;
            else out += (tneg ? " - " : " + ") + t;
        }
        return out;
    }
    std::ostringstream os;
    e.print(GiNaC::print_latex(os));
    return os.str();
}
} // namespace

// The right-hand side of the low-entropy LaTeX (no "H(s) = " prefix), so
// callers can attach it to whatever left-hand side they need (e.g. the loop
// gain uses H_inf, T, beta, H).
std::string low_entropy_latex_rhs(const LowEntropy& le) {
    std::ostringstream os;
    std::string K = to_latex_cdot(le.gain);
    // A factor is wrapped in \left(...\right) when it's a sum (so an additive
    // group stays visually distinct from the product around it). Products get
    // an explicit \cdot between factors (to_latex_cdot) so `Cds_M1 ro_M1`
    // never reads as one term.
    auto is_sum = [](const GiNaC::ex& f) {
        return GiNaC::is_a<GiNaC::add>(f) ||
               GiNaC::is_a<GiNaC::add>(f.expand());
    };
    auto factor_tex = [&](const GiNaC::ex& f) {
        std::string tex = to_latex_cdot(f);
        if (is_sum(f)) return "\\left(" + tex + "\\right)";
        return tex;
    };
    std::string N, D;
    for (const auto& f : le.num_factors) {
        if (!N.empty()) N += "\\cdot ";
        N += factor_tex(f.expr);
    }
    for (const auto& f : le.den_factors) {
        if (!D.empty()) D += "\\cdot ";
        D += factor_tex(f.expr);
    }
    std::string num = K;
    if (!N.empty()) num += (num.empty() ? "" : "\\cdot ") + N;
    if (num.empty()) num = "1";
    if (D.empty())
        os << num;
    else
        os << "\\frac{" << num << "}{" << D << "}";
    return os.str();
}

std::string low_entropy_latex(const LowEntropy& le) {
    return "H(s) = " + low_entropy_latex_rhs(le);
}

// ---------------------------------------------------------------------------
// main entry
// ---------------------------------------------------------------------------
LowEntropy low_entropy(const ex& num, const ex& den, ParamTable& params,
                       const LowEntropyOptions& opts) {
    LowEntropy R;
    ex s = params.get("s");
    R.gain = ex(1);

    // Normalise the *ratio* into one reduced fraction (num and den from MNA
    // carry arbitrary common factors; reducing cancels them).
    ex Hrat = (num / den).normal();
    ex n = Hrat.numer().expand();
    ex d = Hrat.denom().expand();
    if (n.is_zero()) {
        R.gain = ex(0);
        R.text = "0";
        R.text_poly = "0";
        R.latex = "H(s) = 0";
        return R;
    }

    // 1. cancel common s^k
    auto ldeg = [&](const ex& e) -> int {
        if (!e.has(s)) return 0;
        try { return e.ldegree(s); } catch (...) { return 0; }
    };
    int kn = ldeg(n), kd = ldeg(d);
    int common = std::min(kn, kd);
    if (common > 0) {
        n = (n / GiNaC::pow(s, common)).normal();
        d = (d / GiNaC::pow(s, common)).normal();
        kn -= common;
        kd -= common;
    }
    int s_zeros = 0, s_poles = 0;
    if (kn > 0) {
        s_zeros = kn;
        n = (n / GiNaC::pow(s, kn)).normal();
    }
    if (kd > 0) {
        s_poles = kd;
        d = (d / GiNaC::pow(s, kd)).normal();
    }

    // 2. Normalize by the denominator constant term so den(0) == 1. This is
    //    what lets the factored output read as (1 + s*tau1)(1 + s*tau2): the
    //    s-coefficients of the polynomial are not multiplied by the (in
    //    general symbolic) DC gain, and a genuine parallel combination
    //    C1*R1*R2/(R1+R2) collapses to a single (R1||R2)*C1.
    ex c0d = d.coeff(s, 0);
    if (c0d.is_zero()) c0d = d.coeff(s, std::min(kd, 1));
    if (c0d.is_zero()) c0d = ex(1);
    if (opts.normalize && !c0d.is_equal(ex(1))) {
        n = (n / c0d).normal();
        d = (d / c0d).normal();
    }

    // 3. parallel rewrite (moved before magnitude pruning), applied per
    //    s-coefficient, followed by a parallel-aware collapse. The parallel
    //    structure must be recovered BEFORE magnitude pruning: pruning a
    //    series-form `R1 + ro` would drop the small resistor, but that same
    //    expression is the denominator of a parallel `R1||ro = R1*ro/(R1+ro)`,
    //    where the BIG resistor is the negligible one. Recovering `R1||ro`
    //    first lets prune_parallel drop the right term.
    if (opts.use_parallel) {
        auto poly_par = [&](const ex& poly) -> ex {
            int deg = 0;
            try { deg = poly.has(s) ? poly.degree(s) : 0; } catch (...) { deg = 0; }
            if (deg < 0 || deg > 64) return poly;
            ex acc = 0;
            for (int k = 0; k <= deg; ++k) {
                ex c = poly.coeff(s, k);
                if (c.is_zero()) continue;
                c = c.expand();
                // Factor the coefficient so a product denominator such as
                // (R1+R2)*(1+A) is exposed: this lets to_parallel recover an
                // embedded (R1||R2) that an expanded coefficient would hide.
                // Only do this when the coefficient actually has a rational
                // structure (a negative power), to avoid factoring every
                // plain polynomial coefficient (which is slow and needless).
                bool has_den = false;
                if (is_a<GiNaC::mul>(c))
                    for (size_t i = 0; i < c.nops(); ++i) {
                        const ex& op = c.op(i);
                        if (is_a<GiNaC::power>(op) &&
                            is_a<numeric>(op.op(1)) &&
                            GiNaC::ex_to<numeric>(op.op(1)).is_negative()) {
                            has_den = true;
                            break;
                        }
                    }
                if (has_den) c = GiNaC::factor(c);
                if (is_a<GiNaC::add>(c)) {
                    ex cc = 0;
                    for (size_t i = 0; i < c.nops(); ++i)
                        cc += to_parallel(c.op(i));
                    c = cc;
                } else {
                    c = to_parallel(c);
                }
                acc += c * GiNaC::pow(s, k);
            }
            return acc;
        };
        n = poly_par(n);
        d = poly_par(d);
        if (opts.prune) {
            n = prune_parallel(n, params, opts.threshold_db);
            d = prune_parallel(d, params, opts.threshold_db);
            // series reduction is per s-coefficient: the DC "1" term otherwise
            // blocks factoring the common C out of R1*C + R2*C.
            auto poly_series = [&](const ex& poly) -> ex {
                int deg = 0;
                try { deg = poly.has(s) ? poly.degree(s) : 0; } catch (...) { return poly; }
                if (deg < 0 || deg > 64) return poly;
                ex acc = 0;
                for (int k = 0; k <= deg; ++k) {
                    ex c = poly.coeff(s, k);
                    if (c.is_zero()) continue;
                    c = prune_series(c, params, opts.threshold_db);
                    acc += c * GiNaC::pow(s, k);
                }
                return acc;
            };
            n = poly_series(n);
            d = poly_series(d);
        }
    }

    // 4. higher-order term pruning (band ranking). Terms whose magnitude is
    //    far below the dominant one across the sweep are dropped here, but the
    //    pole/zero reduction itself happens later on the factored time
    //    constants (drop_far_factors), so a genuine pole 40 dB away survives.
    if (opts.prune) {
        ex np = n, dp = d;
        prune_poly(np, s, params, opts, "numerator", R.dropped);
        prune_poly(dp, s, params, opts, "denominator", R.dropped);
        if (!dp.expand().is_zero()) {
            n = np;
            d = dp;
        }
    }

    // 5. normalize the denominator again (the rewrite can reintroduce a scale)
    if (opts.normalize) {
        ex c0 = d.coeff(s, 0);
        if (!c0.is_zero() && !c0.is_equal(ex(1))) {
            n = (n / c0).normal();
            d = (d / c0).normal();
        }
    }

    // 5b. Re-reduce the ratio so any common factor the pruning / parallel
    //     rewrite left behind cancels to 1. E.g. an output impedance
    //     R1*ro / (ro + s*R1*ro*Cds) has `ro` in both numerator and
    //     denominator; dividing both by ro yields the simplest form
    //     R1 / (1 + s*R1*Cds). The par() function is a held atom, so this
    //     reduction never expands R1||R2 back out. Reducing a rational ratio
    //     can pull the DC scale back into the denominator, so re-normalize
    //     straight after when normalization is on.
    {
        ex Hred = (n / d).normal();
        ex nr = Hred.numer().expand();
        ex dr = Hred.denom().expand();
        if (!nr.is_zero() && !dr.is_zero()) {
            n = nr;
            d = dr;
        }
        if (opts.normalize) {
            ex c0 = d.coeff(s, 0);
            if (!c0.is_zero() && !c0.is_equal(ex(1))) {
                n = (n / c0).normal();
                d = (d / c0).normal();
            }
        }
    }

    // 5c. Pull common factors out of the numerator/denominator so the printed
    //     form is as compact as possible. Done AFTER the ratio reduction (so
    //     the reduction's .expand() doesn't immediately undo it): the whole-
    //     polynomial factor (x*a + x*b -> x*(a+b)) plus a per-s-coefficient
    //     factor (s*R1*ro*Cds + s*C1*R1*ro -> s*R1*ro*(Cds + C1)).
    //     Each s-coefficient also gets the structural gain reduction, so
    //     A/(GBW*(1 + A)) collapses to 1/GBW when A >> 1 (the "A/(A+1)" rule).
    auto simplify_coeffs = [&](const ex& poly) -> ex {
        int deg = 0;
        try { deg = poly.has(s) ? poly.degree(s) : 0; } catch (...) { return poly; }
        if (deg < 0 || deg > 64) return poly;
        ex acc = 0;
        for (int k = 0; k <= deg; ++k) {
            ex c = poly.coeff(s, k);
            if (c.is_zero()) continue;
            c = simplify_coeff(c, params, opts.threshold_db);
            acc += c * GiNaC::pow(s, k);
        }
        return acc;
    };
    if (opts.prune) {
        ex nf = factor_common_impl(n);
        ex df = factor_common_impl(d);
        if (!nf.is_zero()) n = nf;
        if (!df.is_zero()) d = df;
        n = factor_coeffs(n, s);
        d = factor_coeffs(d, s);
        n = simplify_coeffs(n);
        d = simplify_coeffs(d);
    }

    R.num_poly = n;
    R.den_poly = d;

    // 6. pull out the overall gain K = n(0) so the factors normalize to 1
    ex K = n.coeff(s, 0);
    if (!K.is_equal(ex(1)) && !K.is_zero()) n = (n / K).normal();
    // Reduce the gain's own sums with the structural threshold: A/(A+1) with
    // A = 1e9 becomes 1, but A/(A+1) with A = 1 stays exact.
    if (opts.prune && !K.is_zero())
        K = simplify_gain(K, params, opts.threshold_db);
    // Recover parallel structure in the gain too: -R1*R2*A/(A*(R1+R2)) is
    // -(R1||R2). This is what makes a TIA's gain read -(R1||R2) rather than
    // an expanded resistor ratio.
    if (opts.use_parallel && !K.is_zero()) K = to_parallel(K.normal());
    R.gain = K;

    // 7. origin factors
    if (s_zeros > 0)
        R.num_factors.push_back({GiNaC::pow(s, s_zeros),
                                 pretty(GiNaC::pow(s, s_zeros)), true});
    if (s_poles > 0)
        R.den_factors.push_back({GiNaC::pow(s, s_poles),
                                 pretty(GiNaC::pow(s, s_poles)), true});

    // 8. factor extraction by time-constant matching (TTC style). The
    //    zero-value (open-circuit) time constants computed from the topology
    //    are the physically-correct R*C / L/R products, so they are tried
    //    first: this is what makes a pole read as its actual element (Cgs*R2,
    //    not a numerically-equal C1*R2).
    std::vector<Candidate> cands = build_tau_candidates(params);
    for (auto it = opts.octc.rbegin(); it != opts.octc.rend(); ++it) {
        if (it->tau_value > 0.0 && !it->tau.is_zero())
            cands.insert(cands.begin(),
                         {std::fabs(it->tau_value), it->tau});
    }
    ex dd = d, nn = n;
    bool any_numeric = false;
    peel_factors(dd, cands, params, s, R.den_factors, opts.approx_factor,
                 any_numeric);
    peel_factors(nn, cands, params, s, R.num_factors, opts.approx_factor,
                 any_numeric);
    R.numeric_factors = any_numeric;

    // 8b. drop poles/zeros that are more than 60 dB away in frequency. This is
    //     the pole/zero reduction, done on the time constants (factors) -- not
    //     by chopping terms out of the expanded polynomial, which would leave
    //     an inconsistent polynomial whose numeric re-factoring mis-attributes
    //     the surviving pole (Cgs*R2 -> C1*R2).
    if (opts.prune) {
        drop_far_factors(R.den_factors, params, s, opts.pole_zero_threshold_db,
                         "denominator", R.dropped);
        drop_far_factors(R.num_factors, params, s, opts.pole_zero_threshold_db,
                         "numerator", R.dropped);
        // Rebuild the pruned polynomials from the kept factors so text_poly
        // stays consistent with the dropped (factored) text.
        ex den_poly = ex(1);
        for (const auto& f : R.den_factors)
            if (!f.origin) den_poly = den_poly * f.expr;
        R.den_poly = den_poly.expand();
        ex num_poly = R.gain;
        for (const auto& f : R.num_factors)
            if (!f.origin) num_poly = num_poly * f.expr;
        R.num_poly = num_poly.expand();
    }

    // 9. root tables
    roots_from_factors(R.den_factors, params, s, R.poles);
    roots_from_factors(R.num_factors, params, s, R.zeros);
    // List poles/zeros lowest corner frequency first (the origin, then
    // ascending omega), so a 1e6 pole sorts before a 1e8 pole.
    auto sort_roots = [](std::vector<Root>& v) {
        std::sort(v.begin(), v.end(), [](const Root& a, const Root& b) {
            return a.omega < b.omega;
        });
    };
    sort_roots(R.poles);
    sort_roots(R.zeros);

    // 9. display text (compound numerator/denominator get parentheses)
    std::string Kt = pretty(R.gain);
    std::string Nt = join_factors_pretty(R.num_factors);
    std::string Dt = join_factors_pretty(R.den_factors);
    std::string base;
    if (Kt == "1")
        base = wrap_compound(Nt);
    else if (Nt == "1")
        base = Kt;
    else
        base = Kt + "*" + wrap_compound(Nt);
    R.text = (Dt == "1") ? base : base + " / " + wrap_compound(Dt);
    R.text_poly = pretty_ratio(R.num_poly, R.den_poly, s);

    // 10. LaTeX (factored form)
    R.latex = low_entropy_latex(R);
    return R;
}

} // namespace syms
