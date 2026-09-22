#include "core/Prune.h"
#include "core/Eng.h"
#include "core/Print.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace syms {
using GiNaC::ex;
using GiNaC::exmap;
using GiNaC::is_a;
using GiNaC::numeric;
using GiNaC::pow;

using cplx = std::complex<double>;

// ---------------------------------------------------------------------------
// Numeric evaluation
// ---------------------------------------------------------------------------
std::complex<double> eval_complex(const ex& e, const ParamTable& params,
                                  double omega) {
    exmap m;
    for (const auto& kv : params.est) {
        auto it = params.syms.find(kv.first);
        if (it != params.syms.end()) m[it->second] = ex(kv.second);
    }
    auto sit = params.syms.find("s");
    if (sit != params.syms.end()) m[sit->second] = ex(GiNaC::I) * omega;

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

// ---------------------------------------------------------------------------
// Durand-Kerner root finder
// ---------------------------------------------------------------------------
std::vector<cplx> poly_roots(std::vector<double> c) {
    while (c.size() > 1 && c.back() == 0.0) c.pop_back();
    int n = static_cast<int>(c.size()) - 1;
    std::vector<cplx> roots;
    if (n <= 0) return roots;
    if (n == 1) {
        roots.push_back(cplx(-c[0] / c[1], 0.0));
        return roots;
    }

    std::vector<cplx> a(n + 1);
    for (int k = 0; k <= n; ++k) a[k] = cplx(c[k], 0.0);
    cplx lc = a[n];
    for (auto& v : a) v /= lc;

    auto val = [&](const cplx& x) {
        cplx r(0.0, 0.0);
        for (int k = n; k >= 0; --k) r = r * x + a[k];
        return r;
    };

    std::vector<cplx> z(n);
    cplx p(1.0, 0.0);
    const cplx step(0.4, 0.9);
    for (int j = 0; j < n; ++j) {
        z[j] = p;
        p *= step;
    }

    for (int iter = 0; iter < 800; ++iter) {
        double maxd = 0.0;
        for (int j = 0; j < n; ++j) {
            cplx num = val(z[j]);
            cplx den(1.0, 0.0);
            for (int i = 0; i < n; ++i)
                if (i != j) den *= (z[j] - z[i]);
            if (std::abs(den) < 1e-300) {
                z[j] += cplx(1e-6, 1e-6);
                continue;
            }
            cplx d = num / den;
            z[j] -= d;
            maxd = std::max(maxd, std::abs(d));
        }
        double scale = 1.0;
        for (auto& r : z) scale = std::max(scale, std::abs(r));
        if (maxd < 1e-13 * scale) break;
    }
    return z;
}

// ---------------------------------------------------------------------------
// Time-constant candidates (physical labeling)
// ---------------------------------------------------------------------------
struct Candidate {
    double value;
    ex expr;
};

std::vector<Candidate> build_tau_candidates(const ParamTable& pt) {
    struct Sym { double v; ex e; };
    std::vector<Sym> rs, cs, ls;
    for (const auto& kv : pt.est) {
        auto cit = pt.cls.find(kv.first);
        auto sit = pt.syms.find(kv.first);
        if (cit == pt.cls.end() || sit == pt.syms.end()) continue;
        if (kv.second <= 0.0 || !std::isfinite(kv.second)) continue;
        ex e = sit->second;
        switch (cit->second) {
            case UnitClass::Ohm: rs.push_back({kv.second, e}); break;
            case UnitClass::Siemens: rs.push_back({1.0 / kv.second, pow(e, -1)}); break;
            case UnitClass::Farad: cs.push_back({kv.second, e}); break;
            case UnitClass::Henry: ls.push_back({kv.second, e}); break;
            default: break;
        }
    }
    std::vector<Candidate> out;
    for (const auto& r : rs)
        for (const auto& c : cs) {
            if (out.size() > 4096) return out;
            out.push_back({r.v * c.v, r.e * c.e});
        }
    for (const auto& l : ls)
        for (const auto& r : rs) {
            if (out.size() > 4096) return out;
            out.push_back({l.v / r.v, l.e / r.e});
        }
    return out;
}

// ---------------------------------------------------------------------------
// Term pruning
// ---------------------------------------------------------------------------
struct Entry {
    int k;
    ex term;
    double db;
};

void prune_poly(ex& poly, const ex& s, const ParamTable& pt,
                const PruneOptions& opts, const std::string& what,
                std::vector<DroppedTerm>& dropped) {
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

    double f0 = opts.f0_hz > 1e-12 ? opts.f0_hz : 1e-12;
    double w0 = 2.0 * M_PI * f0;
    double db_per_decade = 20.0 * std::log10(w0);

    std::vector<Entry> entries;
    for (int k = 0; k <= deg; ++k) {
        ex c = pe.coeff(s, k);
        if (c.is_zero()) continue;
        std::vector<ex> terms;
        if (is_a<GiNaC::add>(c))
            for (size_t i = 0; i < c.nops(); ++i) terms.push_back(c.op(i));
        else
            terms.push_back(c);
        for (const ex& t : terms) {
            std::complex<double> z = eval_complex(t, pt, 0.0);
            double db;
            double mag = std::hypot(z.real(), z.imag());
            if (mag > 0.0 && std::isfinite(mag))
                db = 20.0 * std::log10(mag) + k * db_per_decade;
            else
                db = std::numeric_limits<double>::quiet_NaN();
            entries.push_back({k, t, db});
        }
    }
    if (entries.empty()) return;

    // reference magnitude
    auto finite_max = [&](const std::vector<Entry>& es) {
        double m = -std::numeric_limits<double>::infinity();
        for (const auto& e : es)
            if (!std::isnan(e.db)) m = std::max(m, e.db);
        return m;
    };

    std::vector<char> keep(entries.size(), 1);
    std::vector<double> refs(entries.size(), 0.0);

    if (opts.global_ref) {
        double ref = finite_max(entries);
        for (size_t i = 0; i < entries.size(); ++i) {
            refs[i] = ref;
            if (std::isnan(entries[i].db)) continue; // unknowable -> keep
            if (entries[i].db < ref - opts.threshold_db) keep[i] = 0;
        }
    } else {
        // per-coefficient reference
        for (size_t i = 0; i < entries.size(); ++i) {
            std::vector<Entry> group;
            for (const auto& e : entries)
                if (e.k == entries[i].k) group.push_back(e);
            double ref = finite_max(group);
            refs[i] = ref;
            if (std::isnan(entries[i].db)) continue;
            if (entries[i].db < ref - opts.threshold_db) keep[i] = 0;
        }
    }

    // rebuild + record drops
    bool any_keep = false;
    for (char c : keep) any_keep = any_keep || (c != 0);
    if (!any_keep) {
        for (size_t i = 0; i < entries.size(); ++i) keep[i] = 1;
    }

    ex acc = 0;
    std::vector<int> kept_deg;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (!keep[i]) {
            std::string loc = what + ", ";
            if (entries[i].k == 0)
                loc += "constant";
            else if (entries[i].k == 1)
                loc += "s";
            else
                loc += "s^" + std::to_string(entries[i].k);
            double rel = entries[i].db - refs[i];
            dropped.push_back({loc, pretty(entries[i].term),
                               std::isnan(rel) ?0.0 : rel});
            continue;
        }
        acc += entries[i].term * pow(s, entries[i].k);
    }
    poly = acc;
}

// ---------------------------------------------------------------------------
// Symbolic factor peeling
// ---------------------------------------------------------------------------
std::string loc_deg(const ex& term, const ex& s) { (void)term; (void)s; return ""; }

// poly must have constant term == 1 on entry (except when degree 0).
void peel_linear(ex& poly, const std::vector<Candidate>& cands, ParamTable& pt,
                 const ex& s, std::vector<FactorInfo>& factors) {
    while (true) {
        int deg;
        try {
            deg = poly.degree(s);
        } catch (...) {
            return;
        }
        if (deg <= 0) return;

        if (deg == 1) {
            // exact, always: (c0 + c1*s), c0 == 1
            factors.push_back({poly, pretty_in_s(poly, s), false});
            poly = ex(1);
            return;
        }

        // numeric roots for candidate ordering
        std::vector<double> coeffs;
        bool all_real = true;
        for (int k = 0; k <= deg; ++k) {
            ex v = pt.eval_real(poly.coeff(s, k));
            double d = 0.0;
            if (is_a<numeric>(v))
                d = GiNaC::ex_to<numeric>(v).to_double();
            else
                all_real = false;
            coeffs.push_back(d);
        }
        std::vector<cplx> roots = all_real ? poly_roots(coeffs) : std::vector<cplx>{};

        // order candidates by closeness to a real root tau = -1/root
        struct Ranked { double score; const Candidate* c; };
        std::vector<Ranked> ranked;
        for (const auto& cand : cands) {
            double best = std::numeric_limits<double>::infinity();
            for (const cplx& r : roots) {
                if (std::abs(r.imag()) > 1e-6 * (1.0 + std::abs(r.real()))) continue;
                double target = -1.0 / r.real();
                if (target <= 0.0 || cand.value <= 0.0) continue;
                best = std::min(best, std::fabs(std::log(cand.value / target)));
            }
            if (best <= std::log(1.15)) // within ~15%
                ranked.push_back({best, &cand});
        }
        std::sort(ranked.begin(), ranked.end(),
                  [](const Ranked& a, const Ranked& b) { return a.score < b.score; });

        bool peeled = false;
        const size_t max_try = 8;
        size_t tried = 0;
        for (const Ranked& rk : ranked) {
            if (++tried > max_try && rk.score > std::log(1.02)) break;
            ex f = ex(1) + rk.c->expr * s;
            ex q = (poly / f).normal();
            ex rem = (poly - q * f).normal();
            if (rem.is_zero()) {
                factors.push_back({f, pretty_in_s(f, s), false});
                poly = q.expand();
                peeled = true;
                break;
            }
        }
        if (!peeled) {
            // leave the remaining (normalized) polynomial as one honest factor
            factors.push_back({poly, pretty_in_s(poly, s), false});
            poly = ex(1);
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// Root tables
// ---------------------------------------------------------------------------
void roots_from_factors(const std::vector<FactorInfo>& factors, ParamTable& pt,
                        const ex& s, bool is_pole,
                        std::vector<RootInfo>& out) {
    for (const auto& f : factors) {
        if (f.origin_s) {
            RootInfo r;
            r.omega = 0.0;
            r.tau = 0.0;
            r.real_root = true;
            r.label = "origin";
            r.factor_text = f.text;
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
            RootInfo r;
            r.real_root = true;
            r.factor_text = f.text;
            if (is_a<numeric>(tau_n)) r.tau = GiNaC::ex_to<numeric>(tau_n).to_double();
            if (r.tau != 0.0) r.omega = 1.0 / r.tau;
            if (is_a<GiNaC::mul>(tau_ex)) r.label = pretty(tau_ex);
            out.push_back(r);
        } else if (deg == 2) {
            ex a = f.expr.coeff(s, 2); // tau_n^2
            ex b = f.expr.coeff(s, 1);
            ex an = pt.eval_real(a);
            ex bn = pt.eval_real(b);
            RootInfo r;
            r.real_root = false;
            r.factor_text = f.text;
            if (is_a<numeric>(an) && is_a<numeric>(bn)) {
                double av = GiNaC::ex_to<numeric>(an).to_double();
                double bv = GiNaC::ex_to<numeric>(bn).to_double();
                if (av > 0.0) {
                    r.tau = std::sqrt(av);       // tau = 1/wn
                    r.omega = 1.0 / r.tau;
                    if (bv != 0.0) r.q = r.tau / bv * 1.0 / r.tau * r.tau; // sqrt(a)/b
                    r.q = std::sqrt(av) / bv;
                }
            }
            if (is_a<GiNaC::mul>(a)) r.label = pretty(a);
            out.push_back(r);
        } else if (deg > 2) {
            // numeric roots, unlabeled
            std::vector<double> coeffs;
            bool okall = true;
            for (int k = 0; k <= deg; ++k) {
                ex v = pt.eval_real(f.expr.coeff(s, k));
                if (!is_a<numeric>(v)) { okall = false; break; }
                coeffs.push_back(GiNaC::ex_to<numeric>(v).to_double());
            }
            if (!okall) continue;
            auto rr = poly_roots(coeffs);
            for (const cplx& z : rr) {
                RootInfo r;
                r.factor_text = f.text;
                if (std::abs(z.imag()) < 1e-6 * (1.0 + std::abs(z.real()))) {
                    r.real_root = true;
                    r.tau = -1.0 / z.real();
                    r.omega = r.tau != 0.0 ? 1.0 / r.tau : 0.0;
                } else {
                    r.real_root = false;
                    r.omega = std::abs(z);
                    r.q = std::abs(z) / (2.0 * std::abs(z.imag()));
                }
                out.push_back(r);
            }
        }
    }
    (void)is_pole;
}

std::string join_factors(const std::vector<FactorInfo>& fs) {
    std::string out;
    for (const auto& f : fs) {
        if (f.text == "1" && !out.empty()) continue;
        if (out.empty())
            out = f.text;
        else
            out += "\xC2\xB7" + f.text;
    }
    return out.empty() ? "1" : out;
}

std::string wrap_if_compound(const std::string& t) {
    if (t == "1") return t;
    bool compound = t.find("\xC2\xB7") != std::string::npos ||
                    t.find('+') != std::string::npos ||
                    t.find('-') != std::string::npos ||
                    t.find('/') != std::string::npos;
    return compound ? "(" + t + ")" : t;
}

// ---------------------------------------------------------------------------
// Main entry
// ---------------------------------------------------------------------------
Pruned prune_low_entropy(const ex& num, const ex& den, ParamTable& params,
                         const PruneOptions& opts) {
    Pruned R;
    ex s = params.get("s");

    R.gain = ex(1);
    R.num_poly = ex(0);
    R.den_poly = ex(1);
    R.text = "0";
    R.text_poly = "0";

    ex n = num, d = den;
    if (n.expand().is_zero()) {
        R.gain = ex(0);
        R.text = "0";
        R.text_poly = "0";
        return R;
    }

    // 1. cancel common s^k, extract origin factors
    auto ldeg = [&](const ex& e) -> int {
        if (!e.has(s)) return 0;
        try { return e.ldegree(s); } catch (...) { return 0; }
    };
    int kn = ldeg(n), kd = ldeg(d);
    int c = std::min(kn, kd);
    if (c > 0) {
        n = (n / pow(s, c)).normal();
        d = (d / pow(s, c)).normal();
        kn -= c;
        kd -= c;
    }
    int s_zeros = 0, s_poles = 0;
    if (kn > 0) {
        s_zeros = kn;
        n = (n / pow(s, kn)).normal();
    }
    if (kd > 0) {
        s_poles = kd;
        d = (d / pow(s, kd)).normal();
    }

    // 2. normalize denominator constant term to 1
    ex c0d = d.coeff(s, 0);
    if (c0d.is_zero()) {
        // pathological: leave as-is
        R.num_poly = n;
        R.den_poly = d;
        R.text = pretty_ratio(n, d, s);
        R.text_poly = R.text;
        return R;
    }
    n = (n / c0d).normal();
    d = (d / c0d).normal();

    // 3. rank + drop negligible terms -- after normalization so the kept
    //    polynomial and the dropped-term labels match the printed form
    ex n_pre = n, d_pre = d;
    prune_poly(n, s, params, opts, "numerator", R.dropped);
    prune_poly(d, s, params, opts, "denominator", R.dropped);
    if (d.expand().is_zero()) {
        // never happens with sane inputs; fall back to unpruned denominator
        d = d_pre;
        R.dropped.clear();
        n = n_pre;
        prune_poly(n, s, params, opts, "numerator", R.dropped);
    }

    // polynomials for the expanded display form (num still carries the gain)
    R.num_poly = n;
    R.den_poly = d;

    // 4. extract gain K = H(0)
    ex K = n.coeff(s, 0);
    if (!K.is_equal(ex(1)) && !K.is_zero()) n = (n / K).normal();
    R.gain = K;

    // 5. origin factors
    if (s_zeros > 0) {
        ex f = pow(s, s_zeros);
        R.num_factors.push_back({f, pretty(f), true});
    }
    if (s_poles > 0) {
        ex f = pow(s, s_poles);
        R.den_factors.push_back({f, pretty(f), true});
    }

    // 6. peel symbolic (1 + s*tau) factors
    std::vector<Candidate> cands = build_tau_candidates(params);
    peel_linear(d, cands, params, s, R.den_factors);
    peel_linear(n, cands, params, s, R.num_factors);

    // 7. root tables
    roots_from_factors(R.den_factors, params, s, true, R.poles);
    roots_from_factors(R.num_factors, params, s, false, R.zeros);

    // 8. display text
    std::string Kt = pretty(R.gain);
    std::string Nt = join_factors(R.num_factors);
    std::string Dt = join_factors(R.den_factors);

    std::string base;
    if (Kt == "1")
        base = Nt;
    else if (Nt == "1")
        base = Kt;
    else
        base = Kt + "\xC2\xB7" + Nt;
    if (Dt == "1")
        R.text = base;
    else
        R.text = base + " / " + wrap_if_compound(Dt);

    R.text_poly = pretty_ratio(R.num_poly, R.den_poly, s);
    return R;
}

} // namespace syms
