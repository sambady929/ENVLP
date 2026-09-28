#include "core/Print.h"

#include <cmath>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

namespace syms {
namespace {

using GiNaC::ex;
using GiNaC::is_a;
using GiNaC::numeric;

// Pull a common multiplicative factor out of a sum of products:
// a*x + a*y -> a*(x+y). Mirrors LowEntropy.cpp's factor_common_impl so the
// pretty-printer preserves the factored form the engine produced (a bare
// `expand()` in pretty_in_s would otherwise undo it). Only polynomial factors
// are extracted -- never a factor that lives in a denominator.
ex factor_sum(const ex& e) {
    if (!is_a<GiNaC::add>(e)) return e;
    std::vector<std::vector<ex>> terms;
    for (size_t i = 0; i < e.nops(); ++i) {
        std::vector<ex> fs;
        const ex& t = e.op(i);
        if (is_a<GiNaC::mul>(t))
            for (size_t j = 0; j < t.nops(); ++j) fs.push_back(t.op(j));
        else
            fs.push_back(t);
        terms.push_back(fs);
    }
    if (terms.empty()) return e;
    std::vector<ex> common;
    for (const ex& f : terms[0]) {
        if (is_a<GiNaC::power>(f)) {
            const ex& xp = f.op(1);
            if (is_a<numeric>(xp) && GiNaC::ex_to<numeric>(xp).is_negative())
                continue;
        }
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
    ex cpart = ex(1);
    for (const ex& f : common) cpart = cpart * f;
    ex rest = (e / cpart).normal();
    if (!is_a<GiNaC::add>(rest)) return e;
    return cpart * rest;
}

// Multiplication sign in plain-text output. This is the *text* path, so use
// an ASCII `*` -- no Unicode, no LaTeX. The Math tab gets the real
// multiplication dot from the separate LaTeX string (pruned.latex), so the
// two views stay independent.
constexpr const char* kDot = "*";

// Superscript in plain text: `s^2`. The LaTeX form (built separately in
// LowEntropy.cpp) uses `s^{2}` for proper typesetting.
std::string sup(int k) {
    return "^" + std::to_string(k);
}

// GiNaC's default stream output ("print_dflt") as a string.
std::string ginac_str(const ex& e) {
    std::ostringstream os;
    os << e;
    return os.str();
}

std::string fmt_number(const numeric& n) {
    if (n.is_integer()) return ginac_str(n);
    // Keep an exact rational as a fraction ("4/5", not "0.8"): the low-entropy
    // reports are meant to stay exact, matching the LaTeX \frac.
    if (n.is_rational() && !n.is_integer()) {
        return ginac_str(n.numer()) + "/" + ginac_str(n.denom());
    }
    double d = n.to_double();
    char buf[64];
    if (d == std::floor(d) && std::fabs(d) < 1e15)
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(d));
    else
        std::snprintf(buf, sizeof(buf), "%.6g", d);
    return buf;
}

// prec: 0 = top level, 1 = term of a sum, 2 = operand of a product /
//       parenthesized, 3 = power base / strict operand
std::string p(const ex& e, int prec) {
    if (is_a<numeric>(e)) return fmt_number(GiNaC::ex_to<numeric>(e));
    if (is_a<GiNaC::symbol>(e)) return GiNaC::ex_to<GiNaC::symbol>(e).get_name();

    if (is_a<GiNaC::add>(e)) {
        std::string out;
        for (size_t i = 0; i < e.nops(); ++i) {
            std::string ts = p(e.op(i), 1);
            bool neg = !ts.empty() && ts[0] == '-';
            if (neg) ts.erase(0, 1);
            if (i == 0)
                out += (neg ? "-" : "") + ts;
            else
                out += (neg ? " - " : " + ") + ts;
        }
        if (out.empty()) out = "0";
        if (prec >= 2) out = "(" + out + ")";
        return out;
    }

    if (is_a<GiNaC::mul>(e)) {
        // sqrt(2)*sqrt(A)/sqrt(B) -> sqrt(2*A/B): GiNaC splits a radical of a
        // product/quotient, which reads as a chain of separate square roots.
        // Merge a positive numeric radical and both half-power directions back
        // under one root for display.
        {
            ex numpart = 1, numrad = 1, denrad = 1;
            bool all_sqrt = true;
            for (size_t i = 0; i < e.nops() && all_sqrt; ++i) {
                const ex& f = e.op(i);
                if (is_a<numeric>(f)) { numpart = numpart * f; continue; }
                if (is_a<GiNaC::power>(f) && is_a<numeric>(f.op(1))) {
                    double x = GiNaC::ex_to<numeric>(f.op(1)).to_double();
                    if (x == 0.5) { numrad = numrad * f.op(0); continue; }
                    if (x == -0.5) { denrad = denrad * f.op(0); continue; }
                }
                all_sqrt = false;
            }
            if (all_sqrt && (!numrad.is_equal(ex(1)) ||
                             !denrad.is_equal(ex(1)))) {
                ex merged = (numpart * numpart * numrad / denrad).normal();
                ex outside, inside;
                split_radical(merged, outside, inside);
                std::string out;
                if (inside.is_equal(ex(1)))
                    out = p(outside, 0);
                else if (outside.is_equal(ex(1)))
                    out = "sqrt(" + p(inside, 0) + ")";
                else
                    out = p(outside, 3) + kDot + "sqrt(" + p(inside, 0) + ")";
                if (prec >= 3) out = "(" + out + ")";
                return out;
            }
        }
        bool negative = false;
        std::string coeff;
        std::vector<ex> nums, dens;

        for (size_t i = 0; i < e.nops(); ++i) {
            const ex& f = e.op(i);
            if (is_a<numeric>(f)) {
                numeric n = GiNaC::ex_to<numeric>(f);
                if (n.is_negative()) {
                    negative = !negative;
                    n = -n;
                }
                if (!n.is_equal(numeric(1))) coeff = ginac_str(n);
            } else if (is_a<GiNaC::power>(f)) {
                const ex& xp = f.op(1);
                if (is_a<numeric>(xp) && GiNaC::ex_to<numeric>(xp).is_negative())
                    dens.push_back(GiNaC::pow(f.op(0), -xp));
                else
                    nums.push_back(f);
            } else {
                nums.push_back(f);
            }
        }

        std::string num_str = coeff;
        for (const ex& f : nums) {
            std::string fs = p(f, 3);
            if (num_str.empty())
                num_str = fs;
            else
                num_str += kDot + fs; // factors joined by \cdot
        }
        if (num_str.empty()) num_str = "1";

        std::string den_str;
        for (const ex& f : dens) {
            // A single additive denominator factor is already parenthesised by
            // p(f, 2); a negative power like (R1+ro)^-1 arrives here as the
            // base R1+ro, so p(f,2) wraps it. Join multiple factors (each
            // already wrapped when additive) with '*'.
            std::string fs = p(f, is_a<GiNaC::add>(f) ? 2 : 0);
            if (den_str.empty())
                den_str = fs;
            else
                den_str += kDot + fs;
        }

        std::string out = negative ? "-" : "";
        out += num_str;
        if (!den_str.empty()) {
            // A single denominator factor is already wrapped by p(.,2) when it
            // is additive; more than one factor is joined by '*' and needs one
            // outer wrap so `a/(X*Y)` is not parsed as `(a/X)*Y`.
            out += "/" + (dens.size() > 1 ? "(" + den_str + ")" : den_str);
        }
        if (prec >= 3) out = "(" + out + ")";
        return out;
    }

    if (is_a<GiNaC::power>(e)) {
        const ex& b = e.op(0);
        const ex& xp = e.op(1);
        if (is_a<numeric>(xp)) {
            numeric xn = GiNaC::ex_to<numeric>(xp);
            if (xn.is_negative()) {
                ex pos = GiNaC::pow(b, -xn);
                std::string ps = p(pos, 0);
                bool wrap = is_a<GiNaC::add>(pos) || is_a<GiNaC::mul>(pos);
                return "1/" + (wrap ? "(" + ps + ")" : ps);
            }
            if (xn.is_integer()) {
                long k = xn.to_int();
                if (k == 1) return p(b, prec);
                if (k >= 2 && k <= 9) {
                    std::string bs = p(b, 3);
                    return bs + sup(static_cast<int>(k));
                }
                return p(b, 3) + "^" + std::to_string(k);
            }
            if (xn.to_double() == 0.5) return "sqrt(" + p(b, 0) + ")";
            return p(b, 3) + "^(" + p(xp, 0) + ")";
        }
        return p(b, 3) + "^(" + p(xp, 0) + ")";
    }

    // functions (abs, sin, ...) and anything exotic: GiNaC default text
    return ginac_str(e);
}

} // namespace

// Largest a with a^2 | n (n a positive integer), and the square-free leftover.
static void int_square_part(long n, long& a, long& r) {
    a = 1;
    if (n <= 0) { r = 1; return; }
    for (long k = 2; k * k <= n; ++k) {
        while (n % (k * k) == 0) { a *= k; n /= (k * k); }
    }
    r = n;
}

void split_radical(const ex& X, ex& outside, ex& radicand) {
    // Split X = numer/denom into a rational coefficient times powers, then move
    // every perfect-square part (even exponent, and the square factor of the
    // integer coefficients) to the outside.
    ex num = X.numer(), den = X.denom();
    long coeff_n = 1, coeff_d = 1;
    ex sn = 1, sd = 1, inn = 1, ind = 1; // symbolic outside/inside, num/den

    auto take = [&](ex Y, long& coeff, ex& sout, ex& sin) {
        if (is_a<numeric>(Y)) {
            numeric ny = GiNaC::ex_to<numeric>(Y);
            if (ny.is_integer()) {
                long n = 0;
                try { n = ny.to_long(); } catch (...) { n = 0; }
                if (n > 0) coeff *= n;
            }
            return;
        }
        for (size_t i = 0; i < Y.nops(); ++i) {
            const ex& f = Y.op(i);
            if (is_a<numeric>(f)) {
                numeric ny = GiNaC::ex_to<numeric>(f);
                if (ny.is_integer()) {
                    long n = 0;
                    try { n = ny.to_long(); } catch (...) { n = 0; }
                    if (n > 0) coeff *= n;
                }
                continue;
            }
            if (is_a<GiNaC::power>(f) && is_a<numeric>(f.op(1))) {
                long e = 0;
                try { e = GiNaC::ex_to<numeric>(f.op(1)).to_int(); }
                catch (...) { e = 0; }
                ex base = f.op(0);
                if (e % 2 == 0) { sout = sout * GiNaC::pow(base, e / 2); continue; }
                sout = sout * GiNaC::pow(base, (e - 1) / 2);
                sin = sin * base;
                continue;
            }
            sin = sin * f; // a non-power factor stays inside
        }
    };
    take(num, coeff_n, sn, inn);
    take(den, coeff_d, sd, ind);

    long a_n, r_n, a_d, r_d;
    int_square_part(coeff_n, a_n, r_n);
    int_square_part(coeff_d, a_d, r_d);

    outside = (ex(a_n) * sn / (ex(a_d) * sd)).normal();
    radicand = (ex(r_n) * inn / (ex(r_d) * ind)).normal();
}

std::string pretty(const ex& e) { return p(e, 0); }

std::string pretty_factor(const ex& e) {
    std::string s = p(e, 0);
    if (is_a<GiNaC::add>(e)) return "(" + s + ")";
    if (is_a<GiNaC::mul>(e) && !s.empty() && s[0] == '-') return "(" + s + ")";
    return s;
}

std::string pretty_product(const std::vector<ex>& factors) {
    std::string out;
    for (const ex& f : factors) {
        std::string fs = pretty_factor(f);
        if (fs == "1" && !out.empty()) continue;
        if (out.empty())
            out = fs;
        else
            out += kDot + fs;
    }
    return out.empty() ? "1" : out;
}

std::string pretty_in_s(const ex& e, const ex& s) {
    ex pe = e.expand();
    if (!pe.has(s)) return p(pe, 0);

    int deg = 0;
    try {
        deg = pe.degree(s);
    } catch (...) {
        return p(pe, 0);
    }
    if (deg < 0 || deg > 64) return p(pe, 0);

    std::string out;
    bool first = true;
    // ascending powers: constant term first reads as "1 + s*R1*C1"
    for (int k = 0; k <= deg; ++k) {
        ex c = pe.coeff(s, k);
        if (c.is_zero()) continue;
        // Factor common terms out of the coefficient so s*(R1*ro*Cds +
        // C1*R1*ro) reads s*R1*ro*(Cds + C1) instead of the expanded sum.
        c = factor_sum(c);

        std::string body;
        bool neg = false;
        if (k == 0) {
            std::string cs = p(c, 1);
            neg = !cs.empty() && cs[0] == '-';
            if (neg) cs.erase(0, 1);
            body = cs;
        } else {
            std::string cs = p(c, 2);
            neg = !cs.empty() && cs[0] == '-';
            if (neg) cs.erase(0, 1);
            // the power belongs to s, not to the coefficient:
            //   k=1 -> "s*R1*C1",  k=2 -> "s^2*R1*C1"
            std::string spow = (k == 1) ? "s" : ("s" + sup(k));
            if (cs == "1")
                body = spow;
            else
                body = spow + kDot + cs;
        }
        if (body.empty()) continue;
        if (first) {
            out += (neg ? "-" : "") + body;
            first = false;
        } else {
            out += (neg ? " - " : " + ") + body;
        }
    }
    return out.empty() ? "0" : out;
}

std::string pretty_ratio(const ex& num, const ex& den, const ex& s) {
    auto wrapped = [](const std::string& t) {
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
    };
    ex nd = den.expand();
    std::string ds = pretty_in_s(den, s);
    if (is_a<GiNaC::add>(nd) || is_a<GiNaC::mul>(nd)) ds = "(" + ds + ")";

    std::string ns = pretty_in_s(num, s);
    // A sum numerator must be parenthesized, otherwise "a + b / d" would be
    // read as "a + (b/d)" rather than "(a+b)/d".
    ex nn = num.expand();
    if (is_a<GiNaC::add>(nn) && !wrapped(ns)) ns = "(" + ns + ")";
    return ns + " / " + ds;
}

} // namespace syms
