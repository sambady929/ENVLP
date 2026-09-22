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

// par_eval: fold when both arguments are numeric (or equal), otherwise stay
// symbolic so that R1||R2 survives into the output.
ex par_eval(const ex& a, const ex& b) {
    if (a.is_equal(b)) return a / 2;
    if (is_a<numeric>(a) && is_a<numeric>(b)) {
        numeric na = GiNaC::ex_to<numeric>(a), nb = GiNaC::ex_to<numeric>(b);
        if (na.is_zero() || nb.is_zero()) return ex(0);
        return (a * b) / (a + b);
    }
    return par(a, b).hold();
}

ex par_evalf(const ex& a, const ex& b) {
    ex va = a.evalf(), vb = b.evalf();
    if (is_a<numeric>(va) && is_a<numeric>(vb)) return (va * vb) / (va + vb);
    return par(va, vb).hold();
}

void par_print(const ex& a, const ex& b, const GiNaC::print_context& c) {
    c.s << "(";
    a.print(c);
    c.s << "||";
    b.print(c);
    c.s << ")";
}

void par_print_latex(const ex& a, const ex& b, const GiNaC::print_context& c) {
    a.print(c);
    c.s << "\\parallel ";
    b.print(c);
}

// ---------------------------------------------------------------------------
// Pattern extraction:  k * a * b / (a + b)  ->  k * par(a, b)
// ---------------------------------------------------------------------------
// Given a product's numerator factor list and a single two-term denominator,
// return par(a,b) if both denominator terms are present in the numerator.
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

    if (dens.size() != 1) return false;
    const ex& den = dens[0];
    if (!is_a<GiNaC::add>(den) || den.nops() != 2) return false;
    ex d1 = den.op(0), d2 = den.op(1);

    // both denominator terms must appear among the numerator factors
    auto take = [&](const ex& want) -> bool {
        for (size_t i = 0; i < nums.size(); ++i) {
            if (nums[i].is_equal(want)) {
                nums.erase(nums.begin() + i);
                return true;
            }
        }
        return false;
    };
    if (!take(d1) || !take(d2)) return false;

    ex gain = lead * par(d1, d2);
    for (const ex& f : nums) gain = gain * f;
    out_gain = gain;
    return true;
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

GiNaC::ex par_ex(const ex& a, const ex& b) { return par(a, b); }

bool is_parallel(const ex& e) {
    return is_a<GiNaC::function>(e) &&
           GiNaC::ex_to<GiNaC::function>(e).get_name() == "par";
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
