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

std::string ginac_str(const ex& e) {
    std::ostringstream os;
    os << e;
    return os.str();
}

} // namespace

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
    double f0 = o.f0_hz > 1e-12 ? o.f0_hz : 1e-12;
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
            double mag = std::hypot(z.real(), z.imag());
            double db = (mag > 0.0 && std::isfinite(mag))
                            ? 20.0 * std::log10(mag) + k * db_per_decade
                            : std::numeric_limits<double>::quiet_NaN();
            entries.push_back({k, t, db});
        }
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
        if (out.size() > 4096) return;
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
    return out;
}

// ---------------------------------------------------------------------------
// symbolic factor peeling
// ---------------------------------------------------------------------------
void peel_factors(ex& poly, const std::vector<Candidate>& cands,
                  ParamTable& pt, const ex& s, std::vector<Factor>& factors) {
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
            factors.push_back({poly, pretty_in_s(poly, s), false});
            poly = ex(1);
            return;
        }
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

// Idealization pass: with gm*ro >> 1 the "+1" next to a gm*ro product is
// negligible. Rewrite sums where one term is (gm*ro)-like and another is
// small: (gm*ro + x) -> gm*ro. Only applies to sums, never products.
// gm*ro >> 1 idealization pass: inside a sum, if exactly one term is a
// gm*ro product (or a multiple of one) and it dominates, drop the rest.
// This is deliberately conservative -- it only fires on sums that contain an
// explicit gm*ro term, so ordinary polynomials are untouched.
bool looks_like_gm_ro(const ex& e, const ParamTable& pt) {
    // numeric value must be >> 1
    std::complex<double> z = eval_complex(e, pt, 0.0);
    double mag = std::hypot(z.real(), z.imag());
    if (!(mag > 100.0)) return false;
    // and it must contain a gm symbol (Siemens) and an ro symbol (Ohm)
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

ex gm_ro_idealize(const ex& e, const ParamTable& pt) {
    if (is_a<GiNaC::add>(e)) {
        // recurse first
        GiNaC::exvector ops;
        for (size_t i = 0; i < e.nops(); ++i)
            ops.push_back(gm_ro_idealize(e.op(i), pt));
        ex sum = GiNaC::add(ops);
        // find gm*ro-like terms
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
            // drop the other terms (they are the "+1" of gm*ro+1)
            return dom;
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
    // common factors: present in every term (compare structurally)
    std::vector<ex> common;
    for (const ex& f : terms[0]) {
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

std::string join_factors(const std::vector<Factor>& fs) {
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

std::string wrap_compound(const std::string& t) {
    if (t == "1") return t;
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

    // 2. normalize by the denominator constant term so den(0) == 1
    ex c0d = d.coeff(s, 0);
    if (c0d.is_zero()) c0d = d.coeff(s, std::min(kd, 1));
    if (c0d.is_zero()) c0d = ex(1);
    n = (n / c0d).normal();
    d = (d / c0d).normal();

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
    ex c0 = d.coeff(s, 0);
    if (!c0.is_zero() && !c0.is_equal(ex(1))) {
        n = (n / c0).normal();
        d = (d / c0).normal();
    }

    // 4b. gm*ro >> 1 idealization, then pull common factors out of the
    //     numerator/denominator so the printed form is as compact as possible
    //     (e.g. x*a + x*b -> x*(a+b)).
    if (opts.prune) {
        n = gm_ro_idealize(n, params);
        d = gm_ro_idealize(d, params);
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
    peel_factors(dd, cands, params, s, R.den_factors);
    peel_factors(nn, cands, params, s, R.num_factors);

    // 9. root tables
    roots_from_factors(R.den_factors, params, s, R.poles);
    roots_from_factors(R.num_factors, params, s, R.zeros);

    // 9. display text (compound numerator/denominator get parentheses)
    std::string Kt = pretty(R.gain);
    std::string Nt = join_factors(R.num_factors);
    std::string Dt = join_factors(R.den_factors);
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
