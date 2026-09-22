#pragma once
#include <ginac/ginac.h>

// CLN's As() macro corrupts wxWidgets' wxAny::As<T>(); undo it.
#ifdef As
#undef As
#endif

#include <string>
#include <vector>

namespace syms {

struct ParamTable;

// Symbolic "parallel" combination, printed as (a||b).
//   par(a,b) = a*b/(a+b)
// It stays symbolic (so R1||R2 is preserved in low-entropy output) and folds
// to a number when both arguments are numeric.
GiNaC::ex par_ex(const GiNaC::ex& a, const GiNaC::ex& b);

// Is `e` a parallel() node?
bool is_parallel(const GiNaC::ex& e);
std::vector<GiNaC::ex> parallel_args(const GiNaC::ex& e);
GiNaC::ex make_parallel(const std::vector<GiNaC::ex>& args);

// Recursively rewrite a*b/(a+b) into par(a,b). Used to turn raw MNA ratios
// into R1||R2 form.
GiNaC::ex to_parallel(const GiNaC::ex& e);

// Numerical value of a parallel expression using the parameter estimates.
double par_value(const GiNaC::ex& e, const ParamTable& pt);

} // namespace syms
