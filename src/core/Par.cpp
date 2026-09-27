#include "core/Par.h"
#include "core/ParamTable.h"

#include <algorithm>
#include <functional>
#include <sstream>

namespace syms {

using GiNaC::ex;
using GiNaC::is_a;
using GiNaC::numeric;

namespace {

DECLARE_FUNCTION_2P(par)
DECLARE_FUNCTION_2P(ser)

// ser_eval: a *held sum*. A series combination of two same-class passives is
// printed and kept as (a+b) -- never expanded -- so that a parallel partner can
// bind to it as one operand (R1||(R2+R3)). Folds to a number when both
// arguments are numeric. Unlike par(), the operand order is preserved so the
// sum reads in the order the user drew the chain (R2+R3, not R3+R2).
ex ser_eval(const ex& a, const ex& b) {
    if (is_a<numeric>(a) && is_a<numeric>(b)) return a + b;
    return ser(a, b).hold();
}

ex ser_evalf(const ex& a, const ex& b) {
    ex va = a.evalf(), vb = b.evalf();
    if (is_a<numeric>(va) && is_a<numeric>(vb)) return va + vb;
    return ser(va, vb).hold();
}

// The plain printer prints the held sum with its parentheses; the LaTeX printer
// likewise, so `R1||(R3+R2)` is unambiguous in both views.
void ser_print(const ex& a, const ex& b, const GiNaC::print_context& c) {
    c.s << "(";
    a.print(c);
    c.s << "+";
    b.print(c);
    c.s << ")";
}

void ser_print_latex(const ex& a, const ex& b, const GiNaC::print_context& c) {
    c.s << "\\left(";
    a.print(c);
    c.s << "+";
    b.print(c);
    c.s << "\\right)";
}

// par_eval: fold when both arguments are numeric (or equal), otherwise stay
// symbolic so that R1||R2 survives into the output. The arguments are
// canonicalised (ordered) so that par(a,b) and par(b,a) are the *same* held
// atom: this matters both for display consistency and for expression
// comparison (e.g. grouping noise sources that share a transfer).
ex par_eval(const ex& a, const ex& b) {
    if (a.is_equal(b)) return a / 2;
    if (is_a<numeric>(a) && is_a<numeric>(b)) {
        numeric na = GiNaC::ex_to<numeric>(a), nb = GiNaC::ex_to<numeric>(b);
        if (na.is_zero() || nb.is_zero()) return ex(0);
        return (a * b) / (a + b);
    }
    // Order the two arguments by GiNaC's canonical comparison so the atom is
    // commutative. compare() is the ordering GiNaC itself uses for sums.
    if (a.compare(b) > 0) return par(b, a).hold();
    return par(a, b).hold();
}

ex par_evalf(const ex& a, const ex& b) {
    ex va = a.evalf(), vb = b.evalf();
    if (is_a<numeric>(va) && is_a<numeric>(vb)) return (va * vb) / (va + vb);
    return par(va, vb).hold();
}

void par_print(const ex& a, const ex& b, const GiNaC::print_context& c) {
    // Parenthesise an operand that is itself a sum: `R1||R3+R2` reads as
    // `(R1||R3)+R2`, so a held series group like R2+R3 must print as
    // `R1||(R3+R2)`.
    auto op_pr = [&](const ex& o) {
        if (is_a<GiNaC::add>(o)) {
            c.s << "(";
            o.print(c);
            c.s << ")";
        } else {
            o.print(c);
        }
    };
    c.s << "(";
    op_pr(a);
    c.s << "||";
    op_pr(b);
    c.s << ")";
}

// LaTeX form of the parallel operator. The parentheses matter: a bare
// `a\parallel b` sitting inside a product like `Cgd_M1 R1\parallel ro_M1 s`
// reads ambiguously (is it (R1∥ro_M1), or Cgd_M1·R1 ∥ ro_M1·s?). The
// plain-text printer has always parenthesised (`(R1||ro_M1)`); the LaTeX
// printer must do the same so the two views agree. A sum operand is also
// wrapped, so `R1\parallel (R3+R2)` does not read as `(R1\parallel R3)+R2`.
void par_print_latex(const ex& a, const ex& b, const GiNaC::print_context& c) {
    auto op_pr = [&](const ex& o) {
        if (is_a<GiNaC::add>(o)) {
            c.s << "\\left(";
            o.print(c);
            c.s << "\\right)";
        } else {
            o.print(c);
        }
    };
    c.s << "\\left(";
    op_pr(a);
    c.s << "\\parallel ";
    op_pr(b);
    c.s << "\\right)";
}

// ---------------------------------------------------------------------------
// Pattern extraction:  k * a * b / (a + b)  ->  k * par(a, b)
// ---------------------------------------------------------------------------
// Given a product's numerator factor list and denominator factors, return
// par(a,b) when some additive denominator factor (a+b) has both its operands
// present among the numerator factors: k*a*b/[(a+b)*rest] -> k*par(a,b)/rest.
// The `rest` handles the common case where the a+b sits inside a larger
// product such as (R1+R2)*(1+A) (a TIA's closed-loop pole).
//
// NOTE: this only ever fires on an atom that is *already* split; the primary
// low-entropy guarantee is that a series group is formed and held upstream (at
// the MNA stamp), so a parallel partner like R1 combines with (R2+R3) and is
// never expanded in the first place.
bool extract_from_product(const ex& prod, ex& out_gain) {
    // collect multiplicative factors (flattening)
    std::vector<ex> nums, dens;
    ex lead = ex(1);
    std::function<void(const ex&)> walk = [&](const ex& f) {
        if (is_a<GiNaC::mul>(f)) {
            for (size_t i = 0; i < f.nops(); ++i) walk(f.op(i));
            return;
        }
        if (is_a<GiNaC::power>(f)) {
            const ex& base = f.op(0);
            const ex& xp = f.op(1);
            if (is_a<numeric>(xp) && GiNaC::ex_to<numeric>(xp).is_negative()) {
                dens.push_back(GiNaC::pow(base, -xp));
                return;
            }
        }
        if (is_a<numeric>(f)) {
            lead = lead * f;
            return;
        }
        nums.push_back(f);
    };
    walk(prod);

    // Try each additive denominator factor for a parallel match. The factor
    // may have more than two terms, in which case we split it into a two-way
    // partition and accept when BOTH halves appear literally as numerator
    // factors: R1*(R2+R3)/(R1+R2+R3) -> R1||(R2+R3). (A 2-term factor is the
    // m==2 case of the same loop.)
    for (size_t di = 0; di < dens.size(); ++di) {
        const ex& den = dens[di];
        if (!is_a<GiNaC::add>(den)) continue;
        int m = int(den.nops());
        if (m < 2 || m > 12) continue;
        for (int mask = 1; mask < (1 << m) - 1; ++mask) {
            ex a = 0, b = 0;
            for (int i = 0; i < m; ++i) {
                if (mask & (1 << i)) a += den.op(i);
                else b += den.op(i);
            }
            auto has = [&](const ex& want) {
                for (const ex& n : nums)
                    if (n.is_equal(want)) return true;
                return false;
            };
            if (!has(a) || !has(b)) continue;
            // remove one a and one b from the numerator factors
            ex rest = lead;
            bool ra = false, rb = false;
            for (const ex& n : nums) {
                if (!ra && n.is_equal(a)) { ra = true; continue; }
                if (!rb && n.is_equal(b)) { rb = true; continue; }
                rest = rest * n;
            }
            ex out = rest * par(a, b);
            for (size_t dj = 0; dj < dens.size(); ++dj)
                if (dj != di) out = out / dens[dj];
            out_gain = out.normal();
            return true;
        }
    }
    return false;
}

ex rewrite(const ex& e) {
    if (is_parallel(e)) return e;

    if (is_a<GiNaC::mul>(e)) {
        // recurse into factors, then try to pull a parallel factor out
        GiNaC::exvector ops;
        for (size_t i = 0; i < e.nops(); ++i) ops.push_back(rewrite(e.op(i)));
        ex prod = ex(GiNaC::mul(ops)).normal();
        ex gain;
        if (extract_from_product(prod, gain)) return rewrite(gain);
        return prod;
    }
    if (is_a<GiNaC::add>(e)) {
        GiNaC::exvector ops;
        for (size_t i = 0; i < e.nops(); ++i) ops.push_back(rewrite(e.op(i)));
        return GiNaC::add(ops);
    }
    if (is_a<GiNaC::power>(e)) {
        ex b = rewrite(e.op(0));
        return GiNaC::pow(b, e.op(1));
    }
    return e;
}

} // namespace

REGISTER_FUNCTION(par, eval_func(par_eval)
                           .evalf_func(par_evalf)
                           .print_func<GiNaC::print_context>(par_print)
                           .print_func<GiNaC::print_latex>(par_print_latex)
                           .latex_name("\\parallel"));

REGISTER_FUNCTION(ser, eval_func(ser_eval)
                           .evalf_func(ser_evalf)
                           .print_func<GiNaC::print_context>(ser_print)
                           .print_func<GiNaC::print_latex>(ser_print_latex)
                           .latex_name("+"));

GiNaC::ex par_ex(const ex& a, const ex& b) { return par(a, b); }

GiNaC::ex ser_ex(const ex& a, const ex& b) { return ser(a, b); }

bool is_parallel(const ex& e) {
    return is_a<GiNaC::function>(e) &&
           GiNaC::ex_to<GiNaC::function>(e).get_name() == "par";
}

bool is_series(const ex& e) {
    return is_a<GiNaC::function>(e) &&
           GiNaC::ex_to<GiNaC::function>(e).get_name() == "ser";
}

std::vector<ex> series_args(const ex& e) {
    std::vector<ex> out;
    if (!is_series(e)) return out;
    for (size_t i = 0; i < e.nops(); ++i) out.push_back(e.op(i));
    return out;
}

ex make_series(const std::vector<ex>& args) {
    if (args.empty()) return ex(0);
    if (args.size() == 1) return args[0];
    ex acc = args[0];
    for (size_t i = 1; i < args.size(); ++i) acc = ser(acc, args[i]);
    return acc;
}

std::vector<ex> parallel_args(const ex& e) {
    std::vector<ex> out;
    if (!is_parallel(e)) return out;
    for (size_t i = 0; i < e.nops(); ++i) out.push_back(e.op(i));
    return out;
}

ex make_parallel(const std::vector<ex>& args) {
    if (args.empty()) return ex(0);
    if (args.size() == 1) return args[0];
    ex acc = args[0];
    for (size_t i = 1; i < args.size(); ++i) acc = par(acc, args[i]);
    return acc;
}

ex to_parallel(const ex& e) { return rewrite(e); }

// Numerical value of a parallel expression, substituting the estimates given
// by the ParamTable.
double par_value(const ex& e, const ParamTable& pt) {
    if (is_parallel(e)) {
        std::vector<ex> a = parallel_args(e);
        if (a.size() != 2) return 0.0;
        double x = par_value(a[0], pt), y = par_value(a[1], pt);
        if (x == 0.0 || y == 0.0) return 0.0;
        return x * y / (x + y);
    }
    ex v = pt.eval_real(e);
    if (is_a<numeric>(v)) return GiNaC::ex_to<numeric>(v).to_double();
    return 0.0;
}

} // namespace syms
