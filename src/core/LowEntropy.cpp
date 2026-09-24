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

    ex v = e.subs(m).evalf();
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
ex gm_ro_idealize(const ex& e, const ParamTable& pt);

// Exact polynomial divisibility of `num` (a polynomial in `s`) by `fac`.
// GiNaC's ex::degree() drops rational coefficients, so (a+b)*x/y and similar
// rational coefficients are *not* seen as polynomials; clear the denominator
// first, then ask GiNaC for the remainder.
bool poly_remainder_is_zero(const ex& num, const ex& fac, const ex& s) {
    if (fac.is_zero()) return false;
    ex q = (num / fac).normal();
    ex numc = q.numer().expand(); // polynomial part
    ex denc = q.denom().expand(); // symbolic denominator (may be 1)
    try {
        ex r = GiNaC::rem(numc, fac, s);
        if (!r.is_zero()) return false;
        if (!denc.is_equal(ex(1)) && !denc.is_zero()) {
            ex r2 = GiNaC::rem(denc, fac, s);
            if (!r2.is_zero()) return false;
        }
        return true;
    } catch (...) {
        return false;
    }
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
    if (o.global_ref) {
        double ref = finite_max(entries);
        for (size_t i = 0; i < entries.size(); ++i) {
            refs[i] = ref;
            if (std::isnan(entries[i].db)) continue;
            if (entries[i].db < ref - o.threshold_db) keep[i] = 0;
        }
    } else {
        for (size_t i = 0; i < entries.size(); ++i) {
            std::vector<Entry> group;
            for (const auto& e : entries)
                if (e.k == entries[i].k) group.push_back(e);
            double ref = finite_max(group);
            refs[i] = ref;
            if (std::isnan(entries[i].db)) continue;
            if (entries[i].db < ref - o.threshold_db) keep[i] = 0;
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
    for (const auto& kv : pt.est) {
        auto cit = pt.cls.find(kv.first);
        auto sit = pt.syms.find(kv.first);
        if (cit == pt.cls.end() || sit == pt.syms.end()) continue;
        if (kv.second <= 0.0 || !std::isfinite(kv.second)) continue;
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
            if (is_a<numeric>(tau_n)) r.tau = GiNaC::ex_to<numeric>(tau_n).to_double();
            if (r.tau != 0.0) {
                r.omega = 1.0 / r.tau;
                r.f_hz = r.omega / (2.0 * M_PI);
            }
            if (is_a<GiNaC::mul>(tau_ex) || is_a<GiNaC::add>(tau_ex) ||
                is_parallel(tau_ex)) {
                // factor common terms so a pole reads as "(Rd||ro)*(Cgd+CL)"
                ex lab = factor_common_impl(tau_ex);
                r.label = pretty(lab);
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

// gm*ro >> 1 idealization pass: inside a sum, if exactly one term is a
// gm*ro product (or a multiple of one) and it dominates, drop the rest.
// This is deliberately conservative -- it only fires on sums that contain an
// explicit gm*ro term, so ordinary polynomials are untouched.
bool looks_like_gm_ro(const ex& e, const ParamTable& pt) {
    // The term must contain at least one gm-class and one ro-class symbol --
    // that's the structural signature of "a transconductance times an output
    // resistance", regardless of magnitude. The numerical gate below is a
    // separate check on whether this term actually dominates its siblings.
    bool has_gm = false, has_ro = false;
    for (const auto& kv : pt.cls) {
        auto sit = pt.syms.find(kv.first);
        if (sit == pt.syms.end()) continue;
        if (!e.has(sit->second)) continue;
        if (kv.second == UnitClass::Siemens) has_gm = true;
        if (kv.second == UnitClass::Ohm) has_ro = true;
    }
    return has_gm && has_ro;
}

// Magnitude helper: |e| at s = 0 with the user's parameter estimates, with a
// floor to keep log10 sane when a summand has zero numeric value (a bare
// parameter or a term that simplifies to 0).
static double num_mag(const ex& e, const ParamTable& pt) {
    std::complex<double> z = eval_complex(e, pt, 0.0);
    double mag = std::hypot(z.real(), z.imag());
    return mag > 0.0 ? mag : 1e-30;
}

ex gm_ro_idealize(const ex& e, const ParamTable& pt) {
    if (is_a<GiNaC::add>(e)) {
        // recurse first
        GiNaC::exvector ops;
        for (size_t i = 0; i < e.nops(); ++i)
            ops.push_back(gm_ro_idealize(e.op(i), pt));
        ex sum = GiNaC::add(ops);
        // find gm*ro-like terms; if exactly one, drop the rest provided it
        // dominates them by a wide margin (a factor of ~30 = 30 dB, the same
        // magnitude the magnitude-pruning pass uses).
        std::vector<ex> terms;
        if (is_a<GiNaC::add>(sum))
            for (size_t i = 0; i < sum.nops(); ++i) terms.push_back(sum.op(i));
        else
            terms.push_back(sum);
        ex dom;
        int ndom = 0;
        for (const ex& t : terms)
            if (looks_like_gm_ro(t, pt)) { dom = t; ++ndom; }
        if (ndom == 1) {
            double d = num_mag(dom, pt);
            bool dominates = true;
            for (const ex& t : terms) {
                if (t.is_equal(dom)) continue;
                if (num_mag(t, pt) > d / 31.6) { dominates = false; break; }
            }
            if (dominates) return dom;
        }
        return sum;
    }
    if (is_a<GiNaC::mul>(e) || is_a<GiNaC::power>(e)) {
        GiNaC::exvector ops;
        for (size_t i = 0; i < e.nops(); ++i)
            ops.push_back(gm_ro_idealize(e.op(i), pt));
        return is_a<GiNaC::mul>(e) ? ex(GiNaC::mul(ops))
                                   : ex(GiNaC::pow(ops[0], ops[1]));
    }
    return e;
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

// Wrap a factor's text in parentheses when it is a sum or a ratio, so a
// product of factors prints unambiguously: "(a+b)" not "a+b".
std::string paren_factor(const std::string& t) {
    if (t == "1") return t;
    bool sum = t.find('+') != std::string::npos ||
               t.find(" - ") != std::string::npos;
    bool ratio = t.find('/') != std::string::npos;
    bool product = t.find("\xC2\xB7") != std::string::npos;
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
            out += "\xC2\xB7" + t;
    }
    return out.empty() ? "1" : out;
}

std::string wrap_compound(const std::string& t) {
    if (t == "1") return t;
    if (already_wrapped(t)) return t;
    bool compound = t.find("\xC2\xB7") != std::string::npos ||
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

std::string low_entropy_latex(const LowEntropy& le) {
    std::ostringstream os;
    os << "H(s) = ";
    std::string K = to_latex(le.gain);
    std::string N, D;
    for (const auto& f : le.num_factors) {
        if (!N.empty()) N += "\\,";
        N += "\\left(" + to_latex(f.expr) + "\\right)";
    }
    for (const auto& f : le.den_factors) {
        if (!D.empty()) D += "\\,";
        D += "\\left(" + to_latex(f.expr) + "\\right)";
    }
    std::string num = K;
    if (!N.empty()) num += (num.empty() ? "" : "\\,") + N;
    if (num.empty()) num = "1";
    if (D.empty())
        os << num;
    else
        os << "\\frac{" << num << "}{" << D << "}";
    return os.str();
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

    // 3. magnitude pruning (optional)
    if (opts.prune) {
        ex np = n, dp = d;
        prune_poly(np, s, params, opts, "numerator", R.dropped);
        prune_poly(dp, s, params, opts, "denominator", R.dropped);
        if (!dp.expand().is_zero()) {
            n = np;
            d = dp;
        }
    }

    // 4. parallel rewrite, applied per s-coefficient. Coefficients are clean
    //    rationals where Rd*ro/(Rd+ro) appears as a whole, so matching there
    //    is reliable (matching the raw ratio would false-positive).
    if (opts.use_parallel) {
        auto poly_par = [&](const ex& poly) -> ex {
            int deg = 0;
            try { deg = poly.has(s) ? poly.degree(s) : 0; } catch (...) { deg = 0; }
            if (deg < 0 || deg > 64) return poly;
            ex acc = 0;
            for (int k = 0; k <= deg; ++k) {
                ex c = poly.coeff(s, k);
                if (c.is_zero()) continue;
                // expand so (a+b)*x/y becomes a*x/y + b*x/y, letting each
                // term be matched for the parallel pattern independently
                c = c.expand();
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
    }

    // 5. normalize the denominator again (the rewrite can reintroduce a scale)
    if (opts.normalize) {
        ex c0 = d.coeff(s, 0);
        if (!c0.is_zero() && !c0.is_equal(ex(1))) {
            n = (n / c0).normal();
            d = (d / c0).normal();
        }
    }

    // 4b. gm*ro >> 1 idealization, then pull common factors out of the
    //     numerator/denominator so the printed form is as compact as possible
    //     (e.g. x*a + x*b -> x*(a+b)).
    if (opts.prune && opts.gm_ro_assume) {
        n = gm_ro_idealize(n, params);
        d = gm_ro_idealize(d, params);
    }
    if (opts.prune) {
        ex nf = factor_common_impl(n);
        ex df = factor_common_impl(d);
        if (!nf.is_zero()) n = nf;
        if (!df.is_zero()) d = df;
    }

    R.num_poly = n;
    R.den_poly = d;

    // 6. pull out the overall gain K = n(0) so the factors normalize to 1
    ex K = n.coeff(s, 0);
    if (!K.is_equal(ex(1)) && !K.is_zero()) n = (n / K).normal();
    R.gain = K;

    // 7. origin factors
    if (s_zeros > 0)
        R.num_factors.push_back({GiNaC::pow(s, s_zeros),
                                 pretty(GiNaC::pow(s, s_zeros)), true});
    if (s_poles > 0)
        R.den_factors.push_back({GiNaC::pow(s, s_poles),
                                 pretty(GiNaC::pow(s, s_poles)), true});

    // 8. factor extraction by time-constant matching (TTC style)
    std::vector<Candidate> cands = build_tau_candidates(params);
    ex dd = d, nn = n;
    bool any_numeric = false;
    peel_factors(dd, cands, params, s, R.den_factors, opts.approx_factor,
                 any_numeric);
    peel_factors(nn, cands, params, s, R.num_factors, opts.approx_factor,
                 any_numeric);
    R.numeric_factors = any_numeric;

    // 9. root tables
    roots_from_factors(R.den_factors, params, s, R.poles);
    roots_from_factors(R.num_factors, params, s, R.zeros);

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
        base = Kt + "\xC2\xB7" + wrap_compound(Nt);
    R.text = (Dt == "1") ? base : base + " / " + wrap_compound(Dt);
    R.text_poly = pretty_ratio(R.num_poly, R.den_poly, s);

    // 10. LaTeX (factored form)
    R.latex = low_entropy_latex(R);
    return R;
}

} // namespace syms
